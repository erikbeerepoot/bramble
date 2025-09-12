#include "irrigation_mode.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../led_patterns.h"
#include <cstdio>

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;   // 60 seconds

void IrrigationMode::onStart() {
    printf("=== IRRIGATION MODE ACTIVE ===\n");
    printf("- 2 valve irrigation node\n");
    printf("- Green LED heartbeat\n");
    
    // Initialize valve controller
    valve_controller_.initialize();
    
    // Green heartbeat pattern for irrigation nodes
    led_pattern_ = std::make_unique<HeartbeatPattern>(led_, 0, 255, 0);
    
    // Add heartbeat task
    task_manager_.addTask(
        [this](uint32_t time) {
            printf("Irrigation heartbeat\n");
            
            uint32_t uptime = time / 1000;  // Convert to seconds
            uint8_t battery_level = 85;     // Example battery level
            uint8_t signal_strength = 65;   // Example signal strength
            uint8_t error_flags = 0;        // No errors
            
            // Report valve control capability
            uint8_t active_sensors = CAP_VALVE_CONTROL;
            
            // Log active valves if any are open
            uint8_t valve_mask = valve_controller_.getActiveValveMask();
            if (valve_mask != 0) {
                printf("Active valves: 0x%02X\n", valve_mask);
            }
            
            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, 
                                  signal_strength, active_sensors, error_flags);
        },
        HEARTBEAT_INTERVAL_MS,
        "Heartbeat"
    );
}

void IrrigationMode::onActuatorCommand(const ActuatorPayload* payload) {
    if (!payload) {
        printf("ERROR: NULL actuator payload\n");
        return;
    }
    
    // Handle valve commands
    if (payload->actuator_type == ACTUATOR_VALVE) {
        // Validate parameter length (need at least 1 byte for valve ID)
        if (payload->param_length < 1) {
            printf("ERROR: Valve command missing valve ID parameter\n");
            return;
        }
        
        uint8_t valve_id = payload->params[0];  // First parameter is valve ID
        
        if (valve_id >= ValveController::NUM_VALVES) {
            printf("ERROR: Invalid valve ID %d (max %d)\n", valve_id, ValveController::NUM_VALVES - 1);
            return;
        }
        
        if (payload->command == CMD_TURN_ON) {
            printf("Opening valve %d\n", valve_id);
            valve_controller_.openValve(valve_id);
        } else if (payload->command == CMD_TURN_OFF) {
            printf("Closing valve %d\n", valve_id);
            valve_controller_.closeValve(valve_id);
        } else {
            printf("ERROR: Unknown valve command %d\n", payload->command);
        }
    } else {
        printf("WARNING: Unsupported actuator type %d\n", payload->actuator_type);
    }
}