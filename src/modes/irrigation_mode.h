#pragma once

#include "../hal/pmu_client.h"
#include "../hal/valve_controller.h"
#include "../util/irrigation_state_machine.h"
#include "application_mode.h"

/**
 * @brief Update pull state tracking
 *
 * Tracks sequence numbers and message state for update processing.
 */
struct UpdatePullState {
    uint8_t current_sequence;   // Node's current sequence number
    uint8_t pending_check_seq;  // Sequence number of pending CHECK_UPDATES message

    UpdatePullState() : current_sequence(0), pending_check_seq(0) {}

    void reset() { pending_check_seq = 0; }
};

/**
 * @brief Irrigation node mode for valve control
 *
 * Irrigation node that handles valve commands from the hub and
 * integrates with the STM32 PMU for power management and scheduled watering.
 *
 * Uses an event-driven state machine to coordinate the wake cycle:
 * INITIALIZING -> REGISTERING -> AWAITING_TIME -> SYNCING_TIME ->
 * CHECKING_UPDATES -> (APPLYING_UPDATE ->)* READY_FOR_SLEEP
 */
class IrrigationMode : public ApplicationMode {
private:
    ValveController valve_controller_;
    PmuClient *pmu_client_;
    bool pmu_available_;
    UpdatePullState update_state_;
    IrrigationStateMachine irrigation_state_;
    bool needs_registration_;  // True if we need to register with hub

    /**
     * @brief Centralized state change handler - drives all side effects
     */
    void onStateChange(IrrigationState state);

    /**
     * @brief Attempt RTC sync from PMU, fall back to hub heartbeat
     */
    void requestTimeSync();

    // PMU callback handlers
    void handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry);
    void handleScheduleComplete();

    // Registration handling
    void attemptDeferredRegistration();

    // Update handling
    void sendCheckUpdates();
    void onUpdateAvailable(const UpdateAvailablePayload *payload) override;

    // Power management
    void signalReadyForSleep();

public:
    using ApplicationMode::ApplicationMode;

protected:
    void onStart() override;
    void onLoop() override;
    void onActuatorCommand(const ActuatorPayload *payload) override;
    void onHeartbeatResponse(const HeartbeatResponsePayload *payload) override;
    void onRebootRequested() override;
    void onFactoryResetRequested() override;
};
