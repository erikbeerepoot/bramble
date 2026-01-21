#pragma once

#include <cstdint>
#include <functional>

/**
 * @brief Types of work that can be tracked
 *
 * Each mode uses a subset of these work types. The tracker uses a bitmask
 * so only one instance of each work type can be active at a time.
 */
enum class WorkType : uint8_t {
    // Common
    RtcSync = 0,           // Waiting for RTC synchronization

    // SensorMode
    BacklogTransmit = 1,   // Transmitting sensor data backlog

    // IrrigationMode
    UpdatePull = 2,        // Pulling updates from hub
    Registration = 3,      // Registering with hub
};

/**
 * @brief Tracks pending work and signals when idle
 *
 * Provides a centralized way to track what work is pending. The idle callback
 * is only invoked when explicitly checked via checkIdle(), giving the caller
 * full control over when idle is evaluated.
 *
 * Usage:
 *   WorkTracker tracker;
 *   tracker.setIdleCallback([]() { signalReadyForSleep(); });
 *
 *   tracker.addWork(WorkType::RtcSync);
 *   // ... RTC syncs ...
 *   tracker.completeWork(WorkType::RtcSync);
 *
 *   // In main loop - check if idle and fire callback
 *   tracker.checkIdle();
 */
class WorkTracker {
public:
    using IdleCallback = std::function<void()>;

    /**
     * @brief Set callback to invoke when all work completes
     * @param callback Function to call when transitioning to idle
     */
    void setIdleCallback(IdleCallback callback);

    /**
     * @brief Add pending work of the given type
     * @param type The type of work to add
     *
     * If work of this type is already pending, this is a no-op.
     */
    void addWork(WorkType type);

    /**
     * @brief Complete work of the given type
     * @param type The type of work that completed
     *
     * Note: This does NOT automatically fire the idle callback.
     * Call checkIdle() explicitly when ready to evaluate idle state.
     */
    void completeWork(WorkType type);

    /**
     * @brief Check if any work is pending
     * @return true if there is pending work
     */
    bool hasWork() const;

    /**
     * @brief Check if specific work type is pending
     * @param type The work type to check
     * @return true if that work type is pending
     */
    bool hasWork(WorkType type) const;

    /**
     * @brief Check if no work is pending
     * @return true if idle (no pending work)
     */
    bool isIdle() const { return !hasWork(); }

    /**
     * @brief Log current work state for debugging
     */
    void logState() const;

    /**
     * @brief Check if idle and invoke callback if so
     *
     * Call this from the main loop after all event processing is done.
     * Only fires the callback if isIdle() returns true.
     */
    void checkIdle();

private:
    // Bitmask for active work (up to 8 work types)
    uint8_t active_work_ = 0;
    IdleCallback on_idle_;

    /**
     * @brief Get human-readable name for work type
     */
    static const char* workTypeName(WorkType type);
};
