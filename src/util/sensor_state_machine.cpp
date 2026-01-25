#include "sensor_state_machine.h"

#include "../hal/logger.h"

static Logger logger("SensorSM");

void SensorStateMachine::update(const SensorHardwareState &hardware_state)
{
    SensorState new_state = deriveState(hardware_state);

    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
}

SensorState SensorStateMachine::deriveState(const SensorHardwareState &hardware_state) const
{
    if (error_) {
        return SensorState::ERROR;
    }

    if (!initialized_) {
        return SensorState::INITIALIZING;
    }

    if (!hardware_state.rtc_running) {
        return SensorState::AWAITING_TIME;
    }

    // RTC is running - check sensor status
    if (hardware_state.sensor_init_attempted && !hardware_state.sensor_initialized) {
        return SensorState::DEGRADED_NO_SENSOR;
    }

    if (hardware_state.sensor_initialized) {
        return SensorState::OPERATIONAL;
    }

    // RTC synced but sensor not yet attempted - still operational for timestamp purposes
    return SensorState::OPERATIONAL;
}

const char *SensorStateMachine::stateName(SensorState state)
{
    switch (state) {
        case SensorState::INITIALIZING:
            return "INITIALIZING";
        case SensorState::AWAITING_TIME:
            return "AWAITING_TIME";
        case SensorState::OPERATIONAL:
            return "OPERATIONAL";
        case SensorState::DEGRADED_NO_SENSOR:
            return "DEGRADED_NO_SENSOR";
        case SensorState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
