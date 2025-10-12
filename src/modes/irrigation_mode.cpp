#include "irrigation_mode.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../led_patterns.h"
#include "../hal/logger.h"
#include <cstdio>

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

        // Set default wake interval to 5 minutes with result callback
        pmu_logger.info("Sending setWakeInterval command to PMU...");
        protocol.setWakeInterval(300, [](bool success, PMU::ErrorCode error) {
            if (success) {
                pmu_logger.info("Wake interval set successfully");
            } else {
                pmu_logger.error("Failed to set wake interval: error code %d", static_cast<int>(error));
            }
        });
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
            pmu_logger.info("Periodic wake - sending heartbeat");
            // Normal periodic wake - the heartbeat task will handle this
            break;

        case PMU::WakeReason::Scheduled:
            if (entry) {
                pmu_logger.info("Scheduled wake - watering valve %d for %d seconds",
                               entry->valveId, entry->duration);

                // Open the scheduled valve
                if (entry->valveId < ValveController::NUM_VALVES) {
                    logger.info("Opening valve %d for scheduled watering", entry->valveId);
                    valve_controller_.openValve(entry->valveId);

                    // TODO: Implement duration-based automatic valve closure
                    // For now, valve will stay open until PMU sends ScheduleComplete
                } else {
                    logger.error("Invalid valve ID %d in schedule", entry->valveId);
                }
            } else {
                pmu_logger.warn("Scheduled wake but no schedule entry provided");
            }
            break;

        case PMU::WakeReason::External:
            pmu_logger.info("External wake trigger");
            break;
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