#include "task_queue.h"
#include "../hal/logger.h"

static Logger logger("TASKQ");

TaskQueue::TaskQueue() : next_id_(1) {
    // Initialize all task slots as empty
    for (size_t i = 0; i < MAX_TASKS; i++) {
        tasks_[i].state = TaskState::Empty;
        tasks_[i].id = INVALID_ID;
    }

    // Initialize completion callback slots
    for (size_t i = 0; i < MAX_COMPLETIONS; i++) {
        completions_[i].task_id = INVALID_ID;
        completions_[i].callback = nullptr;
    }
}

uint16_t TaskQueue::post(TaskFunction func, void* context, TaskPriority priority) {
    return postInternal(func, context, 0, INVALID_ID, priority);
}

uint16_t TaskQueue::postDelayed(TaskFunction func, void* context,
                                 uint32_t current_time, uint32_t delay_ms,
                                 TaskPriority priority) {
    return postInternal(func, context, current_time + delay_ms, INVALID_ID, priority);
}

uint16_t TaskQueue::postAfter(TaskFunction func, void* context,
                               uint16_t depends_on, TaskPriority priority) {
    return postInternal(func, context, 0, depends_on, priority);
}

uint16_t TaskQueue::postInternal(TaskFunction func, void* context,
                                  uint32_t run_after, uint16_t depends_on,
                                  TaskPriority priority) {
    if (!func) {
        logger.warn("Attempted to post null task function");
        return INVALID_ID;
    }

    Task* slot = findEmptySlot();
    if (!slot) {
        logger.warn("Task queue full, cannot post task");
        return INVALID_ID;
    }

    uint16_t id = getNextId();

    slot->function = func;
    slot->context = context;
    slot->run_after = run_after;
    slot->id = id;
    slot->depends_on = depends_on;
    slot->priority = priority;
    slot->state = TaskState::Pending;

    if (depends_on != INVALID_ID) {
        logger.debug("Posted task %u depending on %u", id, depends_on);
    } else if (run_after > 0) {
        logger.debug("Posted task %u (priority=%u, run_after=%ums)",
                     id, static_cast<uint8_t>(priority), run_after);
    } else {
        logger.debug("Posted task %u (priority=%u)", id, static_cast<uint8_t>(priority));
    }

    return id;
}

bool TaskQueue::cancel(uint16_t task_id) {
    if (task_id == INVALID_ID) {
        return false;
    }

    Task* task = findById(task_id);
    if (!task) {
        return false;
    }

    if (task->state == TaskState::Running) {
        logger.warn("Cannot cancel running task %u", task_id);
        return false;
    }

    logger.debug("Cancelled task %u", task_id);
    task->state = TaskState::Empty;
    task->id = INVALID_ID;

    // Remove any completion callbacks for this task
    for (size_t i = 0; i < MAX_COMPLETIONS; i++) {
        if (completions_[i].task_id == task_id) {
            completions_[i].task_id = INVALID_ID;
            completions_[i].callback = nullptr;
        }
    }

    return true;
}

bool TaskQueue::isActive(uint16_t task_id) const {
    if (task_id == INVALID_ID) {
        return false;
    }

    const Task* task = findById(task_id);
    return task != nullptr && task->state != TaskState::Empty;
}

bool TaskQueue::onComplete(uint16_t task_id, CompletionCallback callback, void* context) {
    if (task_id == INVALID_ID || !callback) {
        return false;
    }

    // Verify task exists
    if (!isActive(task_id)) {
        logger.warn("Cannot set completion callback for inactive task %u", task_id);
        return false;
    }

    // Find empty completion slot
    for (size_t i = 0; i < MAX_COMPLETIONS; i++) {
        if (completions_[i].task_id == INVALID_ID) {
            completions_[i].task_id = task_id;
            completions_[i].callback = callback;
            completions_[i].context = context;
            return true;
        }
    }

    logger.warn("No completion callback slots available");
    return false;
}

size_t TaskQueue::process(uint32_t current_time) {
    size_t executed = 0;

    // Process by priority: High -> Normal -> Low
    for (int p = 0; p <= static_cast<int>(TaskPriority::Low); p++) {
        TaskPriority priority = static_cast<TaskPriority>(p);

        for (size_t i = 0; i < MAX_TASKS; i++) {
            Task& task = tasks_[i];

            if (task.state != TaskState::Pending) {
                continue;
            }

            if (task.priority != priority) {
                continue;
            }

            if (!isReady(task, current_time)) {
                continue;
            }

            // Execute the task
            task.state = TaskState::Running;
            logger.debug("Executing task %u", task.id);

            bool complete = task.function(task.context, current_time);
            executed++;

            if (complete) {
                uint16_t completed_id = task.id;
                task.state = TaskState::Empty;
                task.id = INVALID_ID;
                markComplete(completed_id);
                logger.debug("Task %u completed", completed_id);
            } else {
                // Re-queue for retry
                task.state = TaskState::Pending;
                logger.debug("Task %u re-queued for retry", task.id);
            }
        }
    }

    return executed;
}

size_t TaskQueue::pendingCount() const {
    size_t count = 0;
    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (tasks_[i].state != TaskState::Empty) {
            count++;
        }
    }
    return count;
}

size_t TaskQueue::availableSlots() const {
    return MAX_TASKS - pendingCount();
}

bool TaskQueue::isEmpty() const {
    return pendingCount() == 0;
}

bool TaskQueue::isFull() const {
    return availableSlots() == 0;
}

TaskQueue::Task* TaskQueue::findEmptySlot() {
    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (tasks_[i].state == TaskState::Empty) {
            return &tasks_[i];
        }
    }
    return nullptr;
}

TaskQueue::Task* TaskQueue::findById(uint16_t id) {
    if (id == INVALID_ID) {
        return nullptr;
    }

    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (tasks_[i].id == id && tasks_[i].state != TaskState::Empty) {
            return &tasks_[i];
        }
    }
    return nullptr;
}

const TaskQueue::Task* TaskQueue::findById(uint16_t id) const {
    if (id == INVALID_ID) {
        return nullptr;
    }

    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (tasks_[i].id == id && tasks_[i].state != TaskState::Empty) {
            return &tasks_[i];
        }
    }
    return nullptr;
}

void TaskQueue::markComplete(uint16_t task_id) {
    // Fire any registered completion callbacks
    for (size_t i = 0; i < MAX_COMPLETIONS; i++) {
        if (completions_[i].task_id == task_id && completions_[i].callback) {
            completions_[i].callback(task_id, completions_[i].context);
            completions_[i].task_id = INVALID_ID;
            completions_[i].callback = nullptr;
        }
    }
}

bool TaskQueue::areDependenciesMet(const Task& task) const {
    if (task.depends_on == INVALID_ID) {
        return true;  // No dependency
    }

    // Dependency is met if the task no longer exists (completed or cancelled)
    return findById(task.depends_on) == nullptr;
}

bool TaskQueue::isReady(const Task& task, uint32_t current_time) const {
    // Check time constraint
    if (task.run_after > 0 && current_time < task.run_after) {
        return false;
    }

    // Check dependencies
    return areDependenciesMet(task);
}

uint16_t TaskQueue::getNextId() {
    uint16_t id = next_id_++;

    // Skip zero (INVALID_ID)
    if (next_id_ == INVALID_ID) {
        next_id_ = 1;
    }

    return id;
}
