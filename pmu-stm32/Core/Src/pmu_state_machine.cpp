#include "pmu_state_machine.h"

PmuStateMachine::PmuStateMachine(GetTickCallback getTick) : getTick_(getTick) {}

// =============================================================================
// Pure Reducer Function
// =============================================================================

PmuState PmuStateMachine::reduce(PmuState state, PmuEvent event)
{
    switch (state) {
        case PmuState::BOOTING:
            switch (event) {
                case PmuEvent::BOOT_COMPLETE:
                    // Go to POST_BOOT grace period to allow RP2040 time to initialize
                    return PmuState::POST_BOOT;
                case PmuEvent::CTS_RECEIVED:
                    // CTS received during boot - go directly to WAKE_ACTIVE
                    // This ensures WakeNotification is sent immediately without waiting
                    // for boot animation to complete
                    return PmuState::WAKE_ACTIVE;
                case PmuEvent::ERROR_OCCURRED:
                    return PmuState::ERROR;
                default:
                    return state;
            }

        case PmuState::POST_BOOT:
            switch (event) {
                case PmuEvent::CTS_RECEIVED:
                    // RP2040 is ready - transition to WAKE_ACTIVE
                    return PmuState::WAKE_ACTIVE;
                case PmuEvent::WAKE_TIMEOUT:
                    // Grace period expired without CTS - go to sleep
                    return PmuState::SLEEPING;
                case PmuEvent::ERROR_OCCURRED:
                    return PmuState::ERROR;
                default:
                    return state;
            }

        case PmuState::SLEEPING:
            switch (event) {
                case PmuEvent::RTC_WAKEUP:
                    return PmuState::AWAITING_CTS;
                case PmuEvent::CTS_RECEIVED:
                    // CTS can wake us from sleep (UART interrupt) - go directly to WAKE_ACTIVE
                    return PmuState::WAKE_ACTIVE;
                case PmuEvent::ERROR_OCCURRED:
                    return PmuState::ERROR;
                default:
                    return state;
            }

        case PmuState::AWAITING_CTS:
            switch (event) {
                case PmuEvent::CTS_RECEIVED:
                case PmuEvent::CTS_TIMEOUT:
                    return PmuState::WAKE_ACTIVE;
                case PmuEvent::WAKE_TIMEOUT:
                    return PmuState::SLEEPING;
                case PmuEvent::ERROR_OCCURRED:
                    return PmuState::ERROR;
                default:
                    return state;
            }

        case PmuState::WAKE_ACTIVE:
            switch (event) {
                case PmuEvent::READY_FOR_SLEEP:
                case PmuEvent::WAKE_TIMEOUT:
                    return PmuState::SLEEPING;
                case PmuEvent::ERROR_OCCURRED:
                    return PmuState::ERROR;
                default:
                    return state;
            }

        case PmuState::ERROR:
            // Terminal state - no transitions out
            return state;

        default:
            return state;
    }
}

// =============================================================================
// Event Dispatch Methods
// =============================================================================

void PmuStateMachine::dispatch(PmuEvent event)
{
    PmuState newState = reduce(state_, event);

    if (newState != state_) {
        onEnterState(newState, event);
        transitionTo(newState);
    }
}

void PmuStateMachine::setWakeType(WakeType type, const PMU::ScheduleEntry *entry)
{
    context_.type = type;

    if (type == WakeType::SCHEDULED && entry != nullptr) {
        context_.hasScheduleEntry = true;
        context_.scheduleEntry = *entry;
        // Timeout = duration (seconds) * 1000 + grace period
        context_.timeoutMs =
            static_cast<uint32_t>(context_.scheduleEntry.duration) * 1000 + SCHEDULED_GRACE_PERIOD_MS;
    } else {
        context_.hasScheduleEntry = false;
        context_.timeoutMs = PERIODIC_WAKE_TIMEOUT_MS;
    }
}

void PmuStateMachine::extendWakeTimeout(uint16_t seconds)
{
    // Only effective when awake (POST_BOOT, AWAITING_CTS, or WAKE_ACTIVE)
    if (state_ != PmuState::POST_BOOT && state_ != PmuState::AWAITING_CTS &&
        state_ != PmuState::WAKE_ACTIVE) {
        return;
    }

    // Reset the start time to now
    context_.startTime = getTick_();

    // Set new timeout (0 = use default, otherwise use specified seconds)
    if (seconds > 0) {
        context_.timeoutMs = static_cast<uint32_t>(seconds) * 1000;
    } else {
        context_.timeoutMs = PERIODIC_WAKE_TIMEOUT_MS;
    }
}

void PmuStateMachine::tick()
{
    uint32_t now = getTick_();

    switch (state_) {
        case PmuState::POST_BOOT: {
            // Check boot grace period timeout
            uint32_t elapsed = now - context_.startTime;
            if (elapsed >= BOOT_GRACE_PERIOD_MS) {
                dispatch(PmuEvent::WAKE_TIMEOUT);
            }
            break;
        }

        case PmuState::AWAITING_CTS: {
            // Check CTS timeout
            uint32_t ctsElapsed = now - context_.ctsWaitStartTime;
            if (ctsElapsed >= CTS_TIMEOUT_MS) {
                dispatch(PmuEvent::CTS_TIMEOUT);
                return;
            }

            // Also check overall wake timeout as safety
            uint32_t wakeElapsed = now - context_.startTime;
            if (wakeElapsed >= context_.timeoutMs) {
                dispatch(PmuEvent::WAKE_TIMEOUT);
            }
            break;
        }

        case PmuState::WAKE_ACTIVE: {
            // Check wake timeout
            uint32_t elapsed = now - context_.startTime;
            if (elapsed >= context_.timeoutMs) {
                dispatch(PmuEvent::WAKE_TIMEOUT);
            }
            break;
        }

        default:
            // No timeout handling in other states
            break;
    }
}

// =============================================================================
// State Entry Actions
// =============================================================================

void PmuStateMachine::onEnterState(PmuState newState, PmuEvent event)
{
    uint32_t now = getTick_();

    switch (newState) {
        case PmuState::SLEEPING:
            // Reset context when entering sleep
            context_.reset();
            break;

        case PmuState::POST_BOOT:
            // Set up boot grace period timing
            context_.startTime = now;
            context_.type = WakeType::PERIODIC;
            context_.timeoutMs = BOOT_GRACE_PERIOD_MS;
            break;

        case PmuState::AWAITING_CTS:
            // Set up timing for wake context
            // Wake type and timeout are set by callback via setWakeType()
            context_.startTime = now;
            context_.ctsWaitStartTime = now;
            break;

        case PmuState::WAKE_ACTIVE:
            // Coming from BOOTING with early CTS - set up context
            if (event == PmuEvent::CTS_RECEIVED && context_.startTime == 0) {
                context_.startTime = now;
                context_.type = WakeType::PERIODIC;
                context_.timeoutMs = PERIODIC_WAKE_TIMEOUT_MS;
            }
            // For transitions from POST_BOOT or AWAITING_CTS, context is already set
            break;

        case PmuState::BOOTING:
        case PmuState::ERROR:
            // No special entry actions
            break;
    }
}

// =============================================================================
// Query Methods
// =============================================================================

bool PmuStateMachine::shouldPowerRp2040() const
{
    switch (state_) {
        case PmuState::BOOTING:
        case PmuState::POST_BOOT:
        case PmuState::AWAITING_CTS:
        case PmuState::WAKE_ACTIVE:
            return true;

        case PmuState::SLEEPING:
        case PmuState::ERROR:
        default:
            return false;
    }
}

const PMU::ScheduleEntry *PmuStateMachine::getScheduleEntry() const
{
    if (context_.type == WakeType::SCHEDULED && context_.hasScheduleEntry) {
        return &context_.scheduleEntry;
    }
    return nullptr;
}

const char *PmuStateMachine::stateName(PmuState state)
{
    switch (state) {
        case PmuState::BOOTING:
            return "BOOTING";
        case PmuState::POST_BOOT:
            return "POST_BOOT";
        case PmuState::SLEEPING:
            return "SLEEPING";
        case PmuState::AWAITING_CTS:
            return "AWAITING_CTS";
        case PmuState::WAKE_ACTIVE:
            return "WAKE_ACTIVE";
        case PmuState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

const char *PmuStateMachine::eventName(PmuEvent event)
{
    switch (event) {
        case PmuEvent::BOOT_COMPLETE:
            return "BOOT_COMPLETE";
        case PmuEvent::RTC_WAKEUP:
            return "RTC_WAKEUP";
        case PmuEvent::CTS_RECEIVED:
            return "CTS_RECEIVED";
        case PmuEvent::CTS_TIMEOUT:
            return "CTS_TIMEOUT";
        case PmuEvent::READY_FOR_SLEEP:
            return "READY_FOR_SLEEP";
        case PmuEvent::WAKE_TIMEOUT:
            return "WAKE_TIMEOUT";
        case PmuEvent::ERROR_OCCURRED:
            return "ERROR_OCCURRED";
        default:
            return "UNKNOWN";
    }
}

// =============================================================================
// State Transition
// =============================================================================

void PmuStateMachine::transitionTo(PmuState newState)
{
    if (state_ == newState) {
        return;  // No change
    }

    state_ = newState;

    if (stateCallback_) {
        stateCallback_(state_);
    }
}
