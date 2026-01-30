#ifndef PMU_STATE_MACHINE_H
#define PMU_STATE_MACHINE_H

#include <cstdint>
#include <functional>

#include "pmu_protocol.h"

/**
 * @brief PMU operating states
 *
 * State transitions:
 *
 * [Power On]
 *     |
 *     v
 *  BOOTING -----> reportBootComplete() -----> SLEEPING
 *     |                                           |
 *     v (error)                                   |
 *   ERROR                      +------------------+
 *                              |                  |
 *                 (RTC wake, schedule entry)   (RTC wake, no schedule)
 *                              |                  |
 *                              v                  v
 *                      SCHEDULED_WAKE      PERIODIC_WAKE
 *                              |                  |
 *                              +--------+---------+
 *                                       |
 *                         reportReadyForSleep() or timeout
 *                                       |
 *                                       v
 *                                   SLEEPING
 */
enum class PmuState : uint8_t {
    BOOTING,         ///< Boot animation in progress
    SLEEPING,        ///< Low power STOP mode (or about to enter)
    SCHEDULED_WAKE,  ///< Woke due to schedule entry, RP2040 decides what to do
    PERIODIC_WAKE,   ///< Periodic interval wake, RP2040 decides what to do
    ERROR            ///< Unrecoverable error
};

/**
 * @brief Context for wake sessions
 *
 * Holds information about the current wake session: when it started,
 * whether we've notified RP2040, and the timeout duration.
 */
struct WakeContext {
    uint32_t startTime = 0;         ///< HAL tick when wake started
    bool notificationSent = false;  ///< True if wake notification sent to RP2040
    uint32_t timeoutMs = 0;         ///< Maximum wake duration in ms

    // Optional schedule entry (only valid for SCHEDULED_WAKE)
    bool hasScheduleEntry = false;
    PMU::ScheduleEntry scheduleEntry;

    void reset()
    {
        startTime = 0;
        notificationSent = false;
        timeoutMs = 0;
        hasScheduleEntry = false;
        scheduleEntry = PMU::ScheduleEntry();
    }
};

/**
 * @brief Event-driven state machine for PMU operation
 *
 * Manages PMU state transitions and eliminates scattered boolean flags.
 * State is managed internally - report events and the state machine
 * updates automatically.
 *
 * Usage:
 *   PmuStateMachine pmu(HAL_GetTick);
 *   pmu.setStateCallback([](PmuState state) {
 *       updateLedForState(state);
 *   });
 *
 *   // After boot animation:
 *   pmu.reportBootComplete();
 *
 *   // On RTC wakeup with schedule:
 *   pmu.reportScheduledWake(entry);
 *
 *   // On RTC wakeup without schedule:
 *   pmu.reportPeriodicWake();
 *
 *   // In main loop:
 *   bool stayAwake = pmu.update();
 *   if (!stayAwake) enterStopMode();
 */
class PmuStateMachine {
public:
    using GetTickCallback = uint32_t (*)();
    using StateCallback = std::function<void(PmuState)>;

    // Grace period added to schedule duration for timeouts
    static constexpr uint32_t SCHEDULED_GRACE_PERIOD_MS = 30000;  // 30 seconds
    // Default timeout for periodic wakes
    static constexpr uint32_t PERIODIC_WAKE_TIMEOUT_MS = 120000;  // 2 minutes

    /**
     * @brief Constructor
     * @param getTick Function to get current tick (e.g., HAL_GetTick)
     */
    explicit PmuStateMachine(GetTickCallback getTick);

    // =========================================================================
    // Event Methods - call these to report state changes
    // =========================================================================

    /**
     * @brief Report that boot animation is complete
     *
     * Transitions BOOTING -> SLEEPING
     */
    void reportBootComplete();

    /**
     * @brief Report a scheduled wake event (RTC woke us for a schedule entry)
     *
     * Transitions SLEEPING -> SCHEDULED_WAKE
     * Timeout = entry.duration + 30s grace period
     *
     * @param entry The schedule entry that triggered this wake
     */
    void reportScheduledWake(const PMU::ScheduleEntry &entry);

    /**
     * @brief Report a periodic wake event (RTC woke us on interval)
     *
     * Transitions SLEEPING -> PERIODIC_WAKE
     * Timeout = 2 minutes
     */
    void reportPeriodicWake();

    /**
     * @brief Report that RP2040 signaled ready for sleep
     *
     * Transitions SCHEDULED_WAKE|PERIODIC_WAKE -> SLEEPING
     */
    void reportReadyForSleep();

    /**
     * @brief Report an unrecoverable error
     *
     * Transitions any state -> ERROR
     */
    void reportError();

    /**
     * @brief Mark that wake notification has been sent to RP2040
     *
     * Call after successfully sending wake notification.
     */
    void markWakeNotificationSent();

    // =========================================================================
    // Update Method - call in main loop
    // =========================================================================

    /**
     * @brief Update state machine and check timeouts
     *
     * Call this periodically in the main loop. Handles timeout
     * transitions when RP2040 doesn't respond in time.
     *
     * @return true if should stay awake, false if can enter STOP mode
     */
    bool update();

    // =========================================================================
    // Query Methods
    // =========================================================================

    /**
     * @brief Get current state
     */
    PmuState state() const { return state_; }

    /**
     * @brief Check if RP2040 power (DC/DC) should be enabled
     *
     * True during BOOTING, SCHEDULED_WAKE, and PERIODIC_WAKE.
     */
    bool shouldPowerRp2040() const;

    /**
     * @brief Check if wake notification should be sent
     *
     * True if in a wake state and notification hasn't been sent yet.
     */
    bool shouldSendWakeNotification() const;

    /**
     * @brief Get the schedule entry for current wake (if any)
     *
     * @return Pointer to schedule entry, or nullptr if not a scheduled wake
     */
    const PMU::ScheduleEntry *getScheduleEntry() const;

    /**
     * @brief Get current wake context
     */
    const WakeContext &wakeContext() const { return context_; }

    /**
     * @brief Check if in an active wake state
     */
    bool isAwake() const
    {
        return state_ == PmuState::SCHEDULED_WAKE || state_ == PmuState::PERIODIC_WAKE;
    }

    /**
     * @brief Check if in error state
     */
    bool isError() const { return state_ == PmuState::ERROR; }

    /**
     * @brief Get human-readable state name
     */
    static const char *stateName(PmuState state);

    // =========================================================================
    // Callback
    // =========================================================================

    /**
     * @brief Set callback for state changes
     *
     * Callback is invoked after each state transition.
     */
    void setStateCallback(StateCallback callback) { stateCallback_ = std::move(callback); }

private:
    /**
     * @brief Transition to a new state
     *
     * Updates state and invokes callback if state changed.
     */
    void transitionTo(PmuState newState);

    PmuState state_ = PmuState::BOOTING;
    WakeContext context_;
    GetTickCallback getTick_;
    StateCallback stateCallback_;
};

#endif  // PMU_STATE_MACHINE_H
