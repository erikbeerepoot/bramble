#pragma once

#include <cstdint>
#include <functional>

#include "../hal/pmu_protocol.h"

/**
 * @brief Irrigation-specific states (wake-cycle-aware)
 *
 * States represent the current phase of the wake cycle:
 *
 * [Boot/Wake]
 *     |
 *     v
 * INITIALIZING --> markInitialized() --> REGISTERING
 *                                             |
 *     +-------(callback checks registration)--+
 *     | (already registered)                  | (not registered)
 *     |                                       |
 *     |                         sendRegistrationRequest()
 *     |                                       |
 *     |                    +------------------+
 *     |                    | (timeout)        | (response received)
 *     |                    v                  v
 *     |            READY_FOR_SLEEP   reportRegistrationComplete()
 *     |                                       |
 *     +--------> AWAITING_TIME <--------------+
 *                     |
 *     +---------------+--------------+
 *     | (no PMU time)                | (PMU has time)
 *     v                              v
 * sendHeartbeat()          reportTimeSyncComplete()
 *     |                              |
 *     v                              |
 * SYNCING_TIME ---(hub responds)---->|
 *                                    v
 *                           CHECKING_UPDATES
 *                                    |
 *     +-----------------------------+-----------+
 *     | (has_update=true)                       | (has_update=false)
 *     v                                         v
 * APPLYING_UPDATE                        READY_FOR_SLEEP
 *     |                                         ^
 *     | (success)                               |
 *     +---> CHECKING_UPDATES (loop)             |
 *     |                                         |
 *     | (failure)                               |
 *     +-----------------------------------------+
 *
 * [Scheduled wake] --> VALVE_ACTIVE --> reportValveClosed() --> READY_FOR_SLEEP
 *
 * READY_FOR_SLEEP --> signalReadyForSleep() --> [PMU sleeps]
 *                 --> reportWakeFromSleep() --> AWAITING_TIME
 */
enum class IrrigationState : uint8_t {
    INITIALIZING,      // Hardware setup in progress
    REGISTERING,       // Waiting for hub registration response
    AWAITING_TIME,     // Need RTC sync (try PMU first, then hub)
    SYNCING_TIME,      // Heartbeat sent, awaiting hub time response
    CHECKING_UPDATES,  // CHECK_UPDATES sent, awaiting UPDATE_AVAILABLE
    APPLYING_UPDATE,   // PMU command in flight (schedule, datetime, wake interval)
    VALVE_ACTIVE,      // Valve open from scheduled wake
    READY_FOR_SLEEP,   // All wake work done, signal PMU to sleep
    ERROR,             // Unrecoverable error
};

/**
 * @brief Event-driven state machine for IrrigationMode
 *
 * State is managed internally - no external flags needed. Report events
 * and the state machine updates automatically.
 *
 * Usage:
 *   IrrigationStateMachine state_machine;
 *   state_machine.setCallback([](IrrigationState state) {
 *       // Drive side effects from state changes
 *   });
 *
 *   state_machine.markInitialized();
 *   // ... registration, time sync, update pull, sleep ...
 */
class IrrigationStateMachine {
public:
    using StateCallback = std::function<void(IrrigationState)>;

    // =========================================================================
    // Lifecycle Events
    // =========================================================================

    /**
     * @brief Mark hardware initialization complete
     *
     * Call after all hardware setup is done (PMU, valves, etc.)
     * Transitions INITIALIZING -> REGISTERING.
     */
    void markInitialized();

    /**
     * @brief Mark unrecoverable error
     *
     * Transitions to ERROR state. Only recoverable via reset.
     */
    void markError();

    // =========================================================================
    // Registration Events
    // =========================================================================

    /**
     * @brief Report that registration completed (request sent or already registered)
     *
     * Transitions REGISTERING -> AWAITING_TIME.
     */
    void reportRegistrationComplete();

    /**
     * @brief Report that registration timed out
     *
     * Transitions REGISTERING -> READY_FOR_SLEEP (retry on next wake).
     */
    void reportRegistrationTimeout();

    // =========================================================================
    // Time Sync Events
    // =========================================================================

    /**
     * @brief Report that heartbeat was sent for time sync
     *
     * Transitions AWAITING_TIME -> SYNCING_TIME.
     */
    void reportHeartbeatSent();

    /**
     * @brief Report that time sync is complete
     *
     * Call after RTC is set (from PMU or hub heartbeat response).
     * Transitions AWAITING_TIME/SYNCING_TIME -> CHECKING_UPDATES.
     */
    void reportTimeSyncComplete();

    /**
     * @brief Report that time sync timed out
     *
     * Transitions SYNCING_TIME -> READY_FOR_SLEEP (retry on next wake).
     */
    void reportSyncTimeout();

    // =========================================================================
    // Update Pull Events
    // =========================================================================

    /**
     * @brief Report that an UPDATE_AVAILABLE response was received
     *
     * @param hasMore true if an update payload is included to apply
     *
     * If hasMore: CHECKING_UPDATES -> APPLYING_UPDATE
     * If !hasMore: CHECKING_UPDATES -> READY_FOR_SLEEP
     */
    void reportUpdateReceived(bool hasMore);

    /**
     * @brief Report that an update was successfully applied to PMU
     *
     * Transitions APPLYING_UPDATE -> CHECKING_UPDATES (to pull next update).
     */
    void reportUpdateApplied();

    /**
     * @brief Report that an update check or apply failed
     *
     * Transitions CHECKING_UPDATES/APPLYING_UPDATE -> READY_FOR_SLEEP.
     */
    void reportUpdateFailed();

    // =========================================================================
    // Valve Events
    // =========================================================================

    /**
     * @brief Report that a valve was opened (scheduled wake)
     *
     * Transitions to VALVE_ACTIVE from any operational state.
     */
    void reportValveOpened();

    /**
     * @brief Report that all valves are closed
     *
     * Transitions VALVE_ACTIVE -> READY_FOR_SLEEP.
     */
    void reportValveClosed();

    // =========================================================================
    // Sleep/Wake Events
    // =========================================================================

    /**
     * @brief Report wake from PMU sleep
     *
     * @param reason The PMU wake reason
     *
     * For Periodic/External: READY_FOR_SLEEP -> AWAITING_TIME
     * For Scheduled: READY_FOR_SLEEP -> VALVE_ACTIVE
     */
    void reportWakeFromSleep(PMU::WakeReason reason);

    /**
     * @brief Report that a per-state watchdog fired (generic timeout)
     *
     * Forces transition to READY_FOR_SLEEP from any state.
     */
    void reportWatchdogTimeout();

    // =========================================================================
    // State Queries
    // =========================================================================

    /**
     * @brief Get current state
     */
    IrrigationState state() const { return state_; }

    /**
     * @brief Get previous state (before last transition)
     */
    IrrigationState previousState() const { return previous_state_; }

    /**
     * @brief Check if operational (past time sync, doing useful work)
     */
    bool isOperational() const
    {
        return state_ == IrrigationState::CHECKING_UPDATES ||
               state_ == IrrigationState::APPLYING_UPDATE ||
               state_ == IrrigationState::VALVE_ACTIVE;
    }

    /**
     * @brief Check if RTC is synced (any state past SYNCING_TIME)
     */
    bool isTimeSynced() const
    {
        return state_ != IrrigationState::INITIALIZING && state_ != IrrigationState::REGISTERING &&
               state_ != IrrigationState::AWAITING_TIME &&
               state_ != IrrigationState::SYNCING_TIME && state_ != IrrigationState::ERROR;
    }

    /**
     * @brief Check if valve is active
     */
    bool isValveActive() const { return state_ == IrrigationState::VALVE_ACTIVE; }

    /**
     * @brief Check if ready for sleep
     */
    bool isReadyForSleep() const { return state_ == IrrigationState::READY_FOR_SLEEP; }

    /**
     * @brief Check if in error state
     */
    bool isError() const { return state_ == IrrigationState::ERROR; }

    /**
     * @brief Check if waiting for registration
     */
    bool isRegistering() const { return state_ == IrrigationState::REGISTERING; }

    // =========================================================================
    // Callback
    // =========================================================================

    /**
     * @brief Set callback for state changes
     */
    void setCallback(StateCallback callback) { callback_ = std::move(callback); }

    /**
     * @brief Get human-readable state name
     */
    static const char *stateName(IrrigationState state);

private:
    void transitionTo(IrrigationState newState);

    IrrigationState state_ = IrrigationState::INITIALIZING;
    IrrigationState previous_state_ = IrrigationState::INITIALIZING;
    StateCallback callback_;
    bool initialized_ = false;
    bool error_ = false;
};
