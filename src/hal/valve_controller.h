#pragma once

#include <cstdint>
#include <memory>

#include "hardware/gpio.h"
#include "pico/critical_section.h"

#include "../board/board_pins.h"
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
 * @brief AC solenoid valve driver (SSR-switched)
 *
 * One GPIO per valve drives a solid-state relay: high = SSR on = valve open.
 * The valve stays open only while the GPIO is driven, so these valves require
 * continuous power (the node must not sleep while one is open). No H-bridge or
 * valve indexer — each SSR is independent and several may be on simultaneously.
 */
class ACValveDriver : public ValveDriver {
public:
    explicit ACValveDriver(uint8_t gpio_pin) : pin_(gpio_pin) {}

    void open() override { gpio_put(pin_, 1); }
    void close() override { gpio_put(pin_, 0); }

    bool requiresContinuousPower() override { return true; }
    ValveType getType() override { return ValveType::AC_SOLENOID; }

private:
    uint8_t pin_;
};

/**
 * @brief High-level valve controller
 *
 * Provides simple interface for controlling irrigation valves.
 * Manages H-bridge, valve indexer, and state tracking.
 */
class ValveController {
public:
    static constexpr uint8_t NUM_VALVES = Board::NUM_VALVES;

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
     * Skips valves already known to be CLOSED. Use for runtime shutoff
     * where the in-memory state is authoritative.
     */
    void closeAllValves();

    /**
     * @brief Force-close all valves regardless of cached state
     *
     * Pulses every valve's close coil unconditionally so the physical
     * state is reconciled with the firmware's idea of CLOSED. Use at
     * boot — the in-memory state may have been restored from PMU RAM
     * (so closeAllValves would no-op) but the latching valves could
     * still be mechanically open from a pre-power-loss command.
     */
    void forceCloseAllValves();

    /**
     * @brief Restore a valve's in-memory state without actuating hardware
     *
     * Used to restore known state from persisted storage (e.g. PMU RAM)
     * after a warm wake. DC latching valves hold position during sleep,
     * so no actuation is needed.
     *
     * @param valve_id Valve to restore
     * @param state State to set
     */
    void restoreValveState(uint8_t valve_id, ValveState state);

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
     * @brief Whether this controller's valves require continuous power
     *
     * True for AC SSR valves (the node must stay awake while any valve is
     * open), false for DC latching valves. A node is pure-AC or pure-DC, so
     * valve 0's driver is representative.
     */
    bool usesContinuousPower() const
    {
        return initialized_ && drivers_[0] && drivers_[0]->requiresContinuousPower();
    }

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

    // Pin assignments come from Board:: namespace (board-specific)

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