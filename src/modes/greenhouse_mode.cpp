#include "greenhouse_mode.h"

#include "hardware/watchdog.h"

#include "../board/board_pins.h"
#include "../hal/logger.h"
#include "../hal/rtc_compat.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../util/time.h"
#include "../version.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
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

    // Initialize PMU client for RTC timekeeping
    pmu_client_ =
        new PmuClient(Board::PMU_UART_PORT, Board::PMU_UART_TX_PIN, Board::PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        pmu_logger.info("PMU client initialized successfully");

        // Try to get time from PMU's battery-backed RTC
        pmu_logger.info("Requesting datetime from PMU...");
        pmu_client_->getProtocol().getDateTime([this](bool valid, const PMU::DateTime &datetime) {
            if (valid) {
                pmu_logger.info("PMU has valid time: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                                datetime.month, datetime.day, datetime.hour, datetime.minute,
                                datetime.second);

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
    } else {
        logger.warn("PMU client not available - running without RTC backup");
    }

    // Orange blinking while waiting for RTC sync
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);
    // Green short blink when operational
    operational_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 0, 255, 0);

    // Send initial heartbeat for time sync
    logger.info("Sending initial heartbeat for time sync...");
    uint32_t uptime = 0;
    uint8_t battery_level = 255;  // 255 = external/mains power
    uint8_t signal_strength = 0;
    uint8_t active_sensors = CAP_VALVE_CONTROL;
    uint8_t error_flags = 0;
    messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength, active_sensors,
                             error_flags);

    // Periodic heartbeat
    task_manager_.addTask(
        [this](uint32_t time) {
            uint32_t uptime = time / 1000;
            uint8_t battery_level = 255;  // Mains powered
            uint8_t signal_strength = 0;
            uint8_t error_flags = 0;
            uint8_t active_sensors = CAP_VALVE_CONTROL;

            if (curtain_controller_.isMotorRunning()) {
                logger.info("Heartbeat: curtain %s, position ~%.0f%%",
                            CurtainController::stateName(curtain_controller_.getState()),
                            curtain_controller_.getEstimatedPosition() * 100.0f);
            }

            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength,
                                     active_sensors, error_flags);
        },
        HEARTBEAT_INTERVAL_MS, "Heartbeat");

    greenhouse_state_.markInitialized();
    updateGreenhouseState();
}

void GreenhouseMode::onLoop()
{
    // Non-blocking motor control check
    curtain_controller_.update();

    // Process PMU messages
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }

    // Update state machine if motor state may have changed
    updateGreenhouseState();
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

    switch (payload->command) {
        case CMD_TURN_ON:
            logger.info("Command: OPEN curtain");
            curtain_controller_.open();
            break;

        case CMD_TURN_OFF:
            logger.info("Command: CLOSE curtain");
            curtain_controller_.close();
            break;

        case CMD_STOP:
            logger.info("Command: STOP curtain");
            curtain_controller_.stop();
            break;

        case CMD_CALIBRATE:
            logger.info("Command: CALIBRATE (not yet implemented)");
            break;

        default:
            logger.error("Unknown curtain command %d", payload->command);
            break;
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

        pmu_client_->getProtocol().setDateTime(
            datetime, [](bool success, PMU::ErrorCode error) {
                if (success) {
                    pmu_logger.info("PMU time sync successful");
                } else {
                    pmu_logger.error("PMU time sync failed: error %d", static_cast<int>(error));
                }
            });
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

void GreenhouseMode::updateGreenhouseState()
{
    GreenhouseHardwareState hardware_state;
    hardware_state.rtc_running = rtc_running();
    hardware_state.opening = (curtain_controller_.getState() == CurtainState::OPENING);
    hardware_state.closing = (curtain_controller_.getState() == CurtainState::CLOSING);
    hardware_state.calibrating = (curtain_controller_.getState() == CurtainState::CALIBRATING);
    greenhouse_state_.update(hardware_state);
}
