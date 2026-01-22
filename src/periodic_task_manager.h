#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "pico/multicore.h"
#include "pico/sync.h"

/**
 * @brief Manages periodic tasks with different intervals
 *
 * Can run on either core - if multicore is enabled, tasks run on Core 1
 * This keeps the main communication loop on Core 0 responsive
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
    bool use_multicore_;
    volatile bool running_;
    mutex_t task_mutex_;

    /**
     * @brief Core 1 entry point for multicore mode
     */
    static void core1_entry()
    {
        PeriodicTaskManager *manager = (PeriodicTaskManager *)multicore_fifo_pop_blocking();
        manager->core1_loop();
    }

    /**
     * @brief Core 1 main loop
     */
    void core1_loop()
    {
        while (running_) {
            uint32_t current_time = to_ms_since_boot(get_absolute_time());

            mutex_enter_blocking(&task_mutex_);
            for (auto &task : tasks_) {
                if (current_time - task.last_run >= task.interval_ms) {
                    task.function(current_time);
                    task.last_run = current_time;
                }
            }
            mutex_exit(&task_mutex_);

            // Sleep briefly to avoid hogging CPU
            sleep_ms(10);
        }
    }

public:
    /**
     * @brief Constructor
     * @param use_multicore If true, run tasks on Core 1
     */
    explicit PeriodicTaskManager(bool use_multicore = false)
        : use_multicore_(use_multicore), running_(false)
    {
        mutex_init(&task_mutex_);
    }

    ~PeriodicTaskManager() { stop(); }

    /**
     * @brief Start the task manager (launches Core 1 if multicore enabled)
     */
    void start()
    {
        if (use_multicore_ && !running_) {
            running_ = true;
            multicore_launch_core1(core1_entry);
            multicore_fifo_push_blocking((uint32_t)this);
        }
    }

    /**
     * @brief Stop the task manager (stops Core 1 if multicore enabled)
     */
    void stop()
    {
        if (use_multicore_ && running_) {
            running_ = false;
            multicore_reset_core1();
        }
    }

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
     * @brief Update all tasks (only used in single-core mode)
     * @param current_time Current system time in milliseconds
     */
    void update(uint32_t current_time)
    {
        if (!use_multicore_) {
            mutex_enter_blocking(&task_mutex_);
            for (auto &task : tasks_) {
                if (current_time - task.last_run >= task.interval_ms) {
                    task.function(current_time);
                    task.last_run = current_time;
                }
            }
            mutex_exit(&task_mutex_);
        }
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