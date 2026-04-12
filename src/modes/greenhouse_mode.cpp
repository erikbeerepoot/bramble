#include "greenhouse_mode.h"

#include <cstring>

#include "pico/unique_id.h"

#include "hardware/watchdog.h"

#include "../board/board_pins.h"
#include "../hal/flash.h"
#include "../hal/logger.h"
#include "../hal/rtc_compat.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../util/time.h"
#include "../version.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;

static uint64_t getDeviceId()
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    uint64_t id = 0;
    for (int i = 0; i < 8; i++) {
        id = (id << 8) | board_id.id[i];
    }
    return id;
}
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;  // 60 seconds

static Logger logger("GREEN");
static Logger pmu_logger("PMU");

void GreenhouseMode::onStart()
{
    logger.info("=== GREENHOUSE MODE ACTIVE ===");
    logger.info("- Roll-up curtain controller");
    logger.info("- Mains powered (no sleep)");
    logger.info("- Orange LED blink (init) -> Green short blink (operational)");

    // Initialize curtain controller with motor GPIOs
    curtain_controller_.initialize(Board::PIN_CURTAIN_OPEN, Board::PIN_CURTAIN_CLOSE);

    // Load saved travel time from flash
    Flash *flash = new Flash();
    curtain_config_ = new CurtainConfigManager(*flash);
    uint32_t saved_travel_time = 0;
    if (curtain_config_->loadTravelTime(saved_travel_time) && saved_travel_time > 0) {
        curtain_controller_.setTravelTime(saved_travel_time);
        logger.info("Restored travel time: %lu ms", saved_travel_time);
    } else {
        logger.info("No saved travel time — calibration needed");
    }

    // Initialize PMU client for RTC timekeeping
    pmu_client_ =
        new PmuClient(Board::PMU_UART_PORT, Board::PMU_UART_TX_PIN, Board::PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        pmu_logger.info("PMU client initialized successfully");

        // Register wake notification callback (plumbing for future scheduled curtain operations)
        pmu_client_->getProtocol().onWakeNotification(
            [](PMU::WakeReason reason, const PMU::ScheduleEntry *entry, bool, const uint8_t *) {
                if (reason == PMU::WakeReason::Scheduled && entry) {
                    pmu_logger.info("Scheduled wake: hour=%d min=%d duration=%ds", entry->hour,
                                    entry->minute, entry->duration);
                    // Future: trigger curtain open/close based on schedule
                }
            });

        // Register schedule complete callback (plumbing for future)
        pmu_client_->getProtocol().onScheduleComplete(
            []() { pmu_logger.info("Schedule complete notification received"); });

        // Try to get time from PMU's battery-backed RTC
        // Wake preamble needed in case PMU is in STOP mode
        pmu_logger.info("Requesting datetime from PMU...");
        pmu_client_->sendWakePreamble();
        pmu_client_->getProtocol().getDateTime([this](bool valid, const PMU::DateTime &datetime) {
            if (valid) {
                pmu_logger.info("PMU has valid time: 20%02d-%02d-%02d %02d:%02d:%02d",
                                datetime.year, datetime.month, datetime.day, datetime.hour,
                                datetime.minute, datetime.second);

                datetime_t dt = bramble::util::time::toDatetimeT(datetime);
                if (rtc_set_datetime(&dt)) {
                    sleep_us(64);
                    Logger::syncSubsecondCounter();
                    updateStateMachine();
                    logger.info("RTC synced from PMU");
                    switchToOperationalPattern();
                } else {
                    logger.error("Failed to set RTC from PMU time");
                }
            } else {
                pmu_logger.info("PMU time not valid - waiting for hub sync");
            }
        });

        // Send CTS to transition PMU from AWAITING_CTS to WAKE_ACTIVE
        // Wake preamble needed in case PMU is in STOP mode (SLEEPING)
        pmu_logger.info("Sending ClearToSend to PMU...");
        pmu_client_->sendWakePreamble();
        pmu_client_->getProtocol().clearToSend([](bool success, PMU::ErrorCode) {
            if (success) {
                pmu_logger.info("ClearToSend acknowledged - PMU in WAKE_ACTIVE");
            } else {
                pmu_logger.error("ClearToSend failed");
            }
        });
    } else {
        logger.warn("PMU client not available - running without RTC backup");
    }

    // Try to detect CHT832X temperature/humidity sensor on I2C
    sensor_ =
        std::make_unique<CHT832X>(Board::SENSOR_I2C_PORT, Board::PIN_I2C_SDA, Board::PIN_I2C_SCL);
    if (sensor_->init()) {
        sensor_available_ = true;
        logger.info("CHT832X sensor detected — enabling temperature/humidity sensing");

        BatchTransmitter::Config tx_config = {.max_batches_per_cycle = 1,
                                              .hub_address = HUB_ADDRESS};
        transmitter_ = std::make_unique<BatchTransmitter>(messenger_, tx_config);

        constexpr uint32_t SENSOR_READ_INTERVAL_MS = 60000;  // 60 seconds
        task_manager_.addTask([this](uint32_t time) { readAndTransmitSensorData(time); },
                              SENSOR_READ_INTERVAL_MS, "Sensor read+transmit");
    } else {
        logger.info("No CHT832X sensor detected — running as actuator only");
        sensor_.reset();
    }

    // Orange blinking while waiting for RTC sync
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);
    // Green short blink when operational
    operational_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 0, 255, 0);

    // Send initial heartbeat for time sync
    logger.info("Sending initial heartbeat for time sync...");
    uint64_t device_id = getDeviceId();
    uint32_t uptime = 0;
    uint8_t battery_level = 255;  // 255 = external/mains power
    uint8_t signal_strength = 0;
    uint8_t active_sensors =
        CAP_VALVE_CONTROL | (sensor_available_ ? (CAP_TEMPERATURE | CAP_HUMIDITY) : 0);
    uint8_t error_flags = 0;
    messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength, active_sensors,
                             error_flags, 0, device_id);

    // Periodic heartbeat
    task_manager_.addTask(
        [this, device_id](uint32_t time) {
            uint32_t uptime = time / 1000;
            uint8_t battery_level = 255;  // Mains powered
            uint8_t signal_strength = 0;
            uint8_t error_flags = 0;
            uint8_t active_sensors =
                CAP_VALVE_CONTROL | (sensor_available_ ? (CAP_TEMPERATURE | CAP_HUMIDITY) : 0);

            if (curtain_controller_.isMotorRunning()) {
                logger.info("Heartbeat: curtain %s, position ~%.0f%%",
                            CurtainController::stateName(curtain_controller_.getState()),
                            curtain_controller_.getEstimatedPosition() * 100.0f);
            }

            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength,
                                     active_sensors, error_flags, 0, device_id);
        },
        HEARTBEAT_INTERVAL_MS, "Heartbeat");

    // Periodic CTS keepalive: keeps PMU in WAKE_ACTIVE (resets 120s timeout)
    // and recovers from PMU resets (CTS works from any PMU state -> WAKE_ACTIVE)
    if (pmu_available_) {
        constexpr uint32_t PMU_CTS_KEEPALIVE_INTERVAL_MS = 10000;  // Debug: 10s for testing
        task_manager_.addTask(
            [this](uint32_t) {
                // Wake preamble needed in case PMU fell back to STOP mode
                pmu_logger.info("Sending periodic CTS keepalive...");
                pmu_client_->sendWakePreamble();
                pmu_client_->getProtocol().clearToSend([](bool success, PMU::ErrorCode) {
                    if (success) {
                        pmu_logger.info("CTS keepalive acknowledged");
                    } else {
                        pmu_logger.error("CTS keepalive failed");
                    }
                });
            },
            PMU_CTS_KEEPALIVE_INTERVAL_MS, "PMU CTS keepalive");
        pmu_logger.info("Registered CTS keepalive task (interval=%lums)",
                        PMU_CTS_KEEPALIVE_INTERVAL_MS);
    }

    greenhouse_state_.markInitialized();
    updateGreenhouseState();
}

void GreenhouseMode::onLoop()
{
    // Non-blocking motor control check
    curtain_controller_.update();

    // Check if calibration just completed — persist and send event
    if (curtain_controller_.calibrationJustCompleted()) {
        uint32_t travel_time = curtain_controller_.getTravelTime();
        logger.info("Calibration result: %lu ms — saving to flash", travel_time);

        if (curtain_config_) {
            curtain_config_->saveTravelTime(travel_time);
        }

        // Send calibration complete event with travel time as detail
        uint8_t detail[4];
        detail[0] = static_cast<uint8_t>(travel_time & 0xFF);
        detail[1] = static_cast<uint8_t>((travel_time >> 8) & 0xFF);
        detail[2] = static_cast<uint8_t>((travel_time >> 16) & 0xFF);
        detail[3] = static_cast<uint8_t>((travel_time >> 24) & 0xFF);
        messenger_.sendEvent(HUB_ADDRESS, EVENT_CALIBRATION_COMPLETE, detail, 4);
    }

    // Process PMU messages
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }

    // Update state machine if motor state may have changed
    updateGreenhouseState();
}

void GreenhouseMode::onModeSpecificUpdate(const UpdateAvailablePayload *payload,
                                          uint8_t hub_sequence)
{
    UpdateType update_type = static_cast<UpdateType>(payload->update_type);

    switch (update_type) {
        case UpdateType::ACTUATOR_COMMAND: {
            const uint8_t *data = payload->payload_data;
            ActuatorPayload actuator;
            actuator.actuator_type = data[0];
            actuator.command = data[1];
            actuator.param_length = data[2];
            memset(actuator.params, 0, sizeof(actuator.params));
            if (actuator.param_length > 0 && actuator.param_length <= sizeof(actuator.params)) {
                memcpy(actuator.params, &data[3], actuator.param_length);
            }

            logger.info("  ACTUATOR_COMMAND: type=%u, cmd=%u, params=%u", actuator.actuator_type,
                        actuator.command, actuator.param_length);

            onActuatorCommand(&actuator);
            onUpdateApplied(hub_sequence);
            break;
        }

        default:
            logger.error("Unknown update type %d", payload->update_type);
            onUpdateFailed();
            break;
    }
}

void GreenhouseMode::onActuatorCommand(const ActuatorPayload *payload)
{
    if (!payload) {
        logger.error("NULL actuator payload");
        return;
    }

    if (payload->actuator_type != ACTUATOR_CURTAIN) {
        logger.warn("Unsupported actuator type %d", payload->actuator_type);
        return;
    }

    uint16_t event_code = 0;

    switch (payload->command) {
        case CMD_TURN_ON:
            logger.info("Command: OPEN curtain");
            curtain_controller_.open();
            event_code = EVENT_CURTAIN_OPENING;
            break;

        case CMD_TURN_OFF:
            logger.info("Command: CLOSE curtain");
            curtain_controller_.close();
            event_code = EVENT_CURTAIN_CLOSING;
            break;

        case CMD_STOP:
            logger.info("Command: STOP curtain");
            curtain_controller_.stop();
            event_code = EVENT_CURTAIN_STOPPED;
            break;

        case CMD_CALIBRATE:
            logger.info("Command: CALIBRATE");
            curtain_controller_.startCalibration();
            break;

        default:
            logger.error("Unknown curtain command %d", payload->command);
            break;
    }

    // Send event notification to hub (best effort)
    if (event_code != 0) {
        messenger_.sendEvent(HUB_ADDRESS, event_code, nullptr, 0);
    }

    updateGreenhouseState();
}

void GreenhouseMode::onHeartbeatResponse(const HeartbeatResponsePayload *payload)
{
    // Base class updates RP2040 RTC
    ApplicationMode::onHeartbeatResponse(payload);

    // Also sync time to PMU if available
    if (pmu_available_ && pmu_client_ && payload) {
        PMU::DateTime datetime(payload->year % 100, payload->month, payload->day, payload->dotw,
                               payload->hour, payload->min, payload->sec);

        pmu_logger.info("Syncing time to PMU: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

        pmu_client_->getProtocol().setDateTime(datetime, [](bool success, PMU::ErrorCode error) {
            if (success) {
                pmu_logger.info("PMU time sync successful");
            } else {
                pmu_logger.error("PMU time sync failed: error %d", static_cast<int>(error));
            }
        });
    }

    // Pull any pending data updates (actuator commands, wake interval, schedules).
    // Only CHECK_UPDATES for flags that carry data — REREGISTER, REBOOT, and
    // FACTORY_RESET are handled by their own protocol flows.
    constexpr uint8_t DATA_FLAGS =
        PENDING_FLAG_SCHEDULE | PENDING_FLAG_WAKE_INTERVAL | PENDING_FLAG_ACTUATOR;
    if (payload && (payload->pending_update_flags & DATA_FLAGS)) {
        logger.info("Pending update flags: 0x%02X, sending CHECK_UPDATES",
                    payload->pending_update_flags);
        sendCheckUpdates();
    }

    updateGreenhouseState();
}

void GreenhouseMode::onRebootRequested()
{
    // Stop motor before rebooting
    if (curtain_controller_.isMotorRunning()) {
        logger.warn("Stopping motor before reboot");
        curtain_controller_.stop();
    }

    if (pmu_available_ && pmu_client_) {
        logger.warn("Requesting full system reset via PMU");
        PMU::Protocol &protocol = pmu_client_->getProtocol();
        uint8_t seq = protocol.getNextSequenceNumber();
        protocol.sendCommand(seq, PMU::Command::SystemReset, nullptr, 0);
        sleep_ms(100);
    } else {
        logger.warn("PMU not available - performing RP2040-only watchdog reboot");
    }
    watchdog_reboot(0, 0, 0);
}

void GreenhouseMode::onFactoryResetRequested()
{
    // Stop motor before reset
    if (curtain_controller_.isMotorRunning()) {
        logger.warn("Stopping motor before factory reset");
        curtain_controller_.stop();
    }

    if (pmu_available_ && pmu_client_) {
        logger.warn("Requesting factory reset via PMU (wipes FRAM)");
        PMU::Protocol &protocol = pmu_client_->getProtocol();
        uint8_t seq = protocol.getNextSequenceNumber();
        protocol.sendCommand(seq, PMU::Command::FactoryReset, nullptr, 0);
        sleep_ms(100);
    } else {
        logger.warn("PMU not available - performing RP2040-only watchdog reboot");
    }
    watchdog_reboot(0, 0, 0);
}

void GreenhouseMode::readAndTransmitSensorData(uint32_t /* current_time */)
{
    if (!sensor_ || !transmitter_) {
        return;
    }

    CHT832X::Reading reading = sensor_->read();
    if (!reading.valid) {
        logger.error("Sensor read failed");
        return;
    }

    uint32_t unix_timestamp = getUnixTimestamp();
    if (unix_timestamp == 0) {
        logger.warn("No RTC time — skipping sensor transmit");
        return;
    }

    int16_t temp_fixed = static_cast<int16_t>(reading.temperature * 100.0f);
    uint16_t hum_fixed = static_cast<uint16_t>(reading.humidity * 100.0f);

    logger.info("Sensor: %.2fC, %.1f%%RH (ts=%lu)", reading.temperature, reading.humidity,
                unix_timestamp);

    current_reading_ = {.timestamp = unix_timestamp,
                        .temperature = temp_fixed,
                        .humidity = hum_fixed,
                        .flags = 0,
                        .transmission_status = RECORD_NOT_TRANSMITTED,
                        .crc16 = 0};

    transmitter_->resetCycleCounter();
    if (!transmitter_->transmit(&current_reading_, 1, [](bool success) {
            Logger log("GREEN");
            if (success) {
                log.info("Sensor data transmitted successfully");
            } else {
                log.warn("Sensor data transmit failed");
            }
        })) {
        logger.error("Failed to initiate sensor transmit");
    }
}

void GreenhouseMode::updateGreenhouseState()
{
    GreenhouseHardwareState hardware_state;
    hardware_state.rtc_running = rtc_running();
    hardware_state.opening = (curtain_controller_.getState() == CurtainState::OPENING);
    hardware_state.closing = (curtain_controller_.getState() == CurtainState::CLOSING);
    hardware_state.calibrating = (curtain_controller_.getState() == CurtainState::CALIBRATING);
    greenhouse_state_.update(hardware_state);
}
