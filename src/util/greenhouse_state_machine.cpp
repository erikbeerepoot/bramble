#include "greenhouse_state_machine.h"

#include "../hal/logger.h"

static Logger logger("GreenSM");

void GreenhouseStateMachine::update(const GreenhouseHardwareState &hardware_state)
{
    GreenhouseState new_state = deriveState(hardware_state);

    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
}

GreenhouseState GreenhouseStateMachine::deriveState(
    const GreenhouseHardwareState &hardware_state) const
{
    if (error_) {
        return GreenhouseState::ERROR;
    }

    if (!initialized_) {
        return GreenhouseState::INITIALIZING;
    }

    if (!hardware_state.rtc_running) {
        return GreenhouseState::AWAITING_TIME;
    }

    if (hardware_state.calibrating) {
        return GreenhouseState::CALIBRATING;
    }

    if (hardware_state.opening) {
        return GreenhouseState::CURTAIN_OPENING;
    }

    if (hardware_state.closing) {
        return GreenhouseState::CURTAIN_CLOSING;
    }

    return GreenhouseState::IDLE;
}

const char *GreenhouseStateMachine::stateName(GreenhouseState state)
{
    switch (state) {
        case GreenhouseState::INITIALIZING:
            return "INITIALIZING";
        case GreenhouseState::AWAITING_TIME:
            return "AWAITING_TIME";
        case GreenhouseState::IDLE:
            return "IDLE";
        case GreenhouseState::CURTAIN_OPENING:
            return "CURTAIN_OPENING";
        case GreenhouseState::CURTAIN_CLOSING:
            return "CURTAIN_CLOSING";
        case GreenhouseState::CALIBRATING:
            return "CALIBRATING";
        case GreenhouseState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
