#pragma once

#include <memory>

#include "../config/curtain_config.h"
#include "../hal/cht832x.h"
#include "../hal/curtain_controller.h"
#include "../hal/pmu_client.h"
#include "../lora/batch_transmitter.h"
#include "../storage/sensor_data_record.h"
#include "../util/greenhouse_state_machine.h"
#include "application_mode.h"

/**
 * @brief Greenhouse node mode for roll-up curtain control
 *
 * Controls motorized roll-up curtain sides via two GPIO signals.
 * Always-on (mains powered, no sleep). Uses PMU only for RTC timekeeping.
 */
class GreenhouseMode : public ApplicationMode {
private:
    CurtainController curtain_controller_;
    CurtainConfigManager *curtain_config_;
    PmuClient *pmu_client_;
    bool pmu_available_;
    GreenhouseStateMachine greenhouse_state_;

    // Optional temperature/humidity sensor (auto-detected on I2C at startup)
    std::unique_ptr<CHT832X> sensor_;
    std::unique_ptr<BatchTransmitter> transmitter_;
    SensorDataRecord current_reading_ = {};
    bool sensor_available_ = false;

    void updateGreenhouseState();
    void readAndTransmitSensorData(uint32_t current_time);

public:
    using ApplicationMode::ApplicationMode;

protected:
    void onStart() override;
    void onLoop() override;
    void onActuatorCommand(const ActuatorPayload *payload) override;
    void onModeSpecificUpdate(const UpdateAvailablePayload *payload, uint8_t hub_sequence) override;
    void onHeartbeatResponse(const HeartbeatResponsePayload *payload) override;
    void onRebootRequested() override;
    void onFactoryResetRequested() override;
    bool shouldSleep() const override { return false; }
};
