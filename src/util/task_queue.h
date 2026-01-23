#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Priority levels for task execution
 *
 * Higher priority tasks execute before lower priority tasks
 * within each process() call.
 */
enum class TaskPriority : uint8_t {
    High = 0,    // Critical: ACKs, time-sensitive responses
    Normal = 1,  // Standard: sensor reads, transmissions
    Low = 2,     // Background: statistics, housekeeping
};

/**
 * @brief Internal task state
 */
enum class TaskState : uint8_t {
    Empty = 0,  // Slot available
    Pending,    // Waiting to run (delay or dependency not met)
    Running,    // Currently executing
};

/**
 * @brief Lightweight task queue for deferred and dependent execution
 *
 * Provides a simple, statically-allocated task queue suitable for embedded
 * systems. Tasks can be posted for immediate execution, delayed execution,
 * or execution after another task completes.
 *
 * Key features:
 * - Static allocation (no heap after construction)
 * - Priority-based scheduling
 * - Delayed execution support
 * - Task dependencies
 * - Completion callbacks
 *
 * Usage:
 *   TaskQueue queue;
 *
 *   // Simple task
 *   queue.post([](void* ctx, uint32_t time) { doWork(); return true; });
 *
 *   // Delayed task (run after 100ms)
 *   queue.postDelayed(myFunc, context, 100);
 *
 *   // Dependent task (run after another completes)
 *   uint16_t first = queue.post(firstTask);
 *   queue.postAfter(secondTask, nullptr, first);
 *
 *   // In main loop
 *   queue.process(current_time_ms);
 */
class TaskQueue {
public:
    /**
     * @brief Task function signature
     * @param context User-provided context pointer
     * @param current_time Current system time in milliseconds
     * @return true if task is complete, false to re-queue for retry
     */
    using TaskFunction = bool (*)(void *context, uint32_t current_time);

    /**
     * @brief Completion callback signature
     * @param task_id ID of the completed task
     * @param context User-provided context pointer
     */
    using CompletionCallback = void (*)(uint16_t task_id, void *context);

    static constexpr size_t MAX_TASKS = 16;
    static constexpr size_t MAX_COMPLETIONS = 8;
    static constexpr uint16_t INVALID_ID = 0;

    TaskQueue();

    // === Task Submission ===

    /**
     * @brief Post a task for immediate execution
     * @param func Task function to execute
     * @param context Optional context pointer passed to func
     * @param priority Task priority (default: Normal)
     * @return Task ID (non-zero), or INVALID_ID if queue is full
     */
    uint16_t post(TaskFunction func, void *context = nullptr,
                  TaskPriority priority = TaskPriority::Normal);

    /**
     * @brief Post a task only if no task with the same function is already queued
     * @param func Task function to execute
     * @param context Optional context pointer passed to func
     * @param priority Task priority (default: Normal)
     * @return Task ID if posted, existing task ID if already queued, or INVALID_ID if queue full
     *
     * Use this for idempotent operations where multiple requests should coalesce
     * into a single execution (e.g., sleep signals, flush requests).
     */
    uint16_t postOnce(TaskFunction func, void *context = nullptr,
                      TaskPriority priority = TaskPriority::Normal);

    /**
     * @brief Post a task to run after a delay
     * @param func Task function to execute
     * @param context Context pointer passed to func
     * @param current_time Current system time in milliseconds
     * @param delay_ms Minimum delay before execution in milliseconds
     * @param priority Task priority
     * @return Task ID, or INVALID_ID if queue is full
     */
    uint16_t postDelayed(TaskFunction func, void *context, uint32_t current_time, uint32_t delay_ms,
                         TaskPriority priority = TaskPriority::Normal);

    /**
     * @brief Post a task that runs after another task completes
     * @param func Task function to execute
     * @param context Context pointer passed to func
     * @param depends_on ID of task that must complete first
     * @param priority Task priority
     * @return Task ID, or INVALID_ID if queue is full
     *
     * If depends_on is INVALID_ID or refers to a non-existent task,
     * the task will run immediately (no dependency).
     */
    uint16_t postAfter(TaskFunction func, void *context, uint16_t depends_on,
                       TaskPriority priority = TaskPriority::Normal);

    // === Task Management ===

    /**
     * @brief Cancel a pending task
     * @param task_id ID of task to cancel
     * @return true if task was cancelled, false if not found or already running
     */
    bool cancel(uint16_t task_id);

    /**
     * @brief Check if a task is still pending or running
     * @param task_id ID of task to check
     * @return true if task exists and hasn't completed
     */
    bool isActive(uint16_t task_id) const;

    /**
     * @brief Set callback for when a specific task completes
     * @param task_id ID of task to watch
     * @param callback Function to call on completion
     * @param context Context pointer passed to callback
     * @return true if callback was registered, false if no slots available
     *
     * The callback fires once when the task completes successfully
     * (returns true from its function). It does not fire if the task
     * is cancelled.
     */
    bool onComplete(uint16_t task_id, CompletionCallback callback, void *context);

    // === Processing ===

    /**
     * @brief Process ready tasks
     * @param current_time Current system time in milliseconds
     * @return Number of tasks executed this call
     *
     * Call this from the main loop. Executes all ready tasks in
     * priority order (High before Normal before Low).
     */
    size_t process(uint32_t current_time);

    // === Status ===

    /**
     * @brief Get number of pending/running tasks
     */
    size_t pendingCount() const;

    /**
     * @brief Get number of available task slots
     */
    size_t availableSlots() const;

    /**
     * @brief Check if queue has no tasks
     */
    bool isEmpty() const;

    /**
     * @brief Check if queue is full
     */
    bool isFull() const;

private:
    /**
     * @brief Internal task representation
     */
    struct Task {
        TaskFunction function;  // Function to execute
        void *context;          // User context
        uint32_t run_after;     // Earliest run time (0 = immediate)
        uint16_t id;            // Unique task ID
        uint16_t depends_on;    // Task ID that must complete first (0 = none)
        TaskPriority priority;  // Execution priority
        TaskState state;        // Current state
    };

    /**
     * @brief Completion callback entry
     */
    struct CompletionEntry {
        uint16_t task_id;
        CompletionCallback callback;
        void *context;
    };

    Task tasks_[MAX_TASKS];
    CompletionEntry completions_[MAX_COMPLETIONS];
    uint16_t next_id_;

    /**
     * @brief Find an empty task slot
     * @return Pointer to empty slot, or nullptr if full
     */
    Task *findEmptySlot();

    /**
     * @brief Find a task by ID
     * @return Pointer to task, or nullptr if not found
     */
    Task *findById(uint16_t id);
    const Task *findById(uint16_t id) const;

    /**
     * @brief Find a pending/running task by function pointer
     * @return Pointer to task, or nullptr if not found
     */
    const Task *findByFunction(TaskFunction func) const;

    /**
     * @brief Mark a task as complete and fire callbacks
     * @param task_id ID of completed task
     */
    void markComplete(uint16_t task_id);

    /**
     * @brief Check if a task's dependencies are satisfied
     * @param task Task to check
     * @return true if task can run
     */
    bool areDependenciesMet(const Task &task) const;

    /**
     * @brief Check if a task is ready to execute
     * @param task Task to check
     * @param current_time Current system time
     * @return true if task can run now
     */
    bool isReady(const Task &task, uint32_t current_time) const;

    /**
     * @brief Get the next unique task ID
     * @return New task ID (never returns 0)
     */
    uint16_t getNextId();

    /**
     * @brief Internal task posting with all parameters
     * @param func Task function
     * @param context User context
     * @param run_after Absolute time to run (0 = immediate)
     * @param depends_on Task ID dependency (INVALID_ID = none)
     * @param priority Task priority
     * @return Task ID, or INVALID_ID if queue is full
     */
    uint16_t postInternal(TaskFunction func, void *context, uint32_t run_after, uint16_t depends_on,
                          TaskPriority priority);
};
