# State Machine Inheritance Refactor

## Problem Statement

Currently there are two independent state machines running in `SensorMode`:
1. `BaseStateMachine` (from `ApplicationMode`) - logs as `[StateMachine]`
2. `SensorStateMachine` (in `SensorMode`) - logs as `[SensorSM]`

This causes duplicate log lines like:
```
[+319ms] INFO [StateMachine]: State: INITIALIZING -> AWAITING_TIME
[+400ms] INFO [SensorSM]: State: INITIALIZING -> AWAITING_TIME
```

The design intent was for `SensorStateMachine` to inherit from `BaseStateMachine`, but they're currently separate classes with different paradigms.

## Key Architectural Differences

| Aspect | BaseStateMachine | SensorStateMachine |
|--------|-----------------|-------------------|
| **Pattern** | Hardware-polling | Event-driven |
| **States** | 4 (INITIALIZING, AWAITING_TIME, OPERATIONAL, ERROR) | 11 (wake-cycle workflow states) |
| **Update** | `update(BaseHardwareState)` | `reportXyz()` event methods |
| **State derivation** | `deriveState()` virtual method | Mixed: `updateState()` + `transitionTo()` |

## Design Decision

**Option A: True Inheritance** - SensorStateMachine extends BaseStateMachine
- Requires reconciling hardware-polling vs event-driven
- Complex due to different state enums

**Option B: Disable Base in SensorMode** - SensorMode skips BaseStateMachine updates
- Simple fix but doesn't address design intent
- Leaves dead code

**Option C: State Mapping Bridge** (Recommended)
- SensorStateMachine maps its states to BaseState equivalents
- Notifies BaseStateMachine when base-level state changes
- Single source of truth (SensorStateMachine), no duplicate transitions
- ApplicationMode can still query base state if needed

## Recommended Approach: Option C

### Concept

SensorStateMachine internally tracks the equivalent `BaseState` and can report it:

```cpp
class SensorStateMachine {
public:
    // Map sensor state to base state
    BaseState baseState() const {
        switch (state_) {
            case SensorState::INITIALIZING:
                return BaseState::INITIALIZING;
            case SensorState::AWAITING_TIME:
            case SensorState::SYNCING_TIME:
                return BaseState::AWAITING_TIME;
            case SensorState::ERROR:
                return BaseState::ERROR;
            default:  // All operational states
                return BaseState::OPERATIONAL;
        }
    }
};
```

### Changes Required

#### 1. `src/util/sensor_state_machine.h`
- Add `#include "base_state_machine.h"` for `BaseState` enum
- Add `baseState()` method to map SensorState → BaseState
- Add optional `BaseStateCallback` for notifying when base-level state changes

#### 2. `src/util/sensor_state_machine.cpp`
- Implement `baseState()` mapping
- In `transitionTo()`: check if `baseState()` changed, call base callback if set

#### 3. `src/modes/application_mode.h`
- Make `state_machine_` protected (currently private) so subclasses can access
- Or add `baseStateMachine()` accessor

#### 4. `src/modes/application_mode.cpp`
- In `run()`: check if subclass has its own state machine before calling `updateStateMachine()`
- Or: make `updateStateMachine()` virtual so subclasses can override

#### 5. `src/modes/sensor_mode.cpp`
- Option A: Override base state machine updates to be no-op
- Option B: Connect `sensor_state_` to notify `state_machine_` when base state changes
- Remove duplicate state transition logging

## Implementation Plan

### Step 1: Add baseState() mapping to SensorStateMachine
```cpp
// sensor_state_machine.h
#include "base_state_machine.h"

class SensorStateMachine {
public:
    BaseState baseState() const;
    // ...
};

// sensor_state_machine.cpp
BaseState SensorStateMachine::baseState() const {
    switch (state_) {
        case SensorState::INITIALIZING:
            return BaseState::INITIALIZING;
        case SensorState::AWAITING_TIME:
        case SensorState::SYNCING_TIME:
            return BaseState::AWAITING_TIME;
        case SensorState::ERROR:
            return BaseState::ERROR;
        default:
            return BaseState::OPERATIONAL;
    }
}
```

### Step 2: Prevent duplicate BaseStateMachine transitions in SensorMode

In `ApplicationMode::run()`, add a virtual method that subclasses can override:

```cpp
// application_mode.h
protected:
    virtual bool usesCustomStateMachine() const { return false; }

// application_mode.cpp - in run()
if (!usesCustomStateMachine()) {
    state_machine_.markInitialized();
    updateStateMachine();
}

// sensor_mode.h
protected:
    bool usesCustomStateMachine() const override { return true; }
```

### Step 3: (Optional) Bridge callback for base state changes

If ApplicationMode needs to react to base-level state changes from sensor:

```cpp
// In SensorMode::onStart()
sensor_state_.setBaseStateCallback([this](BaseState base) {
    // Could update ApplicationMode's state_machine_ if needed
    // Or just use for logging coordination
});
```

## Files to Modify

1. `src/util/sensor_state_machine.h` - Add baseState() method
2. `src/util/sensor_state_machine.cpp` - Implement baseState()
3. `src/modes/application_mode.h` - Add usesCustomStateMachine() virtual
4. `src/modes/application_mode.cpp` - Check before base state machine init
5. `src/modes/sensor_mode.h` - Override usesCustomStateMachine()

## Verification

1. Build: `/bramble-build SENSOR`
2. Flash and observe logs - should see only `[SensorSM]` state transitions, not duplicate `[StateMachine]` lines
3. Verify wake cycle works correctly:
   - Boot → INITIALIZING → AWAITING_TIME (after PMU state restored)
   - Time sync → TIME_SYNCED
   - Sensor read → READING_SENSOR → CHECKING_BACKLOG
   - TX if needed → TRANSMITTING → READY_FOR_SLEEP
4. Verify `sensor_state_.baseState()` returns correct base state at each point
