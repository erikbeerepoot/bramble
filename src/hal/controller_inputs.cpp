#include "controller_inputs.h"
#include "gpio_interrupt_manager.h"
#include <cstdio>

void ControllerInputs::initialize(uint8_t pin_a0, uint8_t pin_a1, InputCallback callback) {
    if (initialized_) {
        printf("WARNING: ControllerInputs already initialized\n");
        return;
    }
    
    printf("Initializing ControllerInputs...\n");
    
    // Store configuration
    input_pins_[0] = pin_a0;
    input_pins_[1] = pin_a1;
    callback_ = callback;
    
    // Configure input pins
    printf("Configuring %d input pins...\n", NUM_INPUTS);
    for (uint8_t i = 0; i < NUM_INPUTS; i++) {
        printf("About to configure input %d on GPIO %d\n", i, input_pins_[i]);
        configureInputPin(input_pins_[i]);

        // Read initial state
        input_states_[i] = gpio_get(input_pins_[i]);
        pending_changes_[i] = false;

        printf("Input %d (GPIO %d): configured, initial state = %s\n",
               i, input_pins_[i], input_states_[i] ? "HIGH" : "LOW");
    }
    
    initialized_ = true;
    printf("ControllerInputs initialized\n");
}

bool ControllerInputs::getInputState(uint8_t input_id) const {
    if (input_id >= NUM_INPUTS) {
        return false;
    }
    return input_states_[input_id];
}

void ControllerInputs::update() {
    if (!initialized_) {
        return;
    }
    
    // Process any pending changes from interrupts
    for (uint8_t i = 0; i < NUM_INPUTS; i++) {
        if (pending_changes_[i]) {
            pending_changes_[i] = false;
            
            // Read current state
            bool new_state = gpio_get(input_pins_[i]);
            
            // Only process if state actually changed
            if (new_state != input_states_[i]) {
                printf("Input %d changed: %s -> %s\n", 
                       i, input_states_[i] ? "HIGH" : "LOW", new_state ? "HIGH" : "LOW");
                
                input_states_[i] = new_state;
                
                // Call callback if registered
                if (callback_) {
                    callback_(i, new_state);
                }
            }
        }
    }
}

void ControllerInputs::configureInputPin(uint8_t pin) {
    // Initialize pin as input with pull-down resistor
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_down(pin);  // Pull-down so HIGH = active

    printf("Configuring GPIO %d as input with pull-down\n", pin);

    // Verify pin configuration
    bool is_input = gpio_get_dir(pin) == GPIO_IN;
    bool initial_state = gpio_get(pin);
    printf("GPIO %d config verified: is_input=%d, initial_state=%d\n",
           pin, is_input ? 1 : 0, initial_state ? 1 : 0);

    // Register interrupt handler with the global manager
    GpioInterruptManager::getInstance().registerHandler(
        pin,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        [this, pin](uint gpio, uint32_t events) {
            this->handleGpioInterrupt(gpio, events);
        }
    );

    printf("Interrupts registered for GPIO %d (both edges)\n", pin);
}

void ControllerInputs::handleGpioInterrupt(uint gpio, uint32_t events) {
    printf("GPIO interrupt on pin %d, events: 0x%X\n", gpio, events);
    // Find which input pin triggered the interrupt
    for (uint8_t i = 0; i < NUM_INPUTS; i++) {
        if (input_pins_[i] == gpio) {
            printf("Input %d interrupt detected on GPIO %d\n", i, gpio);
            // Mark as pending for processing in main loop
            pending_changes_[i] = true;
            break;
        }
    }
}