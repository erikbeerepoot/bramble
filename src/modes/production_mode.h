#pragma once
#include "application_mode.h"
#include "hal/valve_controller.h"

/**
 * @brief Production mode for actual sensor node operation
 */
class ProductionMode : public ApplicationMode {
private:
    ValveController valve_controller_;
    
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
    void onActuatorCommand(const ActuatorPayload* payload) override;
};