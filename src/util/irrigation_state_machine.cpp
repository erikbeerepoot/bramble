#include "irrigation_state_machine.h"

#include "../hal/logger.h"

static Logger logger("IrrigationSM");

void IrrigationStateMachine::reportValveOpened()
{
    if (!isOperational()) {
        logger.warn("reportValveOpened() called in non-operational state: %s", stateName(state_));
        return;
    }
    transitionTo(IrrigationState::VALVE_ACTIVE);
}

void IrrigationStateMachine::reportValveClosed()
{
    if (state_ != IrrigationState::VALVE_ACTIVE) {
        logger.warn("reportValveClosed() called in unexpected state: %s", stateName(state_));
        return;
    }
    transitionTo(IrrigationState::IDLE);
}

void IrrigationStateMachine::reportUpdatePullStarted()
{
    if (state_ == IrrigationState::CHECKING_UPDATES) {
        return;  // Already checking - no-op (e.g. pulling next update after applying one)
    }
    if (!isOperational()) {
        logger.warn("reportUpdatePullStarted() called in non-operational state: %s",
                    stateName(state_));
        return;
    }
    transitionTo(IrrigationState::CHECKING_UPDATES);
}

void IrrigationStateMachine::reportUpdateReceived()
{
    if (state_ != IrrigationState::CHECKING_UPDATES) {
        logger.warn("reportUpdateReceived() called in unexpected state: %s", stateName(state_));
        return;
    }
    transitionTo(IrrigationState::APPLYING_UPDATES);
}

void IrrigationStateMachine::reportUpdateApplied()
{
    if (state_ != IrrigationState::APPLYING_UPDATES) {
        logger.warn("reportUpdateApplied() called in unexpected state: %s", stateName(state_));
        return;
    }
    transitionTo(IrrigationState::CHECKING_UPDATES);
}

void IrrigationStateMachine::reportUpdatesComplete()
{
    if (state_ != IrrigationState::CHECKING_UPDATES &&
        state_ != IrrigationState::APPLYING_UPDATES) {
        logger.warn("reportUpdatesComplete() called in unexpected state: %s", stateName(state_));
        return;
    }
    transitionTo(IrrigationState::IDLE);
}

void IrrigationStateMachine::updateState()
{
    IrrigationState new_state = state_;

    if (error_) {
        new_state = IrrigationState::ERROR;
    } else if (!initialized_) {
        new_state = IrrigationState::INITIALIZING;
    } else if (!rtc_synced_) {
        // Only transition to AWAITING_TIME from INITIALIZING
        // Don't override workflow states (they handle RTC loss via reportRtcLost)
        if (state_ == IrrigationState::INITIALIZING) {
            new_state = IrrigationState::AWAITING_TIME;
        }
    } else if (state_ == IrrigationState::AWAITING_TIME) {
        // RTC just synced — go to IDLE
        new_state = IrrigationState::IDLE;
    }

    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
}

void IrrigationStateMachine::transitionTo(IrrigationState newState)
{
    if (newState == state_) {
        return;
    }

    logger.info("State: %s -> %s", stateName(state_), stateName(newState));
    state_ = newState;

    if (callback_) {
        callback_(state_);
    }
}

const char *IrrigationStateMachine::stateName(IrrigationState state)
{
    switch (state) {
        case IrrigationState::INITIALIZING:
            return "INITIALIZING";
        case IrrigationState::AWAITING_TIME:
            return "AWAITING_TIME";
        case IrrigationState::IDLE:
            return "IDLE";
        case IrrigationState::CHECKING_UPDATES:
            return "CHECKING_UPDATES";
        case IrrigationState::APPLYING_UPDATES:
            return "APPLYING_UPDATES";
        case IrrigationState::VALVE_ACTIVE:
            return "VALVE_ACTIVE";
        case IrrigationState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
