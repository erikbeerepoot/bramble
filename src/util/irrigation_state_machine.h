#pragma once

#include <cstdint>
#include <functional>

#include "mode_state_machine_base.h"

/**
 * @brief Irrigation-specific operational states
 *
 * States represent the current phase of the irrigation wake cycle:
 *
 * [Boot/Wake]
 *     │
 *     ▼
 * INITIALIZING ──► markInitialized() ──► AWAITING_TIME
 *                                             │
 *     ┌───────────────────────────────────────┘
 *     │                           │
 *     ▼ (no PMU time)             ▼ (PMU has time / hub response)
 * wait for hub              reportRtcSynced()
 *     │                           │
 *     ▼                           ▼
 * reportRtcSynced() ──────► IDLE
 *                             │
 *          ┌──────────────────┼──────────────────┐
 *          ▼                  ▼                   ▼
 *    VALVE_ACTIVE      CHECKING_UPDATES     (sleep)
 *          │                  │
 *          │                  ▼
 *          │           APPLYING_UPDATES
 *          │                  │
 *          ▼                  ▼
 *    reportValveClosed()  reportUpdatesComplete()
 *          │                  │
 *          └─────► IDLE ◄─────┘
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
 * @brief Event-driven state machine for IrrigationMode
 *
 * State is managed internally — report events and the state machine
 * updates automatically. Common lifecycle/RTC events inherited from base.
 *
 * Usage:
 *   IrrigationStateMachine sm;
 *   sm.setCallback([](IrrigationState s) { ... });
 *   sm.markInitialized();       // → AWAITING_TIME
 *   sm.reportRtcSynced();       // → IDLE
 *   sm.reportValveOpened();     // → VALVE_ACTIVE
 *   sm.reportValveClosed();     // → IDLE
 */
class IrrigationStateMachine : public ModeStateMachineBase {
public:
    using StateCallback = std::function<void(IrrigationState)>;

    // =========================================================================
    // Lifecycle Events (markInitialized, markError) inherited from base
    // =========================================================================

    // =========================================================================
    // Hardware Events
    // RTC events (reportRtcSynced, reportRtcLost) inherited from base
    // =========================================================================

    /**
     * @brief Report that a valve has been opened
     *
     * Transitions to VALVE_ACTIVE from any operational state.
     */
    void reportValveOpened();

    /**
     * @brief Report that all valves have been closed
     *
     * Transitions VALVE_ACTIVE → IDLE.
     */
    void reportValveClosed();

    // =========================================================================
    // Update Cycle Events
    // =========================================================================

    /**
     * @brief Report that an update pull cycle has started
     *
     * Call after successfully sending CHECK_UPDATES.
     * Transitions IDLE → CHECKING_UPDATES (no-op if already checking).
     */
    void reportUpdatePullStarted();

    /**
     * @brief Report that an update was received from hub
     *
     * Call when UPDATE_AVAILABLE with has_update=1 is received.
     * Transitions CHECKING_UPDATES → APPLYING_UPDATES.
     */
    void reportUpdateReceived();

    /**
     * @brief Report that a single update was applied to PMU
     *
     * Call after PMU callback confirms update applied.
     * Transitions APPLYING_UPDATES → CHECKING_UPDATES (pull next).
     */
    void reportUpdateApplied();

    /**
     * @brief Report that all updates are complete (has_update=0 or error)
     *
     * Transitions CHECKING_UPDATES/APPLYING_UPDATES → IDLE.
     */
    void reportUpdatesComplete();

    // =========================================================================
    // State Queries
    // =========================================================================

    IrrigationState state() const { return state_; }

    bool isOperational() const
    {
        return state_ == IrrigationState::IDLE || state_ == IrrigationState::CHECKING_UPDATES ||
               state_ == IrrigationState::APPLYING_UPDATES ||
               state_ == IrrigationState::VALVE_ACTIVE;
    }

    bool isTimeSynced() const
    {
        return state_ != IrrigationState::INITIALIZING &&
               state_ != IrrigationState::AWAITING_TIME && state_ != IrrigationState::ERROR;
    }

    bool isValveActive() const { return state_ == IrrigationState::VALVE_ACTIVE; }
    bool isIdle() const { return state_ == IrrigationState::IDLE; }
    bool isError() const { return state_ == IrrigationState::ERROR; }

    // =========================================================================
    // Callback
    // =========================================================================

    void setCallback(StateCallback callback) { callback_ = std::move(callback); }

    static const char *stateName(IrrigationState state);

protected:
    void updateState() override;

private:
    void transitionTo(IrrigationState newState);

    IrrigationState state_ = IrrigationState::INITIALIZING;
    StateCallback callback_;
};
