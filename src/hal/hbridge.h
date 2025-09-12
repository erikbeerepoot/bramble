#pragma once

#include <cstdint>
#include "pico/stdlib.h"
#include "pico/critical_section.h"

/**
 * @brief H-Bridge controller for bidirectional DC motor/solenoid control
 * 
 * Controls 4 MOSFETs in H-bridge configuration to allow bidirectional
 * current flow through a load (e.g., DC latching solenoid valve).
 * 
 * Safety features:
 * - Shoot-through protection (never enables both high and low on same side)
 * - Maximum pulse duration enforcement
 * - Immediate shutoff capability
 * - All pins default to OFF state
 */
class HBridge {
public:
    /**
     * @brief Direction of current flow through the H-bridge
     */
    enum Direction {
        FORWARD,  // Current flows A->B (HIGH_A + LOW_B active)
        REVERSE   // Current flows B->A (HIGH_B + LOW_A active)
    };
    
    /**
     * @brief Initialize the H-bridge with GPIO pins
     * @param pin_high_a GPIO pin for high-side MOSFET A
     * @param pin_low_a GPIO pin for low-side MOSFET A
     * @param pin_high_b GPIO pin for high-side MOSFET B
     * @param pin_low_b GPIO pin for low-side MOSFET B
     */
    void initialize(uint8_t pin_high_a, uint8_t pin_low_a,
                   uint8_t pin_high_b, uint8_t pin_low_b);
    
    /**
     * @brief Send a pulse through the H-bridge in specified direction
     * @param dir Direction of current flow
     * @param duration_ms Duration of pulse in milliseconds (max 300ms)
     * 
     * This method will:
     * 1. Set all pins LOW (safety)
     * 2. Enable appropriate MOSFETs for direction
     * 3. Wait for duration
     * 4. Set all pins LOW again
     */
    void pulse(Direction dir, uint32_t duration_ms);
    
    /**
     * @brief Disable all MOSFETs
     */
    void off();
    
    /**
     * @brief Check if H-bridge is currently active
     * @return true if pulse is in progress
     */
    bool isActive() const { return is_active_; }
    
private:
    // GPIO pins
    uint8_t pin_high_side_a_;
    uint8_t pin_low_side_a_;
    uint8_t pin_high_side_b_;
    uint8_t pin_low_side_b_;
    
    // State
    bool initialized_;
    bool is_active_;
    
    // Safety
    static constexpr uint32_t MAX_PULSE_DURATION_MS = 300;
    static constexpr uint32_t MOSFET_SWITCHING_DELAY_US = 10;  // Dead time between switching
    critical_section_t mutex_;
    
    /**
     * @brief Set all MOSFET pins to LOW (safe state)
     */
    void setAllPinsLow();
    
    /**
     * @brief Configure GPIO pin as output and set initial state
     */
    void configurePin(uint8_t pin);
};