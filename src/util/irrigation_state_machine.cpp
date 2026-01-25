#include "irrigation_state_machine.h"

#include "../hal/logger.h"

static Logger logger("IrrigationSM");

void IrrigationStateMachine::update(const IrrigationHardwareState &hardware_state)
{
    IrrigationState new_state = deriveState(hardware_state);

    if (new_state != state_) {
        logger.info("State: %s -> %s", stateName(state_), stateName(new_state));
        state_ = new_state;

        if (callback_) {
            callback_(state_);
        }
    }
}

IrrigationState IrrigationStateMachine::deriveState(const IrrigationHardwareState &hardware_state) const
{
    if (error_) {
        return IrrigationState::ERROR;
    }

    if (!initialized_) {
        return IrrigationState::INITIALIZING;
    }

    if (!hardware_state.rtc_running) {
        return IrrigationState::AWAITING_TIME;
    }

    // RTC is running - check operational state
    if (hardware_state.valve_open) {
        return IrrigationState::VALVE_ACTIVE;
    }

    if (hardware_state.applying_update) {
        return IrrigationState::APPLYING_UPDATES;
    }

    if (hardware_state.update_pending) {
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
