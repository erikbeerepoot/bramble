#pragma once

#include "../hal/pmu_client.h"
#include "../hal/pmu_reliability.h"
#include "../hal/valve_controller.h"
#include "../util/irrigation_state_machine.h"
#include "../util/task_queue.h"
#include "application_mode.h"

/**
 * @brief Persisted state stored in PMU RAM across sleep cycles
 *
 * This struct is packed into a 32-byte opaque blob and sent to the PMU
 * with ReadyForSleep. The PMU stores it in RAM (always powered from battery)
 * and returns it in the WakeReason notification.
 *
 * On cold start (battery disconnect), state_valid=false.
 */
struct __attribute__((packed)) IrrigationPersistedState {
    uint8_t version;                 // Format version
    uint8_t board_version;           // Board hardware version (3=V3, 4=V4)
    uint8_t next_seq_num;            // LoRa sequence number
    uint8_t update_sequence;         // Current update pull sequence number
    uint16_t assigned_address;       // Node address (survives warm reboot)
    uint8_t valve_states[4];         // ValveState per valve (CLOSED=0, OPEN=1, UNKNOWN=2)
    uint8_t pending_valve_close;     // 0=no pending close, 1=pending valve close timer
    uint8_t pending_close_valve_id;  // Which valve to close when timer fires
    uint8_t padding[20];             // Reserved (pad to 32 bytes)
};
static_assert(sizeof(IrrigationPersistedState) == 32, "IrrigationPersistedState must be 32 bytes");

constexpr uint8_t IRRIGATION_STATE_VERSION = 5;

/**
 * @brief Irrigation node mode for valve control
 *
 * Irrigation node that handles valve commands from the hub and
 * integrates with the STM32 PMU for power management and scheduled watering.
 *
 * Uses an event-driven state machine to coordinate the wake cycle:
 * INITIALIZING -> REGISTERING -> AWAITING_TIME -> SENDING_HEARTBEAT ->
 * CHECKING_UPDATES -> (APPLYING_UPDATE ->)* READY_FOR_SLEEP
 */
class IrrigationMode : public ApplicationMode {
public:
    using AddressSavedCallback = std::function<void(uint16_t)>;

    void setAddressSavedCallback(AddressSavedCallback callback)
    {
        address_saved_callback_ = callback;
    }

private:
    ValveController valve_controller_;
    PmuClient *pmu_client_ = nullptr;
    PMU::ReliablePmuClient *reliable_pmu_ = nullptr;
    bool pmu_available_ = false;
    bool sleep_pending_ = false;
    IrrigationStateMachine irrigation_state_;
    TaskQueue task_queue_;
    bool needs_registration_;  // True if we need to register with hub
    uint64_t device_id_;       // Unique board ID for heartbeat identification
    uint16_t wake_timeout_id_ = 0;
    uint16_t keepawake_task_id_ = 0;
    uint16_t drain_task_id_ = 0;
    uint32_t drain_start_ms_ = 0;
    bool pending_valve_close_ = false;
    uint8_t pending_close_valve_id_ = 0;
    uint16_t valve_duration_seconds_ = 0;
    AddressSavedCallback address_saved_callback_;

    /**
     * @brief Centralized state change handler - drives all side effects
     */
    void onStateChange(IrrigationState state);

    void scheduleKeepAwake();

    // PMU callback handlers
    void handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry, bool state_valid,
                       const uint8_t *state);
    void handleScheduleComplete();

    // Registration handling
    void attemptDeferredRegistration();

    // Update handling (base class handles SET_WAKE_INTERVAL, SET_DATETIME)
    void onModeSpecificUpdate(const UpdateAvailablePayload *payload, uint8_t hub_sequence) override;
    void onUpdateApplied(uint8_t hub_sequence) override;
    void onUpdateFailed() override;
    void onUpdateReceived(bool has_updates) override;

    // Power management
    void signalReadyForSleep();

    // State persistence
    void packState(IrrigationPersistedState &out) const;
    bool unpackState(const IrrigationPersistedState *persisted);

public:
    using ApplicationMode::ApplicationMode;
    ~IrrigationMode();

protected:
    void onStart() override;
    void onLoop() override;
    void onActuatorCommand(const ActuatorPayload *payload) override;
    void onHeartbeatResponse(const HeartbeatResponsePayload *payload) override;
    void onReregistrationRequested() override;
    void onRebootRequested() override;
    void onFactoryResetRequested() override;
};
