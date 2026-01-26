#pragma once

#include <cstdint>
#include <functional>

/**
 * @brief Sensor-specific states (wake-cycle-aware)
 *
 * States represent the current phase of the wake cycle:
 *
 * [Boot/Wake]
 *     │
 *     ▼
 * INITIALIZING ──► markInitialized() ──► AWAITING_TIME
 *                                             │
 *     ┌───────────────────────────────────────┘
 *     │                           │
 *     ▼ (no PMU time)             ▼ (PMU has time)
 * sendHeartbeat()           reportRtcSynced()
 *     │                           │
 *     ▼                           │
 * SYNCING_TIME ──────────────────►│
 *     │ (hub responds)            │
 *     ▼                           ▼
 * reportRtcSynced() ──────► TIME_SYNCED
 *                                │
 *                                ▼ (first boot)
 *                     reportSensorInit*()
 *                                │
 *             ┌──────────────────┴──────────────────┐
 *             ▼                                      ▼
 *         READING_SENSOR                    DEGRADED_NO_SENSOR
 *             │                                      │
 *             ▼ reportReadComplete()                 │
 *         CHECKING_BACKLOG ◄─────────────────────────┘
 *             │
 *             ├── (not time to TX) ──► LISTENING
 *             │
 *             ▼ (time to TX)
 *         TRANSMITTING
 *             │
 *             ▼ reportTransmitComplete()
 *         LISTENING ──(500ms timeout)──► READY_FOR_SLEEP
 *             │                              │
 *             │ reportListenComplete()       ▼
 *             └──────────────────────► signalReadyForSleep() ──► [PMU sleeps]
 */
enum class SensorState : uint8_t {
    INITIALIZING,        // Hardware setup in progress
    AWAITING_TIME,       // Need RTC sync (no valid time)
    SYNCING_TIME,        // Heartbeat sent, awaiting hub response
    TIME_SYNCED,         // RTC valid, sensor not yet attempted
    READING_SENSOR,      // Taking sensor reading
    CHECKING_BACKLOG,    // Deciding if transmission needed
    TRANSMITTING,        // Sending batch to hub
    LISTENING,           // Receive window open, awaiting hub responses
    READY_FOR_SLEEP,     // All wake work done
    DEGRADED_NO_SENSOR,  // Sensor failed, can still log timestamps
    ERROR,               // Unrecoverable error
};

/**
 * @brief Event-driven state machine for SensorMode
 *
 * State is managed internally - no external flags needed. Report hardware
 * events and the state machine updates automatically.
 *
 * Usage:
 *   SensorStateMachine state_machine;
 *   state_machine.setCallback([](SensorState state) {
 *       logger.info("State: %s", SensorStateMachine::stateName(state));
 *   });
 *
 *   // After hardware setup complete:
 *   state_machine.markInitialized();
 *
 *   // After RTC is set successfully:
 *   state_machine.reportRtcSynced();
 *
 *   // After sensor init:
 *   if (sensor->init()) {
 *       state_machine.reportSensorInitSuccess();
 *   } else {
 *       state_machine.reportSensorInitFailure();
 *   }
 *
 *   // Query state:
 *   if (state_machine.isTimeSynced()) {
 *       // Can store data with timestamps
 *   }
 *   if (state_machine.hasSensor()) {
 *       // Can read sensor
 *   }
 */
class SensorStateMachine {
public:
    using StateCallback = std::function<void(SensorState)>;

    // =========================================================================
    // Lifecycle Events
    // =========================================================================

    /**
     * @brief Mark hardware initialization complete
     *
     * Call after all hardware setup is done (flash, sensor object created, etc.)
     * Transitions from INITIALIZING to AWAITING_TIME.
     */
    void markInitialized();

    /**
     * @brief Mark unrecoverable error
     *
     * Transitions to ERROR state. Only recoverable via reset.
     */
    void markError();

    // =========================================================================
    // Hardware Events - report these when hardware state changes
    // =========================================================================

    /**
     * @brief Report that RTC has been successfully synchronized
     *
     * Call after rtc_set_datetime() succeeds.
     * Transitions to TIME_SYNCED (or OPERATIONAL/DEGRADED if sensor state known).
     */
    void reportRtcSynced();

    /**
     * @brief Report that RTC synchronization was lost
     *
     * Call if RTC stops running unexpectedly.
     * Transitions back to AWAITING_TIME.
     */
    void reportRtcLost();

    /**
     * @brief Report successful sensor initialization
     *
     * Call after sensor->init() returns true.
     * Transitions to READING_SENSOR if RTC is synced.
     */
    void reportSensorInitSuccess();

    /**
     * @brief Report failed sensor initialization
     *
     * Call after sensor->init() returns false.
     * Transitions to DEGRADED_NO_SENSOR (then to CHECKING_BACKLOG).
     */
    void reportSensorInitFailure();

    // =========================================================================
    // Wake Cycle Events - report these as work progresses
    // =========================================================================

    /**
     * @brief Report that heartbeat was sent for time sync
     *
     * Call after sendHeartbeat() when awaiting hub response.
     * Transitions AWAITING_TIME → SYNCING_TIME.
     */
    void reportHeartbeatSent();

    /**
     * @brief Report that sensor read is complete
     *
     * Call after readSensorData() finishes (success or failure).
     * Transitions READING_SENSOR → CHECKING_BACKLOG.
     */
    void reportReadComplete();

    /**
     * @brief Report that backlog check is complete
     *
     * @param needsTransmit true if there's data to transmit
     *
     * Call after checkBacklog() determines next action.
     * Transitions CHECKING_BACKLOG → TRANSMITTING or READY_FOR_SLEEP.
     */
    void reportCheckComplete(bool needsTransmit);

    /**
     * @brief Report that transmission is complete
     *
     * Call after transmitBatch() callback fires (success or failure).
     * Transitions TRANSMITTING → LISTENING.
     */
    void reportTransmitComplete();

    /**
     * @brief Report that listen window is complete
     *
     * Call after the receive window timeout expires.
     * Transitions LISTENING → READY_FOR_SLEEP.
     */
    void reportListenComplete();

    /**
     * @brief Report wake from sleep (restart the sensor cycle)
     *
     * Call when PMU signals a periodic wake. Restarts the cycle based on
     * current hardware state (RTC synced, sensor working, etc.).
     *
     * Transitions:
     * - If time synced + sensor working: → READING_SENSOR
     * - If time synced + sensor degraded: → CHECKING_BACKLOG
     * - If time synced + sensor not tried: → TIME_SYNCED (for sensor init)
     * - If time not synced: stays in current state (caller should sync time)
     *
     * @return true if cycle was restarted, false if time sync needed first
     */
    bool reportWakeFromSleep();

    // =========================================================================
    // State Queries - the ONLY way to check state
    // =========================================================================

    /**
     * @brief Get current state
     */
    SensorState state() const { return state_; }

    /**
     * @brief Check if RTC is synced (any state past SYNCING_TIME)
     */
    bool isTimeSynced() const
    {
        return state_ != SensorState::INITIALIZING && state_ != SensorState::AWAITING_TIME &&
               state_ != SensorState::SYNCING_TIME && state_ != SensorState::ERROR;
    }

    /**
     * @brief Check if can store data (RTC synced, regardless of sensor status)
     */
    bool canStoreData() const { return isTimeSynced(); }

    /**
     * @brief Check if sensor is available and working
     */
    bool hasSensor() const { return sensor_initialized_; }

    /**
     * @brief Check if sensor init should be attempted
     *
     * Returns true only if RTC is synced and sensor hasn't been tried yet.
     */
    bool needsSensorInit() const { return state_ == SensorState::TIME_SYNCED; }

    /**
     * @brief Check if in degraded mode (RTC ok, sensor failed)
     */
    bool isDegraded() const { return state_ == SensorState::DEGRADED_NO_SENSOR; }

    /**
     * @brief Check if waiting for time sync (AWAITING_TIME or SYNCING_TIME)
     */
    bool isAwaitingTime() const
    {
        return state_ == SensorState::AWAITING_TIME || state_ == SensorState::SYNCING_TIME;
    }

    /**
     * @brief Check if in error state
     */
    bool isError() const { return state_ == SensorState::ERROR; }

    /**
     * @brief Check if ready for sleep
     */
    bool isReadyForSleep() const { return state_ == SensorState::READY_FOR_SLEEP; }

    /**
     * @brief Check if currently transmitting
     */
    bool isTransmitting() const { return state_ == SensorState::TRANSMITTING; }

    /**
     * @brief Check if in listen window (awaiting hub responses before sleep)
     */
    bool isListening() const { return state_ == SensorState::LISTENING; }

    /**
     * @brief Get previous state (before last transition)
     *
     * Useful in state change callbacks to determine how we arrived
     * at the current state (e.g., TIME_SYNCED from SYNCING_TIME vs PMU path).
     */
    SensorState previousState() const { return previous_state_; }

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
    static const char *stateName(SensorState state);

private:
    /**
     * @brief Derive state from internal flags and transition if changed
     *
     * Used for flag-based states (INITIALIZING, AWAITING_TIME, TIME_SYNCED,
     * DEGRADED_NO_SENSOR, ERROR).
     */
    void updateState();

    /**
     * @brief Directly transition to a new state
     *
     * Used for workflow states (SYNCING_TIME, READING_SENSOR, CHECKING_BACKLOG,
     * TRANSMITTING, LISTENING, READY_FOR_SLEEP) that progress through explicit events.
     */
    void transitionTo(SensorState newState);

    SensorState state_ = SensorState::INITIALIZING;
    SensorState previous_state_ = SensorState::INITIALIZING;
    StateCallback callback_;

    // Internal state tracking - not accessible from outside
    bool initialized_ = false;
    bool error_ = false;
    bool rtc_synced_ = false;
    bool sensor_initialized_ = false;
    bool sensor_init_attempted_ = false;
};
