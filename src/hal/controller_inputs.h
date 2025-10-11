#pragma once

#include <cstdint>
#include <functional>
#include "hardware/gpio.h"

/**
 * @brief Controller input handler for A0/A1 valve control inputs
 * 
 * Monitors digital inputs and triggers callbacks on state changes.
 * Designed for simple valve on/off control via GPIO interrupts.
 */
class ControllerInputs {
public:
    /**
     * @brief Callback function type for input changes
     * @param input_id Input number (0 for A0, 1 for A1) 
     * @param state New input state (true = high/on, false = low/off)
     */
    using InputCallback = std::function<void(uint8_t input_id, bool state)>;
    
    /**
     * @brief Initialize controller inputs
     * @param pin_a0 GPIO pin for A0 input
     * @param pin_a1 GPIO pin for A1 input  
     * @param callback Function to call when inputs change
     */
    void initialize(uint8_t pin_a0, uint8_t pin_a1, InputCallback callback);
    
    /**
     * @brief Get current state of an input
     * @param input_id Input number (0 or 1)
     * @return Current input state (true = high, false = low)
     */
    bool getInputState(uint8_t input_id) const;
    
    /**
     * @brief Process any pending input changes
     * 
     * Should be called regularly from main loop to handle
     * input changes detected by interrupts.
     */
    void update();

private:
    static constexpr uint8_t NUM_INPUTS = 2;
    
    uint8_t input_pins_[NUM_INPUTS];
    bool input_states_[NUM_INPUTS];
    bool pending_changes_[NUM_INPUTS];
    InputCallback callback_;
    bool initialized_ = false;
    
    /**
     * @brief Configure a GPIO pin as input with pull-down
     * @param pin GPIO pin number
     */
    void configureInputPin(uint8_t pin);

    /**
     * @brief Instance interrupt handler
     * @param gpio GPIO pin that triggered interrupt
     * @param events Event mask
     */
    void handleGpioInterrupt(uint gpio, uint32_t events);
};