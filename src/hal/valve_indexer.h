#pragma once

#include <cstdint>

#include "pico/critical_section.h"
#include "pico/stdlib.h"

/**
 * @brief Valve indexer for selecting individual valves
 *
 * Controls individual MOSFETs to enable current flow to specific valves.
 * Only one valve can be selected at a time for safety.
 *
 * In the circuit:
 * - Each valve has a series MOSFET controlled by this indexer
 * - When MOSFET is ON, valve can receive current from H-bridge
 * - When MOSFET is OFF, valve is isolated
 */
class ValveIndexer {
public:
    static constexpr uint8_t MAX_VALVES = 8;
    static constexpr uint8_t NO_VALVE_SELECTED = 0xFF;

    /**
     * @brief Initialize the valve indexer with GPIO pins
     * @param valve_pins Array of GPIO pins for valve MOSFETs
     * @param valve_count Number of valves (max 8)
     */
    void initialize(const uint8_t *valve_pins, uint8_t valve_count);

    /**
     * @brief Select a specific valve (enables its MOSFET)
     * @param valve_id Valve to select (0 to valve_count-1)
     *
     * Automatically deselects any previously selected valve
     */
    void selectValve(uint8_t valve_id);

    /**
     * @brief Deselect all valves (all MOSFETs OFF)
     */
    void deselectAll();

    /**
     * @brief Get currently selected valve
     * @return Selected valve ID or NO_VALVE_SELECTED if none
     */
    uint8_t getSelectedValve() const { return selected_valve_; }

    /**
     * @brief Check if a specific valve is currently selected
     * @param valve_id Valve to check
     * @return true if valve is selected
     */
    bool isValveSelected(uint8_t valve_id) const { return selected_valve_ == valve_id; }

    /**
     * @brief Get number of configured valves
     * @return Number of valves
     */
    uint8_t getValveCount() const { return valve_count_; }

private:
    uint8_t valve_pins_[MAX_VALVES];
    uint8_t valve_count_;
    uint8_t selected_valve_;
    bool initialized_;

    // Thread safety
    mutable critical_section_t mutex_;

    /**
     * @brief Configure GPIO pin as output
     */
    void configurePin(uint8_t pin);

    /**
     * @brief Validate valve ID is in range
     */
    bool isValidValveId(uint8_t valve_id) const { return valve_id < valve_count_; }
};