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
    updateState();
}

void SensorStateMachine::markError()
{
    if (error_) {
        return;  // Already in error
    }
    error_ = true;
    logger.error("Unrecoverable error reported");
    updateState();
}

void SensorStateMachine::reportRtcSynced()
{
    if (rtc_synced_) {
        return;  // Already synced
    }
    rtc_synced_ = true;
    logger.info("RTC synchronized");
    updateState();
}

void SensorStateMachine::reportRtcLost()
{
    if (!rtc_synced_) {
        return;  // Already not synced
    }
    rtc_synced_ = false;
    logger.warn("RTC synchronization lost");
    updateState();
}

void SensorStateMachine::reportSensorInitSuccess()
{
    sensor_init_attempted_ = true;
    sensor_initialized_ = true;
    logger.info("Sensor initialization successful");
    updateState();
}

void SensorStateMachine::reportSensorInitFailure()
{
    sensor_init_attempted_ = true;
    sensor_initialized_ = false;
    logger.warn("Sensor initialization failed");
    updateState();
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
    // Only restart cycle if we're actually in READY_FOR_SLEEP
    // The PMU wake notification can arrive asynchronously after the state machine
    // has already started processing (e.g., via getDateTime callback path)
    if (state_ != SensorState::READY_FOR_SLEEP) {
        logger.debug("Wake notification ignored - already active (state: %s)", stateName(state_));
        return true;  // Return true since we're already awake and processing
    }

    // Reset per-cycle state
    expected_responses_ = 0;

    if (!rtc_synced_) {
        // Need time sync first - caller should send heartbeat
        logger.info("Wake from sleep but RTC not synced - need time sync");
        return false;
    }

    // Restart the sensor cycle based on hardware state
    if (sensor_initialized_) {
        // Sensor working - read it
        logger.info("Wake from sleep - starting sensor read");
        transitionTo(SensorState::READING_SENSOR);
    } else if (sensor_init_attempted_) {
        // Sensor failed (degraded mode) - skip to backlog check
        logger.info("Wake from sleep (degraded) - checking backlog");
        transitionTo(SensorState::CHECKING_BACKLOG);
    } else {
        // Sensor not yet initialized - go through TIME_SYNCED to try init
        logger.info("Wake from sleep - attempting sensor init");
        transitionTo(SensorState::TIME_SYNCED);
    }
    return true;
}

void SensorStateMachine::updateState()
{
    SensorState new_state = state_;

    // Derive state from internal flags
    // Note: workflow states (SYNCING_TIME, READING_SENSOR, CHECKING_BACKLOG,
    // TRANSMITTING, READY_FOR_SLEEP) are handled by transitionTo() instead
    if (error_) {
        new_state = SensorState::ERROR;
    } else if (!initialized_) {
        new_state = SensorState::INITIALIZING;
    } else if (!rtc_synced_) {
        // Only go to AWAITING_TIME from INITIALIZING
        // Don't override SYNCING_TIME (we're waiting for hub response)
        if (state_ == SensorState::INITIALIZING) {
            new_state = SensorState::AWAITING_TIME;
        }
    } else if (sensor_initialized_) {
        // RTC synced and sensor working - go to READING_SENSOR
        // Only from TIME_SYNCED (after sensor init success)
        if (state_ == SensorState::TIME_SYNCED) {
            new_state = SensorState::READING_SENSOR;
        }
    } else if (sensor_init_attempted_) {
        // Sensor init failed - degraded mode, skip to backlog check
        if (state_ == SensorState::TIME_SYNCED) {
            new_state = SensorState::DEGRADED_NO_SENSOR;
        }
    } else if (state_ == SensorState::AWAITING_TIME || state_ == SensorState::SYNCING_TIME) {
        // RTC just synced - go to TIME_SYNCED
        new_state = SensorState::TIME_SYNCED;
    }

    // Only log and callback on actual state change
    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        previous_state_ = state_;
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
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
