#pragma once

#include "base_state_machine.h"

/**
 * @brief Greenhouse-specific operational states
 */
enum class GreenhouseState : uint8_t {
    INITIALIZING,     // Hardware setup in progress
    AWAITING_TIME,    // Waiting for RTC sync
    IDLE,             // Curtain stationary, ready for commands
    CURTAIN_OPENING,  // Motor running in open direction
    CURTAIN_CLOSING,  // Motor running in close direction
    CALIBRATING,      // Calibration in progress
    ERROR,            // Unrecoverable error
};

/**
 * @brief Hardware state inputs for greenhouse state machine
 */
struct GreenhouseHardwareState : BaseHardwareState {
    bool opening = false;
    bool closing = false;
    bool calibrating = false;
};

/**
 * @brief State machine for GreenhouseMode
 *
 * Tracks the operational phase of the greenhouse curtain controller.
 */
class GreenhouseStateMachine {
public:
    using StateCallback = std::function<void(GreenhouseState)>;

    void update(const GreenhouseHardwareState &hardware_state);

    GreenhouseState state() const { return state_; }

    bool isOperational() const
    {
        return state_ == GreenhouseState::IDLE || state_ == GreenhouseState::CURTAIN_OPENING ||
               state_ == GreenhouseState::CURTAIN_CLOSING || state_ == GreenhouseState::CALIBRATING;
    }

    bool isIdle() const { return state_ == GreenhouseState::IDLE; }

    void setCallback(StateCallback callback) { callback_ = std::move(callback); }
    void markInitialized() { initialized_ = true; }
    void markError() { error_ = true; }

    static const char *stateName(GreenhouseState state);

private:
    GreenhouseState deriveState(const GreenhouseHardwareState &hardware_state) const;

    GreenhouseState state_ = GreenhouseState::INITIALIZING;
    StateCallback callback_;
    bool initialized_ = false;
    bool error_ = false;
};
