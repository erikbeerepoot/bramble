#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "pico/sync.h"

/**
 * @brief Manages periodic tasks with different intervals
 *
 * Tasks are dispatched from the main loop on Core 0 via update().
 */
class PeriodicTaskManager {
public:
    using TaskFunction = std::function<void(uint32_t)>;

    struct Task {
        TaskFunction function;
        uint32_t interval_ms;
        uint32_t last_run;
        const char *name;  // For debugging

        Task(TaskFunction func, uint32_t interval, const char *task_name = "")
            : function(func), interval_ms(interval), last_run(0), name(task_name)
        {
        }
    };

private:
    std::vector<Task> tasks_;
    mutex_t task_mutex_;

public:
    PeriodicTaskManager() { mutex_init(&task_mutex_); }

    /**
     * @brief Add a periodic task
     * @param task Task function to execute
     * @param interval_ms Interval between executions in milliseconds
     * @param name Optional name for debugging
     */
    void addTask(TaskFunction task, uint32_t interval_ms, const char *name = "")
    {
        mutex_enter_blocking(&task_mutex_);
        tasks_.emplace_back(task, interval_ms, name);
        mutex_exit(&task_mutex_);
    }

    /**
     * @brief Update all tasks — call from the main loop each iteration.
     * @param current_time Current system time in milliseconds
     */
    void update(uint32_t current_time)
    {
        mutex_enter_blocking(&task_mutex_);
        for (auto &task : tasks_) {
            if (current_time - task.last_run >= task.interval_ms) {
                task.function(current_time);
                task.last_run = current_time;
            }
        }
        mutex_exit(&task_mutex_);
    }

    /**
     * @brief Clear all tasks
     */
    void clear()
    {
        mutex_enter_blocking(&task_mutex_);
        tasks_.clear();
        mutex_exit(&task_mutex_);
    }

    /**
     * @brief Get number of registered tasks
     * @return Task count
     */
    size_t getTaskCount() const { return tasks_.size(); }
};
