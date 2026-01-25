#pragma once

#include <cstdint>
#include <functional>

/**
 * @brief Base states for all application modes
 *
 * These states represent the fundamental operational states that all modes
 * share. Mode-specific state machines extend this with additional states.
 */
enum class BaseState : uint8_t {
    INITIALIZING,   // Hardware setup in progress
    AWAITING_TIME,  // Waiting for RTC sync (from hub or PMU)
    OPERATIONAL,    // RTC running, mode can do work
    ERROR,          // Unrecoverable error
};

/**
 * @brief Hardware state inputs for base state machine
 *
 * State is derived from hardware - not persisted. This struct captures
 * the hardware checks needed to determine the current state.
 */
struct BaseHardwareState {
    bool rtc_running = false;  // rtc_running() hardware check
};

/**
 * @brief Base state machine for ApplicationMode
 *
 * Provides centralized state management based on hardware checks.
 * State is always derived from hardware - never persisted or guessed.
 *
 * Usage:
 *   BaseStateMachine state_machine;
 *   state_machine.setCallback([](BaseState state) {
 *       logger.info("State changed to: %s", BaseStateMachine::stateName(state));
 *   });
 *
 *   // In main loop or after hardware changes:
 *   BaseHardwareState hw;
 *   hw.rtc_running = rtc_running();
 *   state_machine.update(hw);
 *
 *   if (state_machine.isOperational()) {
 *       // Do work that requires RTC
 *   }
 */
class BaseStateMachine {
public:
    using StateCallback = std::function<void(BaseState)>;

    /**
     * @brief Update state based on current hardware status
     * @param hw Current hardware state
     *
     * State is always derived from hardware - this is the single update path.
     * If state changes, the callback is invoked.
     */
    void update(const BaseHardwareState &hw);

    /**
     * @brief Get current state
     * @return Current base state
     */
    BaseState state() const { return state_; }

    /**
     * @brief Check if in OPERATIONAL state
     * @return true if RTC is running and mode can do work
     */
    bool isOperational() const { return state_ == BaseState::OPERATIONAL; }

    /**
     * @brief Check if waiting for time sync
     * @return true if in AWAITING_TIME state
     */
    bool isAwaitingTime() const { return state_ == BaseState::AWAITING_TIME; }

    /**
     * @brief Check if in error state
     * @return true if in ERROR state
     */
    bool isError() const { return state_ == BaseState::ERROR; }

    /**
     * @brief Set callback for state changes
     * @param callback Function to call when state changes
     */
    void setCallback(StateCallback callback) { callback_ = std::move(callback); }

    /**
     * @brief Mark initialization complete
     *
     * Call this after hardware setup is done to transition from INITIALIZING.
     * Subsequent calls are no-ops.
     */
    void markInitialized() { initialized_ = true; }

    /**
     * @brief Mark as having an unrecoverable error
     *
     * Transitions to ERROR state. Only recoverable via reset.
     */
    void markError() { error_ = true; }

    /**
     * @brief Get human-readable state name
     * @param state State to get name for
     * @return State name string
     */
    static const char *stateName(BaseState state);

protected:
    /**
     * @brief Derive state from hardware and internal flags
     * @param hw Current hardware state
     * @return Derived state
     *
     * Override in derived classes to add mode-specific state logic.
     */
    virtual BaseState deriveState(const BaseHardwareState &hw) const;

    BaseState state_ = BaseState::INITIALIZING;
    StateCallback callback_;
    bool initialized_ = false;
    bool error_ = false;
};
