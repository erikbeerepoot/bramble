#pragma once

#include "base_state_machine.h"

/**
 * @brief Irrigation-specific operational states
 */
enum class IrrigationState : uint8_t {
    INITIALIZING,      // Hardware setup in progress
    AWAITING_TIME,     // Waiting for RTC sync
    IDLE,              // Ready for work, valve closed
    CHECKING_UPDATES,  // Querying hub for schedule updates
    APPLYING_UPDATES,  // Writing schedule to PMU
    VALVE_ACTIVE,      // Valve is open
    ERROR,             // Unrecoverable error
};

/**
 * @brief Hardware state inputs for irrigation state machine
 */
struct IrrigationHardwareState : BaseHardwareState {
    bool valve_open = false;
    bool update_pending = false;
    bool applying_update = false;
};

/**
 * @brief State machine for IrrigationMode
 *
 * Tracks the operational phase of the irrigation controller.
 * State is derived from hardware and operational flags.
 */
class IrrigationStateMachine {
public:
    using StateCallback = std::function<void(IrrigationState)>;

    /**
     * @brief Update state based on current hardware status
     */
    void update(const IrrigationHardwareState &hw);

    /**
     * @brief Get current state
     */
    IrrigationState state() const { return state_; }

    /**
     * @brief Check if operational (RTC synced, ready for work)
     */
    bool isOperational() const
    {
        return state_ == IrrigationState::IDLE || state_ == IrrigationState::CHECKING_UPDATES ||
               state_ == IrrigationState::APPLYING_UPDATES ||
               state_ == IrrigationState::VALVE_ACTIVE;
    }

    /**
     * @brief Check if time is synced
     */
    bool isTimeSynced() const
    {
        return state_ != IrrigationState::AWAITING_TIME && state_ != IrrigationState::INITIALIZING;
    }

    /**
     * @brief Check if valve is active
     */
    bool isValveActive() const { return state_ == IrrigationState::VALVE_ACTIVE; }

    /**
     * @brief Check if idle and ready for sleep
     */
    bool isIdle() const { return state_ == IrrigationState::IDLE; }

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
    static const char *stateName(IrrigationState state);

private:
    IrrigationState deriveState(const IrrigationHardwareState &hw) const;

    IrrigationState state_ = IrrigationState::INITIALIZING;
    StateCallback callback_;
    bool initialized_ = false;
    bool error_ = false;
};
