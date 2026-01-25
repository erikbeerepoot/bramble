#include "irrigation_state_machine.h"

#include "../hal/logger.h"

static Logger logger("IrrigationSM");

void IrrigationStateMachine::update(const IrrigationHardwareState &hw)
{
    IrrigationState new_state = deriveState(hw);

    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
}

IrrigationState IrrigationStateMachine::deriveState(const IrrigationHardwareState &hw) const
{
    if (error_) {
        return IrrigationState::ERROR;
    }

    if (!initialized_) {
        return IrrigationState::INITIALIZING;
    }

    if (!hw.rtc_running) {
        return IrrigationState::AWAITING_TIME;
    }

    // RTC is running - check operational state
    if (hw.valve_open) {
        return IrrigationState::VALVE_ACTIVE;
    }

    if (hw.applying_update) {
        return IrrigationState::APPLYING_UPDATES;
    }

    if (hw.update_pending) {
        return IrrigationState::CHECKING_UPDATES;
    }

    return IrrigationState::IDLE;
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
