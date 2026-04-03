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

void IrrigationStateMachine::reportHeartbeatSent()
{
    if (state_ != IrrigationState::AWAITING_TIME) {
        logger.warn("reportHeartbeatSent() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.debug("Heartbeat sent, awaiting hub response");
    transitionTo(IrrigationState::SYNCING_TIME);
}

void IrrigationStateMachine::reportTimeSyncComplete()
{
    if (state_ != IrrigationState::AWAITING_TIME && state_ != IrrigationState::SYNCING_TIME) {
        logger.warn("reportTimeSyncComplete() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.info("Time sync complete");
    transitionTo(IrrigationState::CHECKING_UPDATES);
}

void IrrigationStateMachine::reportSyncTimeout()
{
    if (state_ != IrrigationState::SYNCING_TIME) {
        logger.warn("reportSyncTimeout() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.warn("Time sync timed out - sleeping for retry");
    transitionTo(IrrigationState::READY_FOR_SLEEP);
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
        case IrrigationState::SYNCING_TIME:
            return "SYNCING_TIME";
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
