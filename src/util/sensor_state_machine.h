#pragma once

#include "base_state_machine.h"

/**
 * @brief Sensor-specific states extending base states
 */
enum class SensorState : uint8_t {
    INITIALIZING,        // Hardware setup in progress
    AWAITING_TIME,       // Waiting for RTC sync (from hub or PMU)
    TIME_SYNCED,         // RTC running, can store data, sensor not yet attempted
    OPERATIONAL,         // RTC running, sensor working
    DEGRADED_NO_SENSOR,  // RTC running but sensor init attempted and failed
    ERROR,               // Unrecoverable error
};

/**
 * @brief Hardware state inputs for sensor state machine
 */
struct SensorHardwareState : BaseHardwareState {
    bool sensor_initialized = false;
    bool sensor_init_attempted = false;
};

/**
 * @brief State machine for SensorMode
 *
 * Extends base state machine with sensor-specific states.
 * State is always derived from hardware checks.
 *
 * Usage:
 *   SensorStateMachine state_machine;
 *   state_machine.setCallback([](SensorState state) {
 *       logger.info("State: %s", SensorStateMachine::stateName(state));
 *   });
 *
 *   // After sensor init attempt:
 *   SensorHardwareState hardware_state;
 *   hardware_state.rtc_running = rtc_running();
 *   hardware_state.sensor_initialized = sensor_->init();
 *   hardware_state.sensor_init_attempted = true;
 *   state_machine.update(hardware_state);
 */
class SensorStateMachine {
public:
    using StateCallback = std::function<void(SensorState)>;

    /**
     * @brief Update state based on current hardware status
     * @param hardware_state Current hardware state
     */
    void update(const SensorHardwareState &hardware_state);

    /**
     * @brief Get current state
     */
    SensorState state() const { return state_; }

    /**
     * @brief Check if in OPERATIONAL state (RTC + sensor working)
     */
    bool isOperational() const { return state_ == SensorState::OPERATIONAL; }

    /**
     * @brief Check if RTC is synced (any state with valid time)
     */
    bool isTimeSynced() const
    {
        return state_ == SensorState::TIME_SYNCED || state_ == SensorState::OPERATIONAL ||
               state_ == SensorState::DEGRADED_NO_SENSOR;
    }

    /**
     * @brief Check if can store data (RTC synced, regardless of sensor status)
     */
    bool canStoreData() const { return isTimeSynced(); }

    /**
     * @brief Check if sensor is available and working
     */
    bool hasSensor() const { return state_ == SensorState::OPERATIONAL; }

    /**
     * @brief Check if sensor init was attempted but not yet successful
     * Use this to decide whether to attempt sensor initialization.
     */
    bool needsSensorInit() const
    {
        return state_ == SensorState::TIME_SYNCED;
    }

    /**
     * @brief Check if in degraded mode (RTC ok, sensor failed)
     */
    bool isDegraded() const { return state_ == SensorState::DEGRADED_NO_SENSOR; }

    /**
     * @brief Check if waiting for time sync
     */
    bool isAwaitingTime() const { return state_ == SensorState::AWAITING_TIME; }

    /**
     * @brief Check if in error state
     */
    bool isError() const { return state_ == SensorState::ERROR; }

    /**
     * @brief Set callback for state changes
     */
    void setCallback(StateCallback callback) { callback_ = std::move(callback); }

    /**
     * @brief Mark initialization complete
     */
    void markInitialized() { initialized_ = true; }

    /**
     * @brief Mark as having an unrecoverable error
     */
    void markError() { error_ = true; }

    /**
     * @brief Get human-readable state name
     */
    static const char *stateName(SensorState state);

private:
    SensorState deriveState(const SensorHardwareState &hardware_state) const;

    SensorState state_ = SensorState::INITIALIZING;
    StateCallback callback_;
    bool initialized_ = false;
    bool error_ = false;
};
