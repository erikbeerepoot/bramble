# Internal Task Queue Design

## Overview

This document proposes a lightweight internal task queue to replace the current scattered work coordination patterns. The goal is to unify work tracking, simplify callback chains, and provide a single point of control for deferred execution.

## Current State Analysis

### Existing Mechanisms

| Component | Purpose | Pain Points |
|-----------|---------|-------------|
| `WorkTracker` | Bitmask-based work tracking with idle callback | Manual `checkIdle()` calls, fixed enum types, no dependencies |
| `PeriodicTaskManager` | Interval-based background tasks | Only periodic, no one-shot tasks |
| `ReliableMessenger` | Message queue with retry | Own queue/retry logic, separate from other work |
| `HubRouter` | Message forwarding queue | Yet another queue implementation |
| Deferred flags | `sleep_requested_` pattern | Scattered, ad-hoc |

### Current Main Loop (ApplicationMode)

```cpp
void ApplicationMode::run() {
    onStart();
    task_manager_.start();

    while (true) {
        onLoop();                              // Mode-specific hook
        task_manager_.update(current_time);   // Periodic tasks

        if (lora_.isInterruptPending()) { ... }
        if (lora_.isMessageReady()) { ... }

        messenger_.update();                  // LoRa message queue
        hub_router_->processQueuedMessages(); // Forwarding queue

        if (shouldSleep()) sleep_ms(10);
    }
}
```

**Problems:**
1. Five separate `update()` calls
2. No unified priority system
3. Callback nesting (heartbeat → PMU sync → RTC sync → backlog)
4. Manual deferred execution patterns

## Proposed Design

### Design Principles

1. **Static allocation** - No dynamic memory after init (RP2040-safe)
2. **Simple** - One queue, one `process()` call
3. **Composable** - Tasks can post other tasks
4. **Minimal overhead** - No vtables, lightweight task struct

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Modes                         │
│           (IrrigationMode, SensorMode, etc.)                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      TaskQueue (NEW)                         │
│   - Static task slot array (16 slots)                       │
│   - Priority levels (HIGH, NORMAL, LOW)                     │
│   - Deferred execution support                              │
│   - Optional completion callbacks                           │
└─────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │  LoRa    │   │   PMU    │   │ Periodic │
        │ Messages │   │ Commands │   │  Tasks   │
        └──────────┘   └──────────┘   └──────────┘
```

### Task Structure

```cpp
// Priority levels
enum class TaskPriority : uint8_t {
    High = 0,    // Critical: ACKs, time-sensitive responses
    Normal = 1,  // Standard: sensor reads, transmissions
    Low = 2,     // Background: statistics, housekeeping
};

// Task state
enum class TaskState : uint8_t {
    Empty = 0,      // Slot available
    Pending,        // Waiting to run
    Ready,          // Eligible to run (delay expired, deps met)
    Running,        // Currently executing
};

// Compact task representation (fits in ~32 bytes)
struct Task {
    TaskFunction function;      // 4 bytes (function pointer)
    void* context;              // 4 bytes (user data)
    uint32_t run_after;         // 4 bytes (earliest run time, 0 = immediate)
    uint16_t id;                // 2 bytes (for dependencies/cancellation)
    uint16_t depends_on;        // 2 bytes (task ID that must complete first, 0 = none)
    TaskPriority priority;      // 1 byte
    TaskState state;            // 1 byte
    uint8_t reserved[2];        // Padding to 4-byte alignment
};
```

### TaskQueue Class

```cpp
class TaskQueue {
public:
    // Task function signature: returns true if complete, false to re-queue
    using TaskFunction = bool (*)(void* context, uint32_t current_time);
    using CompletionCallback = void (*)(uint16_t task_id, void* context);

    static constexpr size_t MAX_TASKS = 16;
    static constexpr uint16_t INVALID_ID = 0;

    TaskQueue();

    // === Task Submission ===

    // Post a task for immediate execution
    uint16_t post(TaskFunction func, void* context = nullptr,
                  TaskPriority priority = TaskPriority::Normal);

    // Post a task to run after a delay
    uint16_t postDelayed(TaskFunction func, void* context,
                         uint32_t delay_ms,
                         TaskPriority priority = TaskPriority::Normal);

    // Post a task that depends on another task completing
    uint16_t postAfter(TaskFunction func, void* context,
                       uint16_t depends_on,
                       TaskPriority priority = TaskPriority::Normal);

    // === Task Management ===

    // Cancel a pending task (returns false if already running/completed)
    bool cancel(uint16_t task_id);

    // Check if a task is still pending or running
    bool isActive(uint16_t task_id) const;

    // Set callback for when a task completes
    void onComplete(uint16_t task_id, CompletionCallback callback, void* context);

    // === Processing ===

    // Process ready tasks (call from main loop)
    // Returns number of tasks executed
    size_t process(uint32_t current_time);

    // === Status ===

    size_t pendingCount() const;
    size_t availableSlots() const;
    bool isEmpty() const;
    bool isFull() const;

private:
    Task tasks_[MAX_TASKS];
    uint16_t next_id_;           // Monotonic ID counter (wraps, skips 0)

    // Completion callback storage (separate to keep Task struct small)
    struct CompletionEntry {
        uint16_t task_id;
        CompletionCallback callback;
        void* context;
    };
    CompletionEntry completions_[MAX_TASKS / 2];  // Fewer completion callbacks needed

    Task* findSlot();
    Task* findById(uint16_t id);
    void markComplete(uint16_t task_id);
    bool areDependenciesMet(const Task& task) const;
};
```

### Usage Examples

#### Basic Task Posting

```cpp
TaskQueue queue;

// Simple one-shot task
queue.post([](void* ctx, uint32_t time) -> bool {
    Logger::info("Task executed at %u", time);
    return true;  // Complete
});

// Task with context
SensorData* data = &sensor_reading;
queue.post([](void* ctx, uint32_t time) -> bool {
    SensorData* d = static_cast<SensorData*>(ctx);
    transmitReading(*d);
    return true;
}, data);
```

#### Delayed Execution

```cpp
// Send sleep signal after 100ms (deferred from callback)
queue.postDelayed([](void* ctx, uint32_t time) -> bool {
    ReliablePmuClient* pmu = static_cast<ReliablePmuClient*>(ctx);
    pmu->readyForSleep(nullptr);
    return true;
}, reliable_pmu_, 100);
```

#### Task Dependencies

```cpp
// RTC sync must complete before backlog transmission
uint16_t sync_task = queue.post([](void* ctx, uint32_t time) -> bool {
    // Sync RTC...
    return rtc_synced;
}, nullptr, TaskPriority::High);

queue.postAfter([](void* ctx, uint32_t time) -> bool {
    // Transmit backlog (only runs after sync_task completes)
    return transmitBacklog();
}, nullptr, sync_task);
```

#### Retry Pattern

```cpp
// Task that retries until successful
queue.post([](void* ctx, uint32_t time) -> bool {
    if (sendMessage()) {
        return true;   // Complete
    }
    return false;      // Re-queue for retry
});
```

### Integration with Existing Code

#### Replacing WorkTracker

**Before (WorkTracker):**
```cpp
work_tracker_.addWork(WorkType::RtcSync);
work_tracker_.addWork(WorkType::BacklogTransmit);

void onRtcSynced() {
    work_tracker_.completeWork(WorkType::RtcSync);
    checkAndTransmitBacklog();
}

void onLoop() {
    work_tracker_.checkIdle();
}
```

**After (TaskQueue):**
```cpp
uint16_t rtc_task = queue_.post(&syncRtc, this, TaskPriority::High);
queue_.postAfter(&transmitBacklog, this, rtc_task);
queue_.onComplete(/* last task */, &signalReadyForSleep, this);
```

#### Replacing Deferred Flags

**Before:**
```cpp
// In callback (can't call PMU directly - stack depth)
sleep_requested_ = true;

// In onLoop()
if (sleep_requested_) {
    sleep_requested_ = false;
    reliable_pmu_->readyForSleep(...);
}
```

**After:**
```cpp
// In callback - post deferred task
queue_.postDelayed(&sendSleepSignal, this, 0);  // 0ms = next loop iteration
```

#### Simplified Main Loop

**Before:**
```cpp
while (true) {
    onLoop();
    task_manager_.update(current_time);
    // ... LoRa handling ...
    messenger_.update();
    hub_router_->processQueuedMessages();
    work_tracker_.checkIdle();
}
```

**After:**
```cpp
while (true) {
    // ... LoRa handling (still event-driven) ...
    queue_.process(current_time);  // Single call handles everything
}
```

### Priority Scheduling

Tasks execute in priority order within each `process()` call:

```cpp
size_t TaskQueue::process(uint32_t current_time) {
    size_t executed = 0;

    // Process by priority: High → Normal → Low
    for (int p = 0; p <= static_cast<int>(TaskPriority::Low); p++) {
        for (auto& task : tasks_) {
            if (task.state == TaskState::Pending &&
                task.priority == static_cast<TaskPriority>(p) &&
                current_time >= task.run_after &&
                areDependenciesMet(task)) {

                task.state = TaskState::Running;
                bool complete = task.function(task.context, current_time);

                if (complete) {
                    markComplete(task.id);
                    task.state = TaskState::Empty;
                } else {
                    task.state = TaskState::Pending;  // Re-queue
                }
                executed++;
            }
        }
    }
    return executed;
}
```

### Memory Budget

| Component | Size | Count | Total |
|-----------|------|-------|-------|
| Task struct | 20 bytes | 16 | 320 bytes |
| CompletionEntry | 10 bytes | 8 | 80 bytes |
| Queue overhead | ~20 bytes | 1 | 20 bytes |
| **Total** | | | **~420 bytes** |

This is acceptable for RP2040 (264KB SRAM).

## What This Replaces vs Keeps

### Replaces

| Component | Replacement |
|-----------|-------------|
| `WorkTracker` | Task completion + dependencies |
| `sleep_requested_` flag | Deferred task posting |
| Manual `checkIdle()` | Completion callbacks |
| Scattered update calls | Single `queue_.process()` |

### Keeps (Unchanged)

| Component | Reason |
|-----------|--------|
| `ReliableMessenger` | Has ACK/retry logic specific to LoRa protocol |
| `PeriodicTaskManager` | Could migrate later, but works fine |
| `HubRouter` | Node-specific forwarding logic |

### Optional Future Migration

The `ReliableMessenger` and `PeriodicTaskManager` could eventually post tasks to the queue instead of having their own loops, but this is not required for the initial implementation.

## Implementation Plan

### Phase 1: Core TaskQueue

1. Create `src/util/task_queue.h` and `.cpp`
2. Implement basic post/process functionality
3. Add priority scheduling
4. Add delayed execution support
5. Unit tests

### Phase 2: Dependency Support

1. Add `postAfter()` for task dependencies
2. Implement completion tracking
3. Add completion callbacks
4. Integration tests

### Phase 3: SensorMode Integration

1. Migrate SensorMode's backlog flow to use TaskQueue
2. Remove `sleep_requested_` flag
3. Remove WorkTracker usage
4. Verify power management still works

### Phase 4: IrrigationMode Integration

1. Migrate registration/update flow
2. Test PMU wake handling with task queue
3. Remove WorkTracker from IrrigationMode

### Phase 5: Cleanup

1. Remove WorkTracker if no longer used
2. Consolidate update calls in ApplicationMode
3. Update documentation

## Alternatives Considered

### 1. FreeRTOS Task Queues

**Pros:** Battle-tested, full-featured
**Cons:** Heavyweight, adds ~10KB flash, overkill for our needs

### 2. Coroutines (C++20)

**Pros:** Elegant async/await syntax
**Cons:** Not well supported on ARM Cortex-M0+, compiler complexity

### 3. Enhance WorkTracker

**Pros:** Minimal change
**Cons:** Still no dependencies, still manual checkIdle(), fixed types

### 4. Event Loop Library (libevent-style)

**Pros:** Standard pattern
**Cons:** File descriptor focus doesn't fit embedded well

## Success Criteria

1. Single `queue_.process()` call replaces 3+ update calls
2. No more deferred flag patterns (`sleep_requested_`, etc.)
3. Task dependencies work correctly (RTC → backlog flow)
4. Memory usage < 500 bytes
5. No dynamic allocation after initialization
6. Existing functionality preserved (power management, messaging)

## Open Questions

1. **Should PeriodicTaskManager migrate?**
   - Could wrap periodic tasks as recurring queue tasks
   - Adds complexity, may not be worth it

2. **Maximum task count?**
   - 16 seems reasonable, could make configurable via template

3. **Should ReliableMessenger use the queue?**
   - Would unify retry logic
   - But messenger has protocol-specific ACK handling

## File Structure

```
bramble/
├── src/
│   ├── util/
│   │   ├── task_queue.h           # NEW
│   │   ├── task_queue.cpp         # NEW
│   │   └── work_tracker.h/.cpp    # DEPRECATED after migration
│   ├── modes/
│   │   ├── application_mode.cpp   # MODIFIED - use TaskQueue
│   │   ├── sensor_mode.cpp        # MODIFIED - use TaskQueue
│   │   └── irrigation_mode.cpp    # MODIFIED - use TaskQueue
```
