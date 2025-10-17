#include "irrigation_mode.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../led_patterns.h"
#include "../hal/logger.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;   // 60 seconds

// PMU UART configuration - adjust pins based on your hardware
#define PMU_UART_ID uart0
#define PMU_UART_TX_PIN 0
#define PMU_UART_RX_PIN 1

static Logger logger("IRRIG");
static Logger pmu_logger("PMU");

void IrrigationMode::onStart() {
    logger.info("=== IRRIGATION MODE ACTIVE ===");
    logger.info("- 2 valve irrigation node");
    logger.info("- PMU power management integration");
    logger.info("- Green LED heartbeat");

    // Initialize valve controller
    valve_controller_.initialize();

    // Initialize PMU client
    pmu_client_ = new PmuClient(PMU_UART_ID, PMU_UART_TX_PIN, PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        pmu_logger.info("PMU client initialized successfully");

        // Set up PMU callback handlers
        auto& protocol = pmu_client_->getProtocol();

        protocol.onWakeNotification([this](PMU::WakeReason reason, const PMU::ScheduleEntry* entry) {
            this->handlePmuWake(reason, entry);
        });

        protocol.onScheduleComplete([this]() {
            this->handleScheduleComplete();
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

    // Green heartbeat pattern for irrigation nodes
    led_pattern_ = std::make_unique<HeartbeatPattern>(led_, 0, 255, 0);

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

            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level,
                                  signal_strength, active_sensors, error_flags);
        },
        HEARTBEAT_INTERVAL_MS,
        "Heartbeat"
    );
}

void IrrigationMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry* entry) {
    switch (reason) {
        case PMU::WakeReason::Periodic:
            pmu_logger.info("Periodic wake - checking for updates");
            // Send CHECK_UPDATES to pull any pending configuration changes
            sendCheckUpdates();
            break;

        case PMU::WakeReason::Scheduled:
            if (entry) {
                logger.info("Scheduled wake - valve %d for %d seconds",
                           entry->valveId, entry->duration);

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
            break;
    }
}

void IrrigationMode::onLoop() {
    // Process any pending PMU messages (moved out of IRQ context)
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }
}

void IrrigationMode::handleScheduleComplete() {
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

void IrrigationMode::onActuatorCommand(const ActuatorPayload* payload) {
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

void IrrigationMode::sendCheckUpdates() {
    logger.info("Sending CHECK_UPDATES (seq=%d)", update_state_.current_sequence);

    // Send using messenger helper
    bool sent = messenger_.sendCheckUpdates(HUB_ADDRESS, update_state_.current_sequence);

    if (sent) {
        // Mark that we're processing updates and need to stay awake
        update_state_.processing = true;
        update_state_.last_keepawake_ms = to_ms_since_boot(get_absolute_time());

        // Tell PMU to stay awake while we process updates
        if (pmu_available_ && pmu_client_) {
            pmu_client_->getProtocol().keepAwake(10);  // Keep awake for 10 seconds
        }
    } else {
        logger.error("Failed to send CHECK_UPDATES");
    }
}

void IrrigationMode::onUpdateAvailable(const UpdateAvailablePayload* payload) {
    if (!payload) {
        logger.error("NULL update payload");
        update_state_.reset();
        return;
    }

    // Keep awake while processing
    update_state_.last_keepawake_ms = to_ms_since_boot(get_absolute_time());
    if (pmu_available_ && pmu_client_) {
        pmu_client_->getProtocol().keepAwake(10);  // Keep awake for 10 seconds
    }

    // Check if there are no more updates
    if (payload->has_update == 0) {
        logger.info("No more updates - allowing sleep");
        update_state_.reset();
        return;
    }

    // Parse update type
    UpdateType update_type = static_cast<UpdateType>(payload->update_type);
    uint8_t hub_sequence = payload->sequence;

    logger.info("Received update: type=%d, seq=%d", payload->update_type, hub_sequence);

    // Get PMU protocol for applying updates
    auto& protocol = pmu_client_->getProtocol();

    switch (update_type) {
        case UpdateType::SET_SCHEDULE: {
            // Parse schedule data (8 bytes)
            const uint8_t* data = payload->payload_data;
            PMU::ScheduleEntry entry;
            entry.hour = data[0];
            entry.minute = data[1];
            entry.duration = (data[2] << 8) | data[3];  // uint16_t duration
            entry.daysMask = static_cast<PMU::DayOfWeek>(data[4]);
            entry.valveId = data[5];
            entry.enabled = (data[6] != 0);
            uint8_t index = data[7];

            logger.info("  SET_SCHEDULE[%d]: %02d:%02d, valve=%d, duration=%ds, days=0x%02X",
                       index, entry.hour, entry.minute, entry.valveId, entry.duration, static_cast<uint8_t>(entry.daysMask));

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
                }
            });
            break;
        }

        case UpdateType::SET_DATETIME: {
            // Parse datetime (7 bytes)
            const uint8_t* data = payload->payload_data;
            PMU::DateTime datetime(
                data[0],  // year
                data[1],  // month
                data[2],  // day
                data[3],  // weekday
                data[4],  // hour
                data[5],  // minute
                data[6]   // second
            );

            logger.info("  SET_DATETIME: 20%02d-%02d-%02d %02d:%02d:%02d",
                       datetime.year, datetime.month, datetime.day,
                       datetime.hour, datetime.minute, datetime.second);

            protocol.setDateTime(datetime, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                if (success) {
                    logger.info("  DateTime set successfully");
                    update_state_.current_sequence = hub_sequence;
                    sendCheckUpdates();
                } else {
                    logger.error("  Failed to set datetime: error %d", static_cast<int>(error));
                    update_state_.reset();
                }
            });
            break;
        }

        case UpdateType::SET_WAKE_INTERVAL: {
            // Parse interval (2 bytes)
            uint16_t interval_seconds = (payload->payload_data[0] << 8) | payload->payload_data[1];
            logger.info("  SET_WAKE_INTERVAL: %d seconds", interval_seconds);

            protocol.setWakeInterval(interval_seconds, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                if (success) {
                    logger.info("  Wake interval set successfully");
                    update_state_.current_sequence = hub_sequence;
                    sendCheckUpdates();
                } else {
                    logger.error("  Failed to set wake interval: error %d", static_cast<int>(error));
                    update_state_.reset();
                }
            });
            break;
        }

        default:
            logger.error("Unknown update type %d", payload->update_type);
            update_state_.reset();
            break;
    }
}