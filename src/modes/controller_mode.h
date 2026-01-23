#pragma once

#include "../hal/controller_inputs.h"
#include "application_mode.h"

/**
 * @brief Controller hub mode for irrigation management
 *
 * Controls irrigation valves based on A0/A1 input signals.
 * Monitors digital inputs and sends valve commands to irrigation nodes.
 * Functions as network hub managing irrigation node communications.
 */
class ControllerMode : public ApplicationMode {
private:
    ControllerInputs input_handler_;
    uint16_t irrigation_node_address_;

    /**
     * @brief Handle input state changes from A0/A1
     * @param input_id Input number (0=A0, 1=A1)
     * @param state New input state (true=high/on, false=low/off)
     */
    void onInputChange(uint8_t input_id, bool state);

    /**
     * @brief Send valve command to irrigation node
     * @param valve_id Valve number (0 or 1)
     * @param turn_on True to turn on valve, false to turn off
     */
    void sendValveCommand(uint8_t valve_id, bool turn_on);

public:
    using ApplicationMode::ApplicationMode;

protected:
    void onStart() override;
};