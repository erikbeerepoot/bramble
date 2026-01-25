#include "base_state_machine.h"

#include "../hal/logger.h"

static Logger logger("StateMachine");

void BaseStateMachine::update(const BaseHardwareState &hardware_state)
{
    BaseState new_state = deriveState(hardware_state);

    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
}

BaseState BaseStateMachine::deriveState(const BaseHardwareState &hardware_state) const
{
    // Error state is terminal
    if (error_) {
        return BaseState::ERROR;
    }

    // Still initializing hardware
    if (!initialized_) {
        return BaseState::INITIALIZING;
    }

    // Check hardware state
    if (!hardware_state.rtc_running) {
        return BaseState::AWAITING_TIME;
    }

    return BaseState::OPERATIONAL;
}

const char *BaseStateMachine::stateName(BaseState state)
{
    switch (state) {
        case BaseState::INITIALIZING:
            return "INITIALIZING";
        case BaseState::AWAITING_TIME:
            return "AWAITING_TIME";
        case BaseState::OPERATIONAL:
            return "OPERATIONAL";
        case BaseState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
