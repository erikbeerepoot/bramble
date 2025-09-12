#pragma once

#include "application_mode.h"
#include "../hal/valve_controller.h"

/**
 * @brief Irrigation node mode for valve control
 * 
 * Simple irrigation node that handles valve commands from the hub.
 * Reports valve control capability and responds to valve on/off commands.
 */
class IrrigationMode : public ApplicationMode {
private:
    ValveController valve_controller_;
    
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
    void onActuatorCommand(const ActuatorPayload* payload) override;
};