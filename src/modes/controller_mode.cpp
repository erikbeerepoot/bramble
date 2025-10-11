#include "controller_mode.h"
#include <cstdio>
#include "../lora/reliable_messenger.h"
#include "../lora/message.h"

// Controller will send commands to this irrigation node address  
constexpr uint16_t DEFAULT_IRRIGATION_NODE = 0x0001;

void ControllerMode::onStart() {
    printf("=== CONTROLLER MODE ACTIVE ===\n");
    printf("- Irrigation controller with A0/A1 input monitoring\n");
    printf("- A0 HIGH -> Valve 0 ON, A0 LOW -> Valve 0 OFF\n");
    printf("- A1 HIGH -> Valve 1 ON, A1 LOW -> Valve 1 OFF\n");
    printf("- Hub mode: managing irrigation network\n");
    
    // Set default irrigation node address
    irrigation_node_address_ = DEFAULT_IRRIGATION_NODE;
    
    printf("=== About to initialize ControllerInputs ===\n");

    // Initialize input handler with callback
    input_handler_.initialize(26, 27,  // PIN_A0, PIN_A1
        [this](uint8_t input_id, bool state) {
            onInputChange(input_id, state);
        });
    
    // Set up LED pattern - blue breathing for hub
    led_pattern_ = std::make_unique<BreathingPattern>(led_, 0, 0, 255);
    
    // Add input monitoring task (just processes pending changes from interrupts)
    task_manager_.addTask(
        [this](uint32_t time) {
            // Process any pending input changes from the interrupt handler
            input_handler_.update();
        },
        10,  // Check every 10ms for responsive input handling
        "Input Monitor"
    );
    
    // Add heartbeat task for hub status
    task_manager_.addTask(
        [this](uint32_t time) {
            printf("Controller heartbeat - A0:%s A1:%s -> Target node: 0x%04X\n",
                   input_handler_.getInputState(0) ? "HIGH" : "LOW",
                   input_handler_.getInputState(1) ? "HIGH" : "LOW", 
                   irrigation_node_address_);
        },
        30000,  // Every 30 seconds
        "Status Report"
    );
    
    printf("Controller initialized - monitoring A0/A1 inputs\n");
}

void ControllerMode::onInputChange(uint8_t input_id, bool state) {
    printf("=== INPUT CHANGE ===\n");
    printf("Input A%d: %s\n", input_id, state ? "HIGH (ON)" : "LOW (OFF)");
    
    // Map input_id to valve_id (direct mapping: A0->Valve0, A1->Valve1)
    uint8_t valve_id = input_id;
    
    // Send valve command
    sendValveCommand(valve_id, state);
}

void ControllerMode::sendValveCommand(uint8_t valve_id, bool turn_on) {
    printf("Sending valve command: Valve %d -> %s to node 0x%04X\n",
           valve_id, turn_on ? "ON" : "OFF", irrigation_node_address_);

    // Create actuator command payload
    uint8_t command = turn_on ? CMD_TURN_ON : CMD_TURN_OFF;
    uint8_t params[] = {valve_id};  // Valve ID as parameter
    
    // Send reliable actuator command
    bool success = messenger_.sendActuatorCommand(
        irrigation_node_address_,    // Target irrigation node
        ACTUATOR_VALVE,             // Valve actuator type
        command,                    // Turn on/off command
        params,                     // Valve ID parameter
        sizeof(params),             // Parameter length
        RELIABLE                    // Ensure delivery
    );
    
    if (success) {
        printf("Valve command sent successfully\n");
    } else {
        printf("ERROR: Failed to send valve command\n");
    }
}