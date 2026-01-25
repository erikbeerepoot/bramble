#include "sensor_state_machine.h"

#include "../hal/logger.h"

static Logger logger("SensorSM");

void SensorStateMachine::update(const SensorHardwareState &hw)
{
    SensorState new_state = deriveState(hw);

    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
}

SensorState SensorStateMachine::deriveState(const SensorHardwareState &hw) const
{
    if (error_) {
        return SensorState::ERROR;
    }

    if (!initialized_) {
        return SensorState::INITIALIZING;
    }

    if (!hw.rtc_running) {
        return SensorState::AWAITING_TIME;
    }

    // RTC is running - check sensor status
    if (hw.sensor_init_attempted && !hw.sensor_initialized) {
        return SensorState::DEGRADED_NO_SENSOR;
    }

    if (hw.sensor_initialized) {
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
