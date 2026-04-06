#include "irrigation_state_machine.h"

#include "../hal/logger.h"

static Logger logger("IrrigationSM");

void IrrigationStateMachine::markInitialized()
{
    if (initialized_) {
        return;
    }
    initialized_ = true;
    logger.debug("Hardware initialization complete");
    transitionTo(IrrigationState::REGISTERING);
}

void IrrigationStateMachine::markError()
{
    if (error_) {
        return;
    }
    error_ = true;
    logger.error("Unrecoverable error reported");
    transitionTo(IrrigationState::ERROR);
}

void IrrigationStateMachine::reportRegistrationComplete()
{
    if (state_ != IrrigationState::REGISTERING) {
        logger.warn("reportRegistrationComplete() called in unexpected state: %s",
                    stateName(state_));
        return;
    }
    logger.info("Registration complete");
    transitionTo(IrrigationState::AWAITING_TIME);
}

void IrrigationStateMachine::reportRegistrationTimeout()
{
    if (state_ != IrrigationState::REGISTERING) {
        logger.warn("reportRegistrationTimeout() called in unexpected state: %s",
                    stateName(state_));
        return;
    }
    logger.warn("Registration timed out - will retry on next wake");
    transitionTo(IrrigationState::READY_FOR_SLEEP);
}

void IrrigationStateMachine::reportHeartbeatSending()
{
    if (state_ != IrrigationState::AWAITING_TIME) {
        logger.warn("reportHeartbeatSending() called in unexpected state: %s", stateName(state_));
        return;
    }
    transitionTo(IrrigationState::SENDING_HEARTBEAT);
}

void IrrigationStateMachine::reportHeartbeatResponseReceived()
{
    if (state_ != IrrigationState::SENDING_HEARTBEAT) {
        logger.warn("reportHeartbeatResponseReceived() called in unexpected state: %s",
                    stateName(state_));
        return;
    }
    logger.info("Heartbeat response received");
    transitionTo(IrrigationState::CHECKING_UPDATES);
}

void IrrigationStateMachine::reportReregistrationRequired()
{
    if (state_ != IrrigationState::SENDING_HEARTBEAT) {
        logger.warn("reportReregistrationRequired() called in unexpected state: %s",
                    stateName(state_));
        return;
    }
    logger.info("Hub requires re-registration");
    transitionTo(IrrigationState::AWAITING_REGISTRATION);
}

void IrrigationStateMachine::reportReregistrationComplete()
{
    if (state_ != IrrigationState::AWAITING_REGISTRATION) {
        logger.warn("reportReregistrationComplete() called in unexpected state: %s",
                    stateName(state_));
        return;
    }
    logger.info("Re-registration complete");
    transitionTo(IrrigationState::SENDING_HEARTBEAT);
}

void IrrigationStateMachine::reportUpdateReceived(bool hasMore)
{
    if (state_ != IrrigationState::CHECKING_UPDATES) {
        logger.warn("reportUpdateReceived() called in unexpected state: %s", stateName(state_));
        return;
    }
    if (hasMore) {
        logger.debug("Update available - applying");
        transitionTo(IrrigationState::APPLYING_UPDATE);
    } else {
        logger.info("No more updates");
        transitionTo(IrrigationState::READY_FOR_SLEEP);
    }
}

void IrrigationStateMachine::reportUpdateApplied()
{
    if (state_ != IrrigationState::APPLYING_UPDATE) {
        logger.warn("reportUpdateApplied() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.debug("Update applied - checking for more");
    transitionTo(IrrigationState::CHECKING_UPDATES);
}

void IrrigationStateMachine::reportUpdateFailed()
{
    if (state_ != IrrigationState::CHECKING_UPDATES && state_ != IrrigationState::APPLYING_UPDATE) {
        logger.warn("reportUpdateFailed() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.warn("Update failed - sleeping");
    transitionTo(IrrigationState::READY_FOR_SLEEP);
}

void IrrigationStateMachine::reportValveOpened()
{
    logger.info("Valve opened");
    transitionTo(IrrigationState::VALVE_ACTIVE);
}

void IrrigationStateMachine::reportValveClosed()
{
    if (state_ != IrrigationState::VALVE_ACTIVE) {
        logger.warn("reportValveClosed() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.info("Valve closed");
    transitionTo(IrrigationState::READY_FOR_SLEEP);
}

void IrrigationStateMachine::reportWakeFromSleep(PMU::WakeReason reason)
{
    if (state_ != IrrigationState::READY_FOR_SLEEP) {
        logger.debug("Wake notification ignored - already active (state: %s)", stateName(state_));
        return;
    }

    switch (reason) {
        case PMU::WakeReason::Scheduled:
            logger.info("Scheduled wake - valve activation");
            transitionTo(IrrigationState::VALVE_ACTIVE);
            break;
        case PMU::WakeReason::ValveTimer:
            logger.info("Valve timer wake - closing valve then normal cycle");
            transitionTo(IrrigationState::AWAITING_TIME);
            break;
        case PMU::WakeReason::Periodic:
        case PMU::WakeReason::External:
            logger.info("Wake from sleep (%s)",
                        reason == PMU::WakeReason::Periodic ? "periodic" : "external");
            transitionTo(IrrigationState::AWAITING_TIME);
            break;
    }
}

void IrrigationStateMachine::reportWatchdogTimeout()
{
    logger.warn("State watchdog timeout in %s - forcing sleep", stateName(state_));
    transitionTo(IrrigationState::READY_FOR_SLEEP);
}

void IrrigationStateMachine::transitionTo(IrrigationState newState)
{
    if (newState == state_) {
        return;
    }

    logger.info("State: %s -> %s", stateName(state_), stateName(newState));
    previous_state_ = state_;
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
        case IrrigationState::REGISTERING:
            return "REGISTERING";
        case IrrigationState::AWAITING_TIME:
            return "AWAITING_TIME";
        case IrrigationState::SENDING_HEARTBEAT:
            return "SENDING_HEARTBEAT";
        case IrrigationState::AWAITING_REGISTRATION:
            return "AWAITING_REGISTRATION";
        case IrrigationState::CHECKING_UPDATES:
            return "CHECKING_UPDATES";
        case IrrigationState::APPLYING_UPDATE:
            return "APPLYING_UPDATE";
        case IrrigationState::VALVE_ACTIVE:
            return "VALVE_ACTIVE";
        case IrrigationState::READY_FOR_SLEEP:
            return "READY_FOR_SLEEP";
        case IrrigationState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
