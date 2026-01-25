#include "irrigation_mode.h"

#include <cstring>

#include "pico/unique_id.h"

#include "hardware/rtc.h"

#include "../hal/logger.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;  // 60 seconds

// PMU UART configuration - adjust pins based on your hardware
#define PMU_UART_ID uart0
#define PMU_UART_TX_PIN 0
#define PMU_UART_RX_PIN 1

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

    // Set up work tracker - signals ready for sleep when all work completes
    work_tracker_.setIdleCallback([this]() { signalReadyForSleep(); });

    // Start with RTC sync work pending
    work_tracker_.addWork(WorkType::RtcSync);

    // Add registration work if needed
    if (needs_registration_) {
        work_tracker_.addWork(WorkType::Registration);
    }

    // Initialize PMU client at 9600 baud to match STM32 LPUART configuration
    pmu_client_ = new PmuClient(PMU_UART_ID, PMU_UART_TX_PIN, PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        pmu_logger.info("PMU client initialized successfully");

        // Set up PMU callback handlers
        auto &protocol = pmu_client_->getProtocol();

        protocol.onWakeNotification(
            [this](PMU::WakeReason reason, const PMU::ScheduleEntry *entry) {
                this->handlePmuWake(reason, entry);
            });

        protocol.onScheduleComplete([this]() { this->handleScheduleComplete(); });

        // Try to get time from PMU's battery-backed RTC (faster than waiting for hub sync)
        // Note: Don't send wake preamble (null bytes) - it can corrupt protocol state machine.
        // If STM32 is in STOP mode, first attempt may fail but we'll fall back to hub sync.
        pmu_logger.info("Requesting datetime from PMU...");
        protocol.getDateTime([this](bool valid, const PMU::DateTime &datetime) {
            if (valid) {
                pmu_logger.info("PMU has valid time: 20%02d-%02d-%02d %02d:%02d:%02d",
                                datetime.year, datetime.month, datetime.day, datetime.hour,
                                datetime.minute, datetime.second);

                // Set RP2040 RTC from PMU time
                datetime_t dt;
                dt.year = 2000 + datetime.year;
                dt.month = datetime.month;
                dt.day = datetime.day;
                dt.dotw = datetime.weekday;
                dt.hour = datetime.hour;
                dt.min = datetime.minute;
                dt.sec = datetime.second;

                if (rtc_set_datetime(&dt)) {
                    sleep_us(64);
                    updateStateMachine();
                    logger.info("RTC synced from PMU: %04d-%02d-%02d %02d:%02d:%02d", dt.year,
                                dt.month, dt.day, dt.hour, dt.min, dt.sec);
                    switchToOperationalPattern();
                    work_tracker_.completeWork(WorkType::RtcSync);
                } else {
                    logger.error("Failed to set RTC from PMU time");
                }
            } else {
                pmu_logger.info("PMU time not valid (first boot?) - waiting for hub sync");
            }
        });

        // NOTE: DateTime, wake interval, and schedules are now managed remotely
        // via the REST API -> Hub -> LoRa update pull system.
        // When the node wakes periodically, it will:
        // 1. Send CHECK_UPDATES to the hub
        // 2. Receive UPDATE_AVAILABLE messages with queued updates
        // 3. Apply updates to the PMU (datetime, wake interval, schedules)
        // 4. Continue pulling until no more updates remain

        pmu_logger.info("Waiting for configuration updates from hub via LoRa...");
    } else {
        logger.warn("PMU client not available - running without power management");
    }

    // Start with orange blinking pattern while waiting for RTC sync
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);
    // Store operational pattern (blue short blink) for after RTC sync
    // Blue single channel at full brightness for power efficiency
    operational_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 0, 0, 255);

    // Send initial heartbeat immediately to sync RTC (if PMU didn't have valid time)
    logger.info("Sending initial heartbeat for time sync...");
    uint32_t uptime = 0;
    uint8_t battery_level = 85;
    uint8_t signal_strength = 65;
    uint8_t active_sensors = CAP_VALVE_CONTROL;
    uint8_t error_flags = 0;
    messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength, active_sensors,
                             error_flags);

    // TODO: Re-enable CHECK_UPDATES after registration is working reliably
    // Currently disabled to focus on fixing registration message delivery
    // sleep_ms(500);
    // logger.info("Sending initial CHECK_UPDATES to hub");
    // sendCheckUpdates();

    // Add heartbeat task
    task_manager_.addTask(
        [this](uint32_t time) {
            logger.debug("Irrigation heartbeat");

            uint32_t uptime = time / 1000;  // Convert to seconds
            uint8_t battery_level = 85;     // Example battery level
            uint8_t signal_strength = 65;   // Example signal strength
            uint8_t error_flags = 0;        // No errors

            // Report valve control capability
            uint8_t active_sensors = CAP_VALVE_CONTROL;

            // Log active valves if any are open
            uint8_t valve_mask = valve_controller_.getActiveValveMask();
            if (valve_mask != 0) {
                logger.info("Active valves: 0x%02X", valve_mask);
            }

            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength,
                                     active_sensors, error_flags);
        },
        HEARTBEAT_INTERVAL_MS, "Heartbeat");

    irrigation_state_.markInitialized();
    updateIrrigationState();
}

void IrrigationMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry)
{
    switch (reason) {
        case PMU::WakeReason::Periodic:
            pmu_logger.info("Periodic wake - checking for updates");
            // Add update pull work and send CHECK_UPDATES
            work_tracker_.addWork(WorkType::UpdatePull);
            sendCheckUpdates();
            break;

        case PMU::WakeReason::Scheduled:
            if (entry) {
                logger.info("Scheduled wake - valve %d for %d seconds", entry->valveId,
                            entry->duration);

                if (entry->valveId < ValveController::NUM_VALVES) {
                    valve_controller_.openValve(entry->valveId);
                } else {
                    logger.error("Invalid valve ID %d in schedule", entry->valveId);
                }
            } else {
                logger.warn("Scheduled wake but no schedule entry provided");
            }
            break;

        case PMU::WakeReason::External:
            pmu_logger.info("External wake trigger");
            // External wake might be first boot or manual intervention
            // If we need registration, do it now
            if (needs_registration_) {
                logger.info("External wake + no address - attempting registration");
                attemptDeferredRegistration();
            } else {
                // No registration needed - check for updates
                work_tracker_.addWork(WorkType::UpdatePull);
                sendCheckUpdates();
            }
            break;
    }
}

void IrrigationMode::onLoop()
{
    // Process any pending PMU messages
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }

    // Check if all work is complete and fire idle callback if so
    // This is done at the end of the loop to ensure all events have settled
    work_tracker_.checkIdle();
}

void IrrigationMode::handleScheduleComplete()
{
    pmu_logger.warn("Schedule complete - power down imminent!");
    logger.info("Closing all valves before shutdown...");

    // Close all valves before power down
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        valve_controller_.closeValve(i);
    }

    // Brief delay to ensure valves are closed
    sleep_ms(500);

    pmu_logger.info("Ready for power down");
}

void IrrigationMode::onActuatorCommand(const ActuatorPayload *payload)
{
    if (!payload) {
        logger.error("NULL actuator payload");
        return;
    }

    // Handle valve commands
    if (payload->actuator_type == ACTUATOR_VALVE) {
        // Validate parameter length (need at least 1 byte for valve ID)
        if (payload->param_length < 1) {
            logger.error("Valve command missing valve ID parameter");
            return;
        }

        uint8_t valve_id = payload->params[0];  // First parameter is valve ID

        if (valve_id >= ValveController::NUM_VALVES) {
            logger.error("Invalid valve ID %d (max %d)", valve_id, ValveController::NUM_VALVES - 1);
            return;
        }

        if (payload->command == CMD_TURN_ON) {
            logger.info("Opening valve %d", valve_id);
            valve_controller_.openValve(valve_id);
        } else if (payload->command == CMD_TURN_OFF) {
            logger.info("Closing valve %d", valve_id);
            valve_controller_.closeValve(valve_id);
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

    // Also sync time to PMU if available
    if (pmu_available_ && pmu_client_ && payload) {
        // Convert HeartbeatResponsePayload to PMU::DateTime
        PMU::DateTime datetime(payload->year % 100,  // PMU uses 2-digit year (e.g., 25 for 2025)
                               payload->month, payload->day, payload->dotw, payload->hour,
                               payload->min, payload->sec);

        pmu_logger.info("Syncing time to PMU: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

        // Send datetime to PMU - PMU decides if update is needed based on drift
        pmu_client_->getProtocol().setDateTime(
            datetime, [this](bool success, PMU::ErrorCode error) {
                if (success) {
                    pmu_logger.info("PMU time sync successful");
                } else {
                    pmu_logger.error("PMU time sync failed: error %d", static_cast<int>(error));
                }
                // Complete RTC sync work regardless of PMU result - RP2040 RTC was synced by base
                // class
                work_tracker_.completeWork(WorkType::RtcSync);
                updateIrrigationState();
            });
    } else if (payload) {
        // No PMU available, but RTC was synced by base class
        work_tracker_.completeWork(WorkType::RtcSync);
        updateIrrigationState();
    }
}

void IrrigationMode::sendCheckUpdates()
{
    logger.info("Sending CHECK_UPDATES (seq=%d)", update_state_.current_sequence);

    // Send using messenger helper and store sequence number
    uint8_t seq_num = messenger_.sendCheckUpdates(HUB_ADDRESS, update_state_.current_sequence);

    if (seq_num != 0) {
        // Store the sequence number so we can cancel it when we receive UPDATE_AVAILABLE
        update_state_.pending_check_seq = seq_num;

        // Tell PMU to stay awake while we process updates
        if (pmu_available_ && pmu_client_) {
            pmu_client_->getProtocol().keepAwake(10);  // Keep awake for 10 seconds
        }
    } else {
        logger.error("Failed to send CHECK_UPDATES");
        // Complete update pull work on failure
        work_tracker_.completeWork(WorkType::UpdatePull);
    }
}

void IrrigationMode::onUpdateAvailable(const UpdateAvailablePayload *payload)
{
    if (!payload) {
        logger.error("NULL update payload");
        update_state_.reset();
        work_tracker_.completeWork(WorkType::UpdatePull);
        return;
    }

    // CRITICAL: Cancel the pending CHECK_UPDATES message
    // UPDATE_AVAILABLE is the response to CHECK_UPDATES, so we don't need to wait for an ACK
    if (update_state_.pending_check_seq != 0) {
        messenger_.cancelPendingMessage(update_state_.pending_check_seq);
        update_state_.pending_check_seq = 0;
    }

    // Keep awake while processing
    if (pmu_available_ && pmu_client_) {
        pmu_client_->getProtocol().keepAwake(10);  // Keep awake for 10 seconds
    }

    // Check if there are no more updates
    if (payload->has_update == 0) {
        logger.info("No more updates");
        update_state_.reset();
        // Complete update pull work - idle callback will signal sleep if no other work
        work_tracker_.completeWork(WorkType::UpdatePull);
        return;
    }

    // Parse update type
    UpdateType update_type = static_cast<UpdateType>(payload->update_type);
    uint8_t hub_sequence = payload->sequence;

    logger.info("Received update: type=%d, seq=%d", payload->update_type, hub_sequence);

    // Check if we've already processed this update (messages crossing in flight)
    if (hub_sequence <= update_state_.current_sequence) {
        logger.info("  Already processed seq=%d (current=%d), sending CHECK_UPDATES", hub_sequence,
                    update_state_.current_sequence);
        sendCheckUpdates();
        return;
    }

    // Get PMU protocol for applying updates
    auto &protocol = pmu_client_->getProtocol();

    switch (update_type) {
        case UpdateType::SET_SCHEDULE: {
            // Parse schedule data (8 bytes)
            const uint8_t *data = payload->payload_data;
            uint8_t index = data[0];
            PMU::ScheduleEntry entry;
            entry.hour = data[1];
            entry.minute = data[2];
            entry.duration = data[3] | (data[4] << 8);  // Little-endian uint16_t
            entry.daysMask = static_cast<PMU::DayOfWeek>(data[5]);
            entry.valveId = data[6];
            entry.enabled = (data[7] != 0);

            logger.info("  SET_SCHEDULE[%d]: %02d:%02d, valve=%d, duration=%ds, days=0x%02X", index,
                        entry.hour, entry.minute, entry.valveId, entry.duration,
                        static_cast<uint8_t>(entry.daysMask));

            // Apply to PMU
            protocol.setSchedule(entry, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                if (success) {
                    logger.info("  Schedule applied successfully");
                    // Update our sequence and send ACK with success status
                    update_state_.current_sequence = hub_sequence;
                    // Note: sendAck is private, we rely on automatic ACK from ReliableMessenger
                    // The UPDATE_AVAILABLE message has MSG_FLAG_RELIABLE so it gets auto-ACKed

                    // Immediately check for next update
                    sendCheckUpdates();
                } else {
                    logger.error("  Failed to apply schedule: error %d", static_cast<int>(error));
                    // For errors, we just don't update sequence and stop pulling updates
                    update_state_.reset();
                    work_tracker_.completeWork(WorkType::UpdatePull);
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
                    sendCheckUpdates();
                } else {
                    logger.error("  Failed to remove schedule: error %d", static_cast<int>(error));
                    update_state_.reset();
                    work_tracker_.completeWork(WorkType::UpdatePull);
                }
            });
            break;
        }

        case UpdateType::SET_DATETIME: {
            // Parse datetime (7 bytes)
            const uint8_t *data = payload->payload_data;
            PMU::DateTime datetime(data[0],  // year
                                   data[1],  // month
                                   data[2],  // day
                                   data[3],  // weekday
                                   data[4],  // hour
                                   data[5],  // minute
                                   data[6]   // second
            );

            logger.info("  SET_DATETIME: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

            protocol.setDateTime(
                datetime, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                    if (success) {
                        logger.info("  DateTime set successfully");
                        update_state_.current_sequence = hub_sequence;
                        sendCheckUpdates();
                    } else {
                        logger.error("  Failed to set datetime: error %d", static_cast<int>(error));
                        update_state_.reset();
                        work_tracker_.completeWork(WorkType::UpdatePull);
                    }
                });
            break;
        }

        case UpdateType::SET_WAKE_INTERVAL: {
            // Parse interval (2 bytes, little-endian to match hub packing)
            uint16_t interval_seconds = payload->payload_data[0] | (payload->payload_data[1] << 8);
            logger.info("  SET_WAKE_INTERVAL: %d seconds", interval_seconds);

            protocol.setWakeInterval(interval_seconds,
                                     [this, hub_sequence](bool success, PMU::ErrorCode error) {
                                         if (success) {
                                             logger.info("  Wake interval set successfully");
                                             update_state_.current_sequence = hub_sequence;
                                             sendCheckUpdates();
                                         } else {
                                             logger.error("  Failed to set wake interval: error %d",
                                                          static_cast<int>(error));
                                             update_state_.reset();
                                             work_tracker_.completeWork(WorkType::UpdatePull);
                                         }
                                     });
            break;
        }

        default:
            logger.error("Unknown update type %d", payload->update_type);
            update_state_.reset();
            work_tracker_.completeWork(WorkType::UpdatePull);
            break;
    }
}
void IrrigationMode::attemptDeferredRegistration()
{
    // Get device ID from hardware
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    uint64_t device_id = 0;
    memcpy(&device_id, board_id.id, sizeof(device_id));

    // Send registration request
    logger.info("Sending deferred registration (device_id=0x%016llX)", device_id);

    uint8_t registration_seq = messenger_.sendRegistrationRequest(
        ADDRESS_HUB, device_id, NODE_TYPE_HYBRID, CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE,
        0x0100,  // Firmware version
        "Irrigation Node");

    if (registration_seq != 0) {
        logger.info("Registration request sent (seq=%d)", registration_seq);
        needs_registration_ = false;  // Clear flag - we've attempted registration
        // Complete registration work - messenger handles response
        work_tracker_.completeWork(WorkType::Registration);
        // We'll get the response via normal message processing
        // When REG_RESPONSE arrives, messenger will update our address
    } else {
        logger.error("Failed to send registration request");
        // Complete registration work on failure too
        work_tracker_.completeWork(WorkType::Registration);
    }
}

void IrrigationMode::signalReadyForSleep()
{
    if (!pmu_available_ || !pmu_client_) {
        logger.debug("PMU not available, skipping ready for sleep signal");
        return;
    }

    // Retry loop - STM32 may need multiple attempts to receive command
    // First attempt wakes STM32 from STOP mode (even if command fails),
    // subsequent attempts should succeed.
    // Don't send wake preamble (null bytes) as it may corrupt protocol state machine.
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

        // Wait for response with polling
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
        sleep_ms(100);  // Small delay before retry
    }
}

void IrrigationMode::updateIrrigationState()
{
    IrrigationHardwareState hardware_state;
    hardware_state.rtc_running = rtc_running();
    hardware_state.valve_open = valve_controller_.getActiveValveMask() != 0;
    hardware_state.update_pending = work_tracker_.hasWork(WorkType::UpdatePull);
    hardware_state.applying_update = false;
    irrigation_state_.update(hardware_state);
}
