#include "sensor_state_machine.h"

#include "../hal/logger.h"

static Logger logger("SensorSM");

void SensorStateMachine::markInitialized()
{
    if (initialized_) {
        return;  // Already initialized
    }
    initialized_ = true;
    logger.debug("Hardware initialization complete");
    transitionTo(SensorState::REGISTERING);
}

void SensorStateMachine::markError()
{
    if (error_) {
        return;  // Already in error
    }
    error_ = true;
    logger.error("Unrecoverable error reported");
    transitionTo(SensorState::ERROR);
}

void SensorStateMachine::reportRegistrationSent()
{
    // Logging-only — the transition to REGISTERING happens via markInitialized()
    if (state_ != SensorState::REGISTERING) {
        logger.warn("reportRegistrationSent() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.debug("Registration message sent, awaiting hub response");
}

void SensorStateMachine::reportRegistrationComplete()
{
    if (state_ != SensorState::REGISTERING) {
        logger.warn("reportRegistrationComplete() called in unexpected state: %s",
                    stateName(state_));
        return;
    }
    logger.info("Registration complete");
    transitionTo(SensorState::AWAITING_TIME);
}

void SensorStateMachine::reportRegistrationTimeout()
{
    if (state_ != SensorState::REGISTERING) {
        logger.warn("reportRegistrationTimeout() called in unexpected state: %s",
                    stateName(state_));
        return;
    }
    logger.warn("Registration timed out - will retry on next wake");
    transitionTo(SensorState::READY_FOR_SLEEP);
}

void SensorStateMachine::reportSyncTimeout()
{
    if (state_ != SensorState::SYNCING_TIME) {
        logger.warn("reportSyncTimeout() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.warn("Time sync timed out - sleeping for retry");
    transitionTo(SensorState::READY_FOR_SLEEP);
}

void SensorStateMachine::reportTimeSyncComplete()
{
    if (state_ != SensorState::AWAITING_TIME && state_ != SensorState::SYNCING_TIME) {
        logger.warn("reportTimeSyncComplete() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.info("Time sync complete");
    transitionTo(SensorState::TIME_SYNCED);
}

void SensorStateMachine::reportSensorInitSuccess()
{
    if (state_ != SensorState::TIME_SYNCED) {
        logger.warn("reportSensorInitSuccess() called in unexpected state: %s", stateName(state_));
        return;
    }
    sensor_init_attempted_ = true;
    sensor_initialized_ = true;
    logger.info("Sensor initialization successful");
    transitionTo(SensorState::READING_SENSOR);
}

void SensorStateMachine::reportSensorInitFailure()
{
    if (state_ != SensorState::TIME_SYNCED) {
        logger.warn("reportSensorInitFailure() called in unexpected state: %s", stateName(state_));
        return;
    }
    sensor_init_attempted_ = true;
    sensor_initialized_ = false;
    logger.warn("Sensor initialization failed");
    transitionTo(SensorState::DEGRADED_NO_SENSOR);
}

void SensorStateMachine::reportDegradedSleepReady()
{
    if (state_ != SensorState::DEGRADED_NO_SENSOR) {
        logger.warn("reportDegradedSleepReady() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.info("Sensor degraded - sleeping for lazy retry on next wake");
    transitionTo(SensorState::READY_FOR_SLEEP);
}

void SensorStateMachine::reportWatchdogTimeout()
{
    logger.warn("State watchdog timeout in %s - forcing sleep", stateName(state_));
    transitionTo(SensorState::READY_FOR_SLEEP);
}

void SensorStateMachine::reportHeartbeatSent()
{
    if (state_ != SensorState::AWAITING_TIME) {
        logger.warn("reportHeartbeatSent() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.debug("Heartbeat sent, awaiting hub response");
    transitionTo(SensorState::SYNCING_TIME);
}

void SensorStateMachine::reportReadComplete()
{
    if (state_ != SensorState::READING_SENSOR) {
        logger.warn("reportReadComplete() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.debug("Sensor read complete");
    transitionTo(SensorState::CHECKING_BACKLOG);
}

void SensorStateMachine::reportCheckComplete(bool needsTransmit)
{
    if (state_ != SensorState::CHECKING_BACKLOG) {
        logger.warn("reportCheckComplete() called in unexpected state: %s", stateName(state_));
        return;
    }
    if (needsTransmit) {
        logger.debug("Backlog needs transmission");
        transitionTo(SensorState::TRANSMITTING);
    } else if (hasExpectedResponses()) {
        logger.debug("No transmission needed, listening for responses");
        transitionTo(SensorState::LISTENING);
    } else {
        logger.debug("No transmission needed, no responses expected");
        transitionTo(SensorState::READY_FOR_SLEEP);
    }
}

void SensorStateMachine::reportTransmitComplete()
{
    if (state_ != SensorState::TRANSMITTING) {
        logger.warn("reportTransmitComplete() called in unexpected state: %s", stateName(state_));
        return;
    }
    if (hasExpectedResponses()) {
        logger.debug("Transmission complete, listening for responses");
        transitionTo(SensorState::LISTENING);
    } else {
        logger.debug("Transmission complete, no responses expected");
        transitionTo(SensorState::READY_FOR_SLEEP);
    }
}

void SensorStateMachine::expectResponse()
{
    expected_responses_++;
    logger.debug("Expected responses: %d", expected_responses_);
}

void SensorStateMachine::reportListenComplete()
{
    if (state_ != SensorState::LISTENING) {
        logger.warn("reportListenComplete() called in unexpected state: %s", stateName(state_));
        return;
    }
    logger.debug("Listen window complete");
    transitionTo(SensorState::READY_FOR_SLEEP);
}

bool SensorStateMachine::reportWakeFromSleep()
{
    if (state_ != SensorState::READY_FOR_SLEEP) {
        logger.debug("Wake notification ignored - already active (state: %s)", stateName(state_));
        return true;
    }

    expected_responses_ = 0;
    transitionTo(SensorState::AWAITING_TIME);
    return true;
}

void SensorStateMachine::transitionTo(SensorState newState)
{
    if (newState == state_) {
        return;  // No change
    }

    logger.info("State: %s -> %s", stateName(state_), stateName(newState));
    previous_state_ = state_;
    state_ = newState;

    if (callback_) {
        callback_(state_);
    }
}

const char *SensorStateMachine::stateName(SensorState state)
{
    switch (state) {
        case SensorState::INITIALIZING:
            return "INITIALIZING";
        case SensorState::REGISTERING:
            return "REGISTERING";
        case SensorState::AWAITING_TIME:
            return "AWAITING_TIME";
        case SensorState::SYNCING_TIME:
            return "SYNCING_TIME";
        case SensorState::TIME_SYNCED:
            return "TIME_SYNCED";
        case SensorState::READING_SENSOR:
            return "READING_SENSOR";
        case SensorState::CHECKING_BACKLOG:
            return "CHECKING_BACKLOG";
        case SensorState::TRANSMITTING:
            return "TRANSMITTING";
        case SensorState::LISTENING:
            return "LISTENING";
        case SensorState::READY_FOR_SLEEP:
            return "READY_FOR_SLEEP";
        case SensorState::DEGRADED_NO_SENSOR:
            return "DEGRADED_NO_SENSOR";
        case SensorState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
