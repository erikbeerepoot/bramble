# Sensor Mode State Machine

## Problem Statement

The current SensorMode has scattered boolean flags (`rtc_synced_`, `sensor_initialized_`, `pmu_available_`, etc.) and conditional checks that create bugs:

1. **Invalid state combinations**: `rtc_synced_ = true` was set without `rtc_running()` being true
2. **Silent failures**: Sensor init failures weren't reported because the error check assumed init was attempted
3. **Scattered preconditions**: Each function checks its own preconditions inconsistently
4. **Hard to reason about**: No clear picture of valid states and transitions

The TaskQueue helps with task scheduling but doesn't enforce state invariants.

## Proposed Solution: State Machine

A state machine that:
1. Defines explicit states with clear semantics
2. Enforces preconditions on every transition
3. Centralizes state-dependent logic
4. Integrates with TaskQueue for scheduling state-triggered work

## State Definitions

```cpp
enum class SensorState : uint8_t {
    // === Startup States ===
    INITIALIZING,       // Just started, hardware not ready
    AWAITING_TIME,      // Waiting for time source (PMU or hub)

    // === Operational States ===
    TIME_SYNCED,        // RTC running, can store data, sensor may not be ready
    OPERATIONAL,        // Fully operational: time synced + sensor working

    // === Degraded States ===
    DEGRADED_NO_SENSOR, // Time works, but sensor failed (can still transmit old data)

    // === Terminal States ===
    ERROR_FATAL,        // Unrecoverable error (flash failure, etc.)
};
```

### State Invariants

| State | Invariants (must all be true) |
|-------|------------------------------|
| `INITIALIZING` | None |
| `AWAITING_TIME` | Hardware initialized |
| `TIME_SYNCED` | `rtc_running() == true` |
| `OPERATIONAL` | `rtc_running() == true` AND `sensor_initialized_ == true` |
| `DEGRADED_NO_SENSOR` | `rtc_running() == true` AND sensor init attempted but failed |
| `ERROR_FATAL` | Unrecoverable condition detected |

### State Transition Diagram

```
                    ┌─────────────────┐
                    │  INITIALIZING   │
                    └────────┬────────┘
                             │ hardware init complete
                             ▼
                    ┌─────────────────┐
                    │  AWAITING_TIME  │◄─────────────────┐
                    └────────┬────────┘                  │
                             │ rtc_set_datetime()        │ RTC stops
                             │ succeeds                  │ (error recovery)
                             ▼                           │
                    ┌─────────────────┐                  │
            ┌───────│   TIME_SYNCED   │──────────────────┘
            │       └────────┬────────┘
            │                │
            │    sensor init │
            │    ┌───────────┴───────────┐
            │    │                       │
            │    ▼ success               ▼ failure
    ┌───────┴────────┐          ┌─────────────────────┐
    │  OPERATIONAL   │          │  DEGRADED_NO_SENSOR │
    └───────┬────────┘          └──────────┬──────────┘
            │                              │
            │ sensor fails                 │ sensor recovers
            └──────────────────────────────┘

    Any State ──────► ERROR_FATAL (on unrecoverable error)
```

## Implementation Design

### SensorStateMachine Class

```cpp
// src/util/state_machine.h

class SensorStateMachine {
public:
    using StateChangeCallback = void (*)(SensorState old_state, SensorState new_state, void* context);

    explicit SensorStateMachine(Logger& logger);

    // === State Queries ===
    SensorState state() const { return state_; }
    const char* stateName() const;

    // Convenience checks
    bool canStoreData() const;      // TIME_SYNCED or better
    bool canReadSensor() const;     // OPERATIONAL
    bool isOperational() const;     // OPERATIONAL
    bool hasError() const;          // Any error/degraded state

    // === State Transitions ===
    // Each returns true if transition succeeded, false if preconditions not met

    bool transitionToAwaitingTime();
    bool transitionToTimeSynced();      // Checks rtc_running()
    bool transitionToOperational();     // Checks rtc_running() + sensor_initialized
    bool transitionToDegradedNoSensor();
    bool transitionToFatalError(const char* reason);

    // === Callbacks ===
    void setStateChangeCallback(StateChangeCallback callback, void* context);

    // === Error Flag Generation ===
    // Returns error flags based on current state (for heartbeat)
    uint16_t getErrorFlags() const;

private:
    SensorState state_ = SensorState::INITIALIZING;
    Logger& logger_;
    StateChangeCallback callback_ = nullptr;
    void* callback_context_ = nullptr;
    const char* fatal_reason_ = nullptr;

    bool checkPreconditions(SensorState target) const;
    void doTransition(SensorState new_state);
};
```

### Integration with SensorMode

```cpp
// sensor_mode.h
class SensorMode : public ApplicationMode {
private:
    SensorStateMachine state_machine_;

    // Remove scattered flags:
    // - bool sensor_initialized_;      // Now tracked by state machine
    // - bool sensor_init_attempted_;   // Now tracked by state machine
    // - bool rtc_synced_;              // Now tracked by state machine (via ApplicationMode)
```

### Example: Fixed RTC Sync Flow

**Before (buggy):**
```cpp
// Timeout path - sets flag without checking RTC
if (elapsed >= HEARTBEAT_TIMEOUT_MS) {
    rtc_synced_ = true;  // BUG: RTC may not be running!
    onRtcSynced();
}
```

**After (state machine):**
```cpp
// Timeout path - state machine enforces preconditions
if (elapsed >= HEARTBEAT_TIMEOUT_MS) {
    if (!state_machine_.transitionToTimeSynced()) {
        // Transition failed - RTC isn't actually running
        logger.error("Cannot proceed: RTC not running despite sync attempt");
        state_machine_.transitionToFatalError("RTC hardware failure");
        return;
    }
    onRtcSynced();
}
```

### Example: Sensor Data Storage

**Before:**
```cpp
void SensorMode::readAndStoreSensorData(uint32_t current_time) {
    if (!sensor_initialized_ && !tryInitSensor()) {
        return;  // Silent failure
    }
    if (!isRtcSynced()) {
        return;  // Another check
    }
    // ... store data
}
```

**After:**
```cpp
void SensorMode::readAndStoreSensorData(uint32_t current_time) {
    if (!state_machine_.canReadSensor()) {
        // State machine knows exactly why we can't read
        logger.debug("Cannot read sensor in state: %s", state_machine_.stateName());
        return;
    }
    // ... store data
}
```

### Error Flag Generation

The state machine centralizes error flag logic:

```cpp
uint16_t SensorStateMachine::getErrorFlags() const {
    uint16_t flags = ERR_FLAG_NONE;

    switch (state_) {
        case SensorState::AWAITING_TIME:
        case SensorState::INITIALIZING:
            flags |= ERR_FLAG_RTC_NOT_SYNCED;
            break;

        case SensorState::DEGRADED_NO_SENSOR:
            flags |= ERR_FLAG_SENSOR_FAILURE;
            break;

        case SensorState::ERROR_FATAL:
            flags |= ERR_FLAG_SENSOR_FAILURE;  // Or specific flag
            break;

        case SensorState::TIME_SYNCED:
        case SensorState::OPERATIONAL:
            // Check for transient issues
            if (!rtc_running()) {
                flags |= ERR_FLAG_RTC_NOT_SYNCED;
            }
            break;
    }

    return flags;
}
```

## TaskQueue Integration

The state machine and TaskQueue work together:

1. **State machine** enforces invariants and tracks current state
2. **TaskQueue** schedules work when states change

```cpp
void SensorMode::onStart() {
    // Set up state change handler to post tasks
    state_machine_.setStateChangeCallback([](SensorState old_state, SensorState new_state, void* ctx) {
        auto* self = static_cast<SensorMode*>(ctx);

        switch (new_state) {
            case SensorState::TIME_SYNCED:
                // Time is ready - try to init sensor
                self->task_queue_.post(&trySensorInit, self);
                break;

            case SensorState::OPERATIONAL:
                // Fully ready - start periodic sensor reads
                self->task_queue_.post(&startSensorReads, self);
                break;

            case SensorState::ERROR_FATAL:
                // Fatal error - signal for help
                self->task_queue_.post(&sendErrorHeartbeat, self);
                break;
        }
    }, this);
}
```

## Implementation Plan

### Phase 1: Core State Machine
1. Create `src/util/sensor_state_machine.h` and `.cpp`
2. Define states and transition methods
3. Implement precondition checks
4. Add state change callbacks
5. Unit tests for transitions

### Phase 2: SensorMode Integration
1. Add state machine to SensorMode
2. Replace `rtc_synced_` usage with state machine
3. Replace `sensor_initialized_` usage with state machine
4. Update `collectErrorFlags()` to use state machine
5. Fix the timeout bug (require valid transition)

### Phase 3: Simplification
1. Remove redundant boolean flags
2. Consolidate scattered precondition checks
3. Update logging to show state transitions
4. Integration tests

### Phase 4: IrrigationMode (Optional)
1. Create IrrigationStateMachine if beneficial
2. Similar pattern for irrigation-specific states

## Migration Strategy

To avoid breaking changes, migrate incrementally:

1. **Add state machine alongside existing flags**
   - State machine tracks state
   - Existing flags still work
   - Log warnings when they disagree

2. **Validate consistency**
   ```cpp
   void SensorMode::validateStateConsistency() {
       bool sm_can_store = state_machine_.canStoreData();
       bool flags_can_store = rtc_synced_ && rtc_running();
       if (sm_can_store != flags_can_store) {
           logger.warn("State inconsistency: SM=%d, flags=%d", sm_can_store, flags_can_store);
       }
   }
   ```

3. **Remove flags once confident**

## Success Criteria

1. No more invalid state combinations possible
2. Single source of truth for operational state
3. Error flags accurately reflect actual state
4. Clear logging of state transitions
5. Bugs like "rtc_synced but RTC not running" are impossible

## Open Questions

1. **Should ApplicationMode have a base state machine?**
   - Common states like AWAITING_TIME could be shared
   - Or keep mode-specific for simplicity

2. **Persistence across sleep cycles?**
   - State could be saved to flash for faster wake
   - Probably not needed - re-evaluate state on wake

3. **State machine for HubMode/IrrigationMode?**
   - Similar patterns exist there
   - Implement for SensorMode first, then evaluate
