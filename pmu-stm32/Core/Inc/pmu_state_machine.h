#ifndef PMU_STATE_MACHINE_H
#define PMU_STATE_MACHINE_H

#include <cstdint>
#include <functional>

#include "pmu_protocol.h"

/**
 * @brief Events that can be dispatched to the PMU state machine
 */
enum class PmuEvent : uint8_t {
    BOOT_COMPLETE,    ///< Boot animation finished
    RTC_WAKEUP,       ///< RTC woke us (any type - scheduled or periodic)
    CTS_RECEIVED,     ///< RP2040 sent ClearToSend
    CTS_TIMEOUT,      ///< Waited too long for CTS
    READY_FOR_SLEEP,  ///< RP2040 signaled work complete
    WAKE_TIMEOUT,     ///< Overall wake timeout expired
    ERROR_OCCURRED    ///< Unrecoverable error
};

/**
 * @brief PMU operating states
 *
 * State transitions (reducer pattern):
 *
 * [Power On]
 *     |
 *     v
 *  BOOTING ---> BOOT_COMPLETE ---> SLEEPING
 *     |                               |
 *     v (error)                       |
 *   ERROR <--------------------+      |
 *                              |      |
 *                    RTC_WAKEUP       |
 *                              |      |
 *                              v      v
 *  SLEEPING <-------------- AWAITING_CTS
 *     ^                        |
 *     |            CTS_RECEIVED/CTS_TIMEOUT
 *     |                        |
 *     |                        v
 *     +---------------- WAKE_ACTIVE
 *         READY_FOR_SLEEP
 *         or WAKE_TIMEOUT
 */
enum class PmuState : uint8_t {
    BOOTING,       ///< Boot animation in progress
    SLEEPING,      ///< Low power STOP mode (or about to enter)
    AWAITING_CTS,  ///< DC/DC on, waiting for RP2040 CTS signal
    WAKE_ACTIVE,   ///< Wake notification sent, RP2040 working
    ERROR          ///< Unrecoverable error
};

/**
 * @brief Type of wake event (context, not encoded in state)
 */
enum class WakeType : uint8_t {
    NONE,       ///< Not in a wake cycle
    SCHEDULED,  ///< Woke due to schedule entry
    PERIODIC    ///< Woke on periodic interval
};

/**
 * @brief Context for wake sessions
 *
 * Holds information about the current wake session: when it started,
 * what type of wake it is, and timeout information.
 */
struct WakeContext {
    WakeType type = WakeType::NONE;  ///< Type of wake event
    uint32_t startTime = 0;          ///< HAL tick when wake started
    uint32_t timeoutMs = 0;          ///< Maximum wake duration in ms
    uint32_t ctsWaitStartTime = 0;   ///< HAL tick when CTS wait started

    // Optional schedule entry (only valid when type == SCHEDULED)
    bool hasScheduleEntry = false;
    PMU::ScheduleEntry scheduleEntry;

    void reset()
    {
        type = WakeType::NONE;
        startTime = 0;
        timeoutMs = 0;
        ctsWaitStartTime = 0;
        hasScheduleEntry = false;
        scheduleEntry = PMU::ScheduleEntry();
    }
};

/**
 * @brief Event-driven state machine for PMU operation using reducer pattern
 *
 * Manages PMU state transitions through a central reducer function.
 * Events are dispatched to the reducer which returns the next state.
 *
 * Usage:
 *   PmuStateMachine pmu(HAL_GetTick);
 *   pmu.setStateCallback([](PmuState state) {
 *       updateLedForState(state);
 *   });
 *
 *   // After boot animation:
 *   pmu.dispatch(PmuEvent::BOOT_COMPLETE);
 *
 *   // On RTC wakeup (callback determines wake type):
 *   pmu.dispatch(PmuEvent::RTC_WAKEUP);
 *
 *   // When CTS received:
 *   pmu.dispatch(PmuEvent::CTS_RECEIVED);
 *
 *   // In main loop (checks timeouts):
 *   pmu.tick();
 */
class PmuStateMachine {
public:
    using GetTickCallback = uint32_t (*)();
    using StateCallback = std::function<void(PmuState)>;

    // CTS timeout - fallback for old firmware that doesn't send CTS
    static constexpr uint32_t CTS_TIMEOUT_MS = 2000;  // 2 seconds
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
    // Event Dispatch Methods
    // =========================================================================

    /**
     * @brief Dispatch an event to the state machine
     *
     * The reducer processes the event and transitions to the appropriate state.
     *
     * @param event The event to dispatch
     */
    void dispatch(PmuEvent event);

    /**
     * @brief Check timeouts and dispatch timeout events
     *
     * Call this periodically in the main loop. Generates CTS_TIMEOUT
     * or WAKE_TIMEOUT events when appropriate.
     */
    void tick();

    /**
     * @brief Set the wake type and optional schedule entry
     *
     * Called by the state change callback when entering AWAITING_CTS
     * to configure the wake context based on RTC/schedule analysis.
     *
     * @param type The type of wake (SCHEDULED or PERIODIC)
     * @param entry Optional schedule entry (only for SCHEDULED type)
     */
    void setWakeType(WakeType type, const PMU::ScheduleEntry *entry = nullptr);

    // =========================================================================
    // Query Methods
    // =========================================================================

    /**
     * @brief Get current state
     */
    PmuState state() const { return state_; }

    /**
     * @brief Get current wake type
     */
    WakeType wakeType() const { return context_.type; }

    /**
     * @brief Check if RP2040 power (DC/DC) should be enabled
     *
     * True during BOOTING, AWAITING_CTS, and WAKE_ACTIVE states.
     */
    bool shouldPowerRp2040() const;

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
     * @brief Check if in an active wake state (AWAITING_CTS or WAKE_ACTIVE)
     */
    bool isAwake() const
    {
        return state_ == PmuState::AWAITING_CTS || state_ == PmuState::WAKE_ACTIVE;
    }

    /**
     * @brief Check if in error state
     */
    bool isError() const { return state_ == PmuState::ERROR; }

    /**
     * @brief Get human-readable state name
     */
    static const char *stateName(PmuState state);

    /**
     * @brief Get human-readable event name
     */
    static const char *eventName(PmuEvent event);

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
     * @brief Pure reducer function - computes next state from current state and event
     *
     * This function has no side effects. It only computes the next state.
     *
     * @param state Current state
     * @param event Event to process
     * @return Next state
     */
    static PmuState reduce(PmuState state, PmuEvent event);

    /**
     * @brief Transition to a new state
     *
     * Updates state and invokes callback if state changed.
     */
    void transitionTo(PmuState newState);

    /**
     * @brief Handle entry actions for a state
     *
     * Called when entering a new state. Sets up context as needed.
     */
    void onEnterState(PmuState newState, PmuEvent event);

    PmuState state_ = PmuState::BOOTING;
    WakeContext context_;
    GetTickCallback getTick_;
    StateCallback stateCallback_;
};

#endif  // PMU_STATE_MACHINE_H
