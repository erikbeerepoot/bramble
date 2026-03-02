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
                    // Boot animation done - start sending WakeNotification
                    return PmuState::AWAITING_ACK;
                case PmuEvent::NOTIFICATION_ACK:
                    // RP2040 acked during boot - go directly to WAKE_ACTIVE
                    return PmuState::WAKE_ACTIVE;
                case PmuEvent::ERROR_OCCURRED:
                    return PmuState::ERROR;
                default:
                    return state;
            }

        case PmuState::SLEEPING:
            switch (event) {
                case PmuEvent::RTC_WAKEUP:
                    return PmuState::AWAITING_ACK;
                case PmuEvent::NOTIFICATION_ACK:
                    // UART activity can wake us from sleep - go directly to WAKE_ACTIVE
                    return PmuState::WAKE_ACTIVE;
                case PmuEvent::ERROR_OCCURRED:
                    return PmuState::ERROR;
                default:
                    return state;
            }

        case PmuState::AWAITING_ACK:
            switch (event) {
                case PmuEvent::NOTIFICATION_ACK:
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
    // Only effective when awake (AWAITING_ACK or WAKE_ACTIVE)
    if (state_ != PmuState::AWAITING_ACK && state_ != PmuState::WAKE_ACTIVE) {
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
        case PmuState::AWAITING_ACK:
        case PmuState::WAKE_ACTIVE: {
            // Check wake timeout (safety net for both states)
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

        case PmuState::AWAITING_ACK:
            context_.startTime = now;
            if (event == PmuEvent::BOOT_COMPLETE) {
                // After boot animation: short grace period, periodic wake
                context_.type = WakeType::PERIODIC;
                context_.timeoutMs = BOOT_GRACE_PERIOD_MS;
            } else {
                // After RTC wakeup: safe default, overridden by callback via setWakeType()
                context_.timeoutMs = PERIODIC_WAKE_TIMEOUT_MS;
            }
            break;

        case PmuState::WAKE_ACTIVE:
            // Coming from BOOTING or SLEEPING with early ack - set up context
            if (context_.startTime == 0) {
                context_.startTime = now;
                context_.type = WakeType::PERIODIC;
                context_.timeoutMs = PERIODIC_WAKE_TIMEOUT_MS;
            }
            // For transitions from AWAITING_ACK, context is already set
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
        case PmuState::AWAITING_ACK:
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
        case PmuState::SLEEPING:
            return "SLEEPING";
        case PmuState::AWAITING_ACK:
            return "AWAITING_ACK";
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
        case PmuEvent::NOTIFICATION_ACK:
            return "NOTIFICATION_ACK";
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
