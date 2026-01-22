#pragma once

#include <cstdint>
#include <memory>

#include "pico/critical_section.h"

#include "hbridge.h"
#include "valve_indexer.h"

/**
 * @brief Valve states
 */
enum class ValveState : uint8_t {
    CLOSED = 0,
    OPEN = 1,
    UNKNOWN = 2  // Used at startup before first command
};

/**
 * @brief Valve types for future extensibility
 */
enum class ValveType : uint8_t {
    DC_LATCHING = 0,  // Current implementation
    AC_SOLENOID = 1   // Future support
};

/**
 * @brief Abstract valve driver interface
 */
class ValveDriver {
public:
    virtual ~ValveDriver() = default;
    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool requiresContinuousPower() = 0;
    virtual ValveType getType() = 0;
};

/**
 * @brief DC Latching solenoid valve driver
 *
 * Uses H-bridge to send pulses in forward/reverse direction
 * to latch/unlatch the valve
 */
class DCLatchingDriver : public ValveDriver {
public:
    DCLatchingDriver(HBridge *hbridge, uint32_t pulse_duration_ms = 250)
        : hbridge_(hbridge), pulse_duration_ms_(pulse_duration_ms)
    {
    }

    void open() override { hbridge_->pulse(HBridge::FORWARD, pulse_duration_ms_); }

    void close() override { hbridge_->pulse(HBridge::REVERSE, pulse_duration_ms_); }

    bool requiresContinuousPower() override { return false; }
    ValveType getType() override { return ValveType::DC_LATCHING; }

private:
    HBridge *hbridge_;
    uint32_t pulse_duration_ms_;
};

/**
 * @brief High-level valve controller
 *
 * Provides simple interface for controlling irrigation valves.
 * Manages H-bridge, valve indexer, and state tracking.
 */
class ValveController {
public:
    static constexpr uint8_t NUM_VALVES = 2;  // Currently only 2 valves connected

    /**
     * @brief Constructor - sets initial state
     */
    ValveController() : initialized_(false) {}

    /**
     * @brief Initialize the valve controller
     *
     * Sets up H-bridge pins, valve indexer pins, and initializes
     * all valves as DC latching type in CLOSED state
     */
    void initialize();

    /**
     * @brief Open a specific valve
     * @param valve_id Valve to open (0 to NUM_VALVES-1)
     */
    void openValve(uint8_t valve_id);

    /**
     * @brief Close a specific valve
     * @param valve_id Valve to close (0 to NUM_VALVES-1)
     */
    void closeValve(uint8_t valve_id);

    /**
     * @brief Close all valves
     *
     * Useful for emergency shutoff or initialization
     */
    void closeAllValves();

    /**
     * @brief Get current state of a valve
     * @param valve_id Valve to query
     * @return Current valve state
     */
    ValveState getValveState(uint8_t valve_id) const;

    /**
     * @brief Get bitmask of open valves
     * @return Bitmask where bit N is 1 if valve N is open
     */
    uint8_t getActiveValveMask() const;

    /**
     * @brief Set valve type (for future AC valve support)
     * @param valve_id Valve to configure
     * @param type Valve type
     */
    void setValveType(uint8_t valve_id, ValveType type);

    /**
     * @brief Get current reading for a valve (stub)
     * @param valve_id Valve to query
     * @return Current in mA (always returns 0.0 for now)
     */
    float getValveCurrent(uint8_t valve_id) const { return 0.0; }

    /**
     * @brief Check if valve has a fault (stub)
     * @param valve_id Valve to check
     * @return false (no fault detection yet)
     */
    bool detectValveFault(uint8_t valve_id) const { return false; }

private:
    // Hardware components
    HBridge hbridge_;
    ValveIndexer indexer_;

    // Valve drivers and state
    std::unique_ptr<ValveDriver> drivers_[NUM_VALVES];
    ValveState valve_states_[NUM_VALVES];

    // Configuration
    bool initialized_;

    // Thread safety
    mutable critical_section_t mutex_;

    // GPIO pin assignments
    static constexpr uint8_t PIN_MOTOR_LO_1 = 26;  // A0 pin
    static constexpr uint8_t PIN_MOTOR_LO_2 = 27;  // A1 pin
    static constexpr uint8_t PIN_MOTOR_HI_1 = 28;  // A2 pin
    static constexpr uint8_t PIN_MOTOR_HI_2 = 29;  // A3 pin

    static constexpr uint8_t VALVE_PINS[NUM_VALVES] = {24, 25};  // GPIO24 and GPIO25

    /**
     * @brief Ensure controller is initialized before use
     */
    void ensureInitialized() const;

    /**
     * @brief Check if valve ID is valid
     */
    bool isValidValveId(uint8_t valve_id) const { return valve_id < NUM_VALVES; }

    /**
     * @brief Operate a valve (internal helper)
     * @param valve_id Valve to operate
     * @param open true to open, false to close
     */
    void operateValve(uint8_t valve_id, bool open);
};