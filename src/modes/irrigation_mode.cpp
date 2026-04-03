#include "irrigation_mode.h"

#include <cstring>

#include "pico/unique_id.h"

#include "hardware/watchdog.h"

#include "../hal/logger.h"
#include "../hal/rtc_compat.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../util/time.h"
#include "../version.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;  // 60 seconds

// PMU UART configuration - selected by board version via board_pins.h
#include "../board/board_pins.h"

static Logger logger("IRRIG");
static Logger pmu_logger("PMU");

void IrrigationMode::onStart()
{
    logger.info("=== IRRIGATION MODE ACTIVE ===");
    logger.info("- 2 valve irrigation node");
    logger.info("- PMU power management integration");
    logger.info("- Orange LED blink (init) -> Blue short blink (operational)");

    // Check if we need to register (no saved address)
    needs_registration_ = (messenger_.getNodeAddress() == ADDRESS_UNREGISTERED);
    if (needs_registration_) {
        logger.info("No saved address - will register on first PMU wake");
    }

    // Initialize valve controller
    // TODO: Re-enable after UART debug testing - GPIO24 conflict
    // valve_controller_.initialize();

    // Set up state machine callback - drives all side effects
    irrigation_state_.setCallback([this](IrrigationState state) { this->onStateChange(state); });

    // Initialize PMU client at 9600 baud to match STM32 LPUART configuration
    pmu_client_ =
        new PmuClient(Board::PMU_UART_PORT, Board::PMU_UART_TX_PIN, Board::PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        pmu_logger.info("PMU client initialized successfully");

        // Set up PMU callback handlers
        PMU::Protocol &protocol = pmu_client_->getProtocol();

        protocol.onWakeNotification([this](PMU::WakeReason reason, const PMU::ScheduleEntry *entry,
                                           bool state_valid, const uint8_t *state) {
            (void)state_valid;
            (void)state;
            this->handlePmuWake(reason, entry);
        });

        protocol.onScheduleComplete([this]() { this->handleScheduleComplete(); });

        pmu_logger.info("Waiting for configuration updates from hub via LoRa...");
    } else {
        logger.warn("PMU client not available - running without power management");
    }

    // Start with orange blinking pattern while waiting for RTC sync
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);
    operational_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 0, 0, 255);

    // Add heartbeat task
    task_manager_.addTask(
        [this](uint32_t time) {
            logger.debug("Irrigation heartbeat");

            uint32_t uptime = time / 1000;
            uint8_t battery_level = 85;
            uint8_t signal_strength = 65;
            uint8_t error_flags = 0;
            uint8_t active_sensors = CAP_VALVE_CONTROL;

            uint8_t valve_mask = valve_controller_.getActiveValveMask();
            if (valve_mask != 0) {
                logger.info("Active valves: 0x%02X", valve_mask);
            }

            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength,
                                     active_sensors, error_flags);
        },
        HEARTBEAT_INTERVAL_MS, "Heartbeat");

    // Kick off the state machine - triggers REGISTERING via callback
    irrigation_state_.markInitialized();
}

void IrrigationMode::onStateChange(IrrigationState state)
{
    switch (state) {
        case IrrigationState::REGISTERING:
            if (needs_registration_) {
                attemptDeferredRegistration();
            } else {
                // Already registered, skip to time sync
                irrigation_state_.reportRegistrationComplete();
            }
            break;

        case IrrigationState::AWAITING_TIME:
            requestTimeSync();
            break;

        case IrrigationState::SYNCING_TIME:
            // Waiting for hub heartbeat response - no action needed
            break;

        case IrrigationState::CHECKING_UPDATES:
            sendCheckUpdates();
            break;

        case IrrigationState::APPLYING_UPDATE:
            // Waiting for PMU callback - no action needed
            break;

        case IrrigationState::VALVE_ACTIVE:
            // Keep PMU awake while valve is open
            if (pmu_available_ && pmu_client_) {
                pmu_client_->getProtocol().keepAwake(10);
            }
            break;

        case IrrigationState::READY_FOR_SLEEP:
            switchToOperationalPattern();
            signalReadyForSleep();
            break;

        case IrrigationState::ERROR:
            led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 0, 0, 250, 250);
            break;

        case IrrigationState::INITIALIZING:
            // Should not re-enter INITIALIZING
            break;
    }
}

void IrrigationMode::requestTimeSync()
{
    if (!pmu_available_ || !pmu_client_) {
        // No PMU - send heartbeat to hub for time sync
        logger.info("No PMU - sending heartbeat for time sync");
        messenger_.sendHeartbeat(HUB_ADDRESS, 0, 85, 65, CAP_VALVE_CONTROL, 0);
        irrigation_state_.reportHeartbeatSent();
        return;
    }

    // Try to get time from PMU's battery-backed RTC first
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
                logger.info("RTC synced from PMU: %04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month,
                            dt.day, dt.hour, dt.min, dt.sec);
                switchToOperationalPattern();
                irrigation_state_.reportTimeSyncComplete();
            } else {
                logger.error("Failed to set RTC from PMU time - falling back to hub sync");
                messenger_.sendHeartbeat(HUB_ADDRESS, 0, 85, 65, CAP_VALVE_CONTROL, 0);
                irrigation_state_.reportHeartbeatSent();
            }
        } else {
            pmu_logger.info("PMU time not valid (first boot?) - sending heartbeat for hub sync");
            messenger_.sendHeartbeat(HUB_ADDRESS, 0, 85, 65, CAP_VALVE_CONTROL, 0);
            irrigation_state_.reportHeartbeatSent();
        }
    });
}

void IrrigationMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry)
{
    // For scheduled wakes, open the valve before reporting to state machine
    if (reason == PMU::WakeReason::Scheduled && entry) {
        logger.info("Scheduled wake - valve %d for %d seconds", entry->valveId, entry->duration);
        if (entry->valveId < ValveController::NUM_VALVES) {
            valve_controller_.openValve(entry->valveId);
        } else {
            logger.error("Invalid valve ID %d in schedule", entry->valveId);
        }
    }

    // Let state machine handle the transition
    irrigation_state_.reportWakeFromSleep(reason);

    // For external wake with pending registration
    if (reason == PMU::WakeReason::External && needs_registration_) {
        logger.info("External wake + no address - registration will happen via state machine");
    }
}

void IrrigationMode::onLoop()
{
    // Process any pending PMU messages
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }
}

void IrrigationMode::handleScheduleComplete()
{
    pmu_logger.warn("Schedule complete - power down imminent!");
    logger.info("Closing all valves before shutdown...");

    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        valve_controller_.closeValve(i);
    }

    sleep_ms(500);
    pmu_logger.info("Ready for power down");
}

void IrrigationMode::onActuatorCommand(const ActuatorPayload *payload)
{
    if (!payload) {
        logger.error("NULL actuator payload");
        return;
    }

    if (payload->actuator_type == ACTUATOR_VALVE) {
        if (payload->param_length < 1) {
            logger.error("Valve command missing valve ID parameter");
            return;
        }

        uint8_t valve_id = payload->params[0];

        if (valve_id >= ValveController::NUM_VALVES) {
            logger.error("Invalid valve ID %d (max %d)", valve_id, ValveController::NUM_VALVES - 1);
            return;
        }

        if (payload->command == CMD_TURN_ON) {
            logger.info("Opening valve %d", valve_id);
            valve_controller_.openValve(valve_id);
            irrigation_state_.reportValveOpened();
        } else if (payload->command == CMD_TURN_OFF) {
            logger.info("Closing valve %d", valve_id);
            valve_controller_.closeValve(valve_id);
            // Only report closed if no valves remain open
            if (valve_controller_.getActiveValveMask() == 0) {
                irrigation_state_.reportValveClosed();
            }
        } else {
            logger.error("Unknown valve command %d", payload->command);
        }
    } else {
        logger.warn("Unsupported actuator type %d", payload->actuator_type);
    }
}

void IrrigationMode::onHeartbeatResponse(const HeartbeatResponsePayload *payload)
{
    // Call base class implementation to update RP2040 RTC
    ApplicationMode::onHeartbeatResponse(payload);

    // Sync time to PMU if available
    if (pmu_available_ && pmu_client_ && payload) {
        PMU::DateTime datetime(payload->year % 100, payload->month, payload->day, payload->dotw,
                               payload->hour, payload->min, payload->sec);

        pmu_logger.info("Syncing time to PMU: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

        pmu_client_->getProtocol().setDateTime(
            datetime, [this](bool success, PMU::ErrorCode error) {
                if (success) {
                    pmu_logger.info("PMU time sync successful");
                } else {
                    pmu_logger.error("PMU time sync failed: error %d", static_cast<int>(error));
                }
                // RP2040 RTC was synced by base class regardless of PMU result
                irrigation_state_.reportTimeSyncComplete();
            });
    } else if (payload) {
        // No PMU available, but RTC was synced by base class
        irrigation_state_.reportTimeSyncComplete();
    }

    // If hub signals pending updates and we're already past time sync,
    // this will be handled by the normal CHECKING_UPDATES flow
    if (payload && payload->pending_update_flags != PENDING_FLAG_NONE) {
        logger.info("Pending updates flagged (0x%02X)", payload->pending_update_flags);
    }
}

void IrrigationMode::sendCheckUpdates()
{
    logger.info("Sending CHECK_UPDATES (seq=%d)", update_state_.current_sequence);

    uint8_t seq_num = messenger_.sendCheckUpdates(HUB_ADDRESS, update_state_.current_sequence);

    if (seq_num != 0) {
        update_state_.pending_check_seq = seq_num;

        // Keep PMU awake while processing updates
        if (pmu_available_ && pmu_client_) {
            pmu_client_->getProtocol().keepAwake(10);
        }
    } else {
        logger.error("Failed to send CHECK_UPDATES");
        irrigation_state_.reportUpdateFailed();
    }
}

void IrrigationMode::onUpdateAvailable(const UpdateAvailablePayload *payload)
{
    if (!payload) {
        logger.error("NULL update payload");
        update_state_.reset();
        irrigation_state_.reportUpdateFailed();
        return;
    }

    // Cancel the pending CHECK_UPDATES message (UPDATE_AVAILABLE is the response)
    if (update_state_.pending_check_seq != 0) {
        messenger_.cancelPendingMessage(update_state_.pending_check_seq);
        update_state_.pending_check_seq = 0;
    }

    // Keep awake while processing
    if (pmu_available_ && pmu_client_) {
        pmu_client_->getProtocol().keepAwake(10);
    }

    // No more updates?
    if (payload->has_update == 0) {
        update_state_.reset();
        irrigation_state_.reportUpdateReceived(false);
        return;
    }

    // We have an update to apply
    UpdateType update_type = static_cast<UpdateType>(payload->update_type);
    uint8_t hub_sequence = payload->sequence;

    logger.info("Received update: type=%d, seq=%d", payload->update_type, hub_sequence);

    // Already processed?
    if (hub_sequence <= update_state_.current_sequence) {
        logger.info("  Already processed seq=%d (current=%d), re-checking", hub_sequence,
                    update_state_.current_sequence);
        sendCheckUpdates();
        return;
    }

    // Signal state machine that we have an update to apply
    irrigation_state_.reportUpdateReceived(true);

    // Apply the update via PMU
    PMU::Protocol &protocol = pmu_client_->getProtocol();

    switch (update_type) {
        case UpdateType::SET_SCHEDULE: {
            const uint8_t *data = payload->payload_data;
            uint8_t index = data[0];
            PMU::ScheduleEntry entry;
            entry.hour = data[1];
            entry.minute = data[2];
            entry.duration = data[3] | (data[4] << 8);
            entry.daysMask = static_cast<PMU::DayOfWeek>(data[5]);
            entry.valveId = data[6];
            entry.enabled = (data[7] != 0);

            logger.info("  SET_SCHEDULE[%d]: %02d:%02d, valve=%d, duration=%ds, days=0x%02X", index,
                        entry.hour, entry.minute, entry.valveId, entry.duration,
                        static_cast<uint8_t>(entry.daysMask));

            protocol.setSchedule(entry, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                if (success) {
                    logger.info("  Schedule applied successfully");
                    update_state_.current_sequence = hub_sequence;
                    irrigation_state_.reportUpdateApplied();
                } else {
                    logger.error("  Failed to apply schedule: error %d", static_cast<int>(error));
                    update_state_.reset();
                    irrigation_state_.reportUpdateFailed();
                }
            });
            break;
        }

        case UpdateType::REMOVE_SCHEDULE: {
            uint8_t index = payload->payload_data[0];
            logger.info("  REMOVE_SCHEDULE[%d]", index);

            protocol.clearSchedule(index, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                if (success) {
                    logger.info("  Schedule removed successfully");
                    update_state_.current_sequence = hub_sequence;
                    irrigation_state_.reportUpdateApplied();
                } else {
                    logger.error("  Failed to remove schedule: error %d", static_cast<int>(error));
                    update_state_.reset();
                    irrigation_state_.reportUpdateFailed();
                }
            });
            break;
        }

        case UpdateType::SET_DATETIME: {
            const uint8_t *data = payload->payload_data;
            PMU::DateTime datetime(data[0], data[1], data[2], data[3], data[4], data[5], data[6]);

            logger.info("  SET_DATETIME: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

            protocol.setDateTime(
                datetime, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                    if (success) {
                        logger.info("  DateTime set successfully");
                        update_state_.current_sequence = hub_sequence;
                        irrigation_state_.reportUpdateApplied();
                    } else {
                        logger.error("  Failed to set datetime: error %d", static_cast<int>(error));
                        update_state_.reset();
                        irrigation_state_.reportUpdateFailed();
                    }
                });
            break;
        }

        case UpdateType::SET_WAKE_INTERVAL: {
            uint16_t interval_seconds = payload->payload_data[0] | (payload->payload_data[1] << 8);
            logger.info("  SET_WAKE_INTERVAL: %d seconds", interval_seconds);

            protocol.setWakeInterval(interval_seconds,
                                     [this, hub_sequence](bool success, PMU::ErrorCode error) {
                                         if (success) {
                                             logger.info("  Wake interval set successfully");
                                             update_state_.current_sequence = hub_sequence;
                                             irrigation_state_.reportUpdateApplied();
                                         } else {
                                             logger.error("  Failed to set wake interval: error %d",
                                                          static_cast<int>(error));
                                             update_state_.reset();
                                             irrigation_state_.reportUpdateFailed();
                                         }
                                     });
            break;
        }

        default:
            logger.error("Unknown update type %d", payload->update_type);
            update_state_.reset();
            irrigation_state_.reportUpdateFailed();
            break;
    }
}

void IrrigationMode::attemptDeferredRegistration()
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    uint64_t device_id = 0;
    memcpy(&device_id, board_id.id, sizeof(device_id));

    logger.info("Sending deferred registration (device_id=0x%016llX)", device_id);

    uint8_t registration_seq = messenger_.sendRegistrationRequest(
        ADDRESS_HUB, device_id, NODE_TYPE_HYBRID, CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE,
        BRAMBLE_FIRMWARE_VERSION, "Irrigation Node");

    if (registration_seq != 0) {
        logger.info("Registration request sent (seq=%d)", registration_seq);
        needs_registration_ = false;
        irrigation_state_.reportRegistrationComplete();
    } else {
        logger.error("Failed to send registration request");
        irrigation_state_.reportRegistrationComplete();
    }
}

void IrrigationMode::signalReadyForSleep()
{
    if (!pmu_available_ || !pmu_client_) {
        logger.debug("PMU not available, skipping ready for sleep signal");
        return;
    }

    static constexpr int MAX_RETRIES = 3;
    static constexpr int RESPONSE_WAIT_MS = 500;
    static constexpr int POLL_INTERVAL_MS = 50;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        volatile bool got_response = false;
        volatile bool sleep_acked = false;

        pmu_logger.info("ReadyForSleep attempt %d/%d", attempt + 1, MAX_RETRIES);
        pmu_client_->getProtocol().readyForSleep(
            [&got_response, &sleep_acked](bool success, PMU::ErrorCode error) {
                got_response = true;
                sleep_acked = success;
                if (success) {
                    pmu_logger.info("Ready for sleep acknowledged");
                } else {
                    pmu_logger.error("Ready for sleep failed: error %d", static_cast<int>(error));
                }
            });

        for (int wait = 0; wait < RESPONSE_WAIT_MS / POLL_INTERVAL_MS && !got_response; wait++) {
            sleep_ms(POLL_INTERVAL_MS);
            pmu_client_->process();
        }

        if (got_response) {
            if (sleep_acked) {
                pmu_logger.info("Sleep signal successful on attempt %d", attempt + 1);
            }
            break;
        }

        pmu_logger.warn("No response to ReadyForSleep (attempt %d/%d)", attempt + 1, MAX_RETRIES);
        sleep_ms(100);
    }
}

void IrrigationMode::onRebootRequested()
{
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

void IrrigationMode::onFactoryResetRequested()
{
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
