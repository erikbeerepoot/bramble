#include "controller_mode.h"

#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "hal/logger.h"

static Logger logger("Controller");

// Controller will send commands to this irrigation node address
constexpr uint16_t DEFAULT_IRRIGATION_NODE = 0x0001;

void ControllerMode::onStart()
{
    logger.info("=== CONTROLLER MODE ACTIVE ===");
    logger.info("- Irrigation controller with A0/A1 input monitoring");
    logger.info("- A0 HIGH -> Valve 0 ON, A0 LOW -> Valve 0 OFF");
    logger.info("- A1 HIGH -> Valve 1 ON, A1 LOW -> Valve 1 OFF");
    logger.info("- Hub mode: managing irrigation network");

    // Set default irrigation node address
    irrigation_node_address_ = DEFAULT_IRRIGATION_NODE;

    logger.info("=== About to initialize ControllerInputs ===");

    // Initialize input handler with callback
    input_handler_.initialize(
        26, 27,  // PIN_A0, PIN_A1
        [this](uint8_t input_id, bool state) { onInputChange(input_id, state); });

    // Set up LED pattern - blue breathing for hub
    led_pattern_ = std::make_unique<BreathingPattern>(led_, 0, 0, 255);

    // Add input monitoring task (just processes pending changes from interrupts)
    task_manager_.addTask(
        [this](uint32_t time) {
            // Process any pending input changes from the interrupt handler
            input_handler_.update();
        },
        10,  // Check every 10ms for responsive input handling
        "Input Monitor");

    // Add heartbeat task for hub status
    task_manager_.addTask(
        [this](uint32_t time) {
            logger.info("Controller heartbeat - A0:%s A1:%s -> Target node: 0x%04X",
                        input_handler_.getInputState(0) ? "HIGH" : "LOW",
                        input_handler_.getInputState(1) ? "HIGH" : "LOW", irrigation_node_address_);
        },
        30000,  // Every 30 seconds
        "Status Report");

    logger.info("Controller initialized - monitoring A0/A1 inputs");
}

void ControllerMode::onInputChange(uint8_t input_id, bool state)
{
    logger.info("=== INPUT CHANGE ===");
    logger.info("Input A%d: %s", input_id, state ? "HIGH (ON)" : "LOW (OFF)");

    // Map input_id to valve_id (direct mapping: A0->Valve0, A1->Valve1)
    uint8_t valve_id = input_id;

    // Send valve command
    sendValveCommand(valve_id, state);
}

void ControllerMode::sendValveCommand(uint8_t valve_id, bool turn_on)
{
    logger.info("Sending valve command: Valve %d -> %s to node 0x%04X", valve_id,
                turn_on ? "ON" : "OFF", irrigation_node_address_);

    // Create actuator command payload
    uint8_t command = turn_on ? CMD_TURN_ON : CMD_TURN_OFF;
    uint8_t params[] = {valve_id};  // Valve ID as parameter

    // Send reliable actuator command
    bool success =
        messenger_.sendActuatorCommand(irrigation_node_address_,  // Target irrigation node
                                       ACTUATOR_VALVE,            // Valve actuator type
                                       command,                   // Turn on/off command
                                       params,                    // Valve ID parameter
                                       sizeof(params),            // Parameter length
                                       RELIABLE                   // Ensure delivery
        );

    if (success) {
        logger.info("Valve command sent successfully");
    } else {
        logger.error("Failed to send valve command");
    }
}