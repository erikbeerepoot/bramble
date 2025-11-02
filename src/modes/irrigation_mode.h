#pragma once

#include "application_mode.h"
#include "../hal/valve_controller.h"
#include "../hal/pmu_client.h"

/**
 * @brief Update pull state tracking
 */
struct UpdatePullState {
    uint8_t current_sequence;        // Node's current sequence number
    uint32_t last_keepawake_ms;      // Last keep-awake call time
    bool processing;                 // Whether actively processing an update
    uint32_t timeout_ms;             // Timeout for update processing (5 seconds)
    uint8_t pending_check_seq;       // Sequence number of pending CHECK_UPDATES message

    UpdatePullState() : current_sequence(0), last_keepawake_ms(0),
                       processing(false), timeout_ms(5000), pending_check_seq(0) {}

    void reset() {
        processing = false;
        last_keepawake_ms = 0;
        pending_check_seq = 0;
    }
};

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
    UpdatePullState update_state_;

    // PMU callback handlers
    void handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry* entry);
    void handleScheduleComplete();

    // Update handling
    void sendCheckUpdates();
    void onUpdateAvailable(const UpdateAvailablePayload* payload) override;

public:
    using ApplicationMode::ApplicationMode;

protected:
    void onStart() override;
    void onLoop() override;
    void onActuatorCommand(const ActuatorPayload* payload) override;
    void onHeartbeatResponse(const HeartbeatResponsePayload* payload) override;
};