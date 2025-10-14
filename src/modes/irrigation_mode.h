#pragma once

#include "application_mode.h"
#include "../hal/valve_controller.h"
#include "../hal/pmu_client.h"

/**
 * @brief Irrigation node mode for valve control
 *
 * Irrigation node that handles valve commands from the hub and
 * integrates with the STM32 PMU for power management and scheduled watering.
 */
class IrrigationMode : public ApplicationMode {
private:
    ValveController valve_controller_;
    PmuClient* pmu_client_;
    bool pmu_available_;

    // PMU callback handlers
    void handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry* entry);
    void handleScheduleComplete();

public:
    using ApplicationMode::ApplicationMode;

protected:
    void onStart() override;
    void onLoop() override;
    void onActuatorCommand(const ActuatorPayload* payload) override;
};