#include "pmu_state_machine.h"

PmuStateMachine::PmuStateMachine(GetTickCallback getTick) : getTick_(getTick) {}

void PmuStateMachine::reportBootComplete()
{
    if (state_ != PmuState::BOOTING) {
        return;  // Ignore if not in BOOTING state
    }

    context_.reset();
    transitionTo(PmuState::SLEEPING);
}

void PmuStateMachine::reportScheduledWake(const PMU::ScheduleEntry &entry)
{
    if (state_ != PmuState::SLEEPING) {
        return;  // Can only wake from SLEEPING
    }

    context_.reset();
    context_.startTime = getTick_();
    context_.hasScheduleEntry = true;
    context_.scheduleEntry = entry;
    // Timeout = duration (seconds) * 1000 + grace period
    context_.timeoutMs = static_cast<uint32_t>(entry.duration) * 1000 + SCHEDULED_GRACE_PERIOD_MS;

    transitionTo(PmuState::SCHEDULED_WAKE);
}

void PmuStateMachine::reportPeriodicWake()
{
    if (state_ != PmuState::SLEEPING) {
        return;  // Can only wake from SLEEPING
    }

    context_.reset();
    context_.startTime = getTick_();
    context_.timeoutMs = PERIODIC_WAKE_TIMEOUT_MS;

    transitionTo(PmuState::PERIODIC_WAKE);
}

void PmuStateMachine::reportReadyForSleep()
{
    if (state_ != PmuState::SCHEDULED_WAKE && state_ != PmuState::PERIODIC_WAKE) {
        return;  // Only valid during wake states
    }

    context_.reset();
    transitionTo(PmuState::SLEEPING);
}

void PmuStateMachine::reportError()
{
    context_.reset();
    transitionTo(PmuState::ERROR);
}

void PmuStateMachine::markWakeNotificationSent()
{
    context_.notificationSent = true;
}

bool PmuStateMachine::update()
{
    switch (state_) {
        case PmuState::BOOTING:
            // Stay awake during boot animation
            return true;

        case PmuState::SLEEPING:
            // Can enter STOP mode
            return false;

        case PmuState::SCHEDULED_WAKE:
        case PmuState::PERIODIC_WAKE: {
            // Check for timeout
            uint32_t elapsed = getTick_() - context_.startTime;
            if (elapsed >= context_.timeoutMs) {
                // Timeout - force return to sleep
                context_.reset();
                transitionTo(PmuState::SLEEPING);
                return false;
            }
            // Stay awake
            return true;
        }

        case PmuState::ERROR:
            // Stay awake in error state (for debugging)
            return true;

        default:
            return false;
    }
}

bool PmuStateMachine::shouldPowerRp2040() const
{
    switch (state_) {
        case PmuState::BOOTING:
        case PmuState::SCHEDULED_WAKE:
        case PmuState::PERIODIC_WAKE:
            return true;

        case PmuState::SLEEPING:
        case PmuState::ERROR:
        default:
            return false;
    }
}

bool PmuStateMachine::shouldSendWakeNotification() const
{
    if (state_ != PmuState::SCHEDULED_WAKE && state_ != PmuState::PERIODIC_WAKE) {
        return false;
    }
    return !context_.notificationSent;
}

const PMU::ScheduleEntry *PmuStateMachine::getScheduleEntry() const
{
    if (state_ == PmuState::SCHEDULED_WAKE && context_.hasScheduleEntry) {
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
        case PmuState::SCHEDULED_WAKE:
            return "SCHEDULED_WAKE";
        case PmuState::PERIODIC_WAKE:
            return "PERIODIC_WAKE";
        case PmuState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

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
