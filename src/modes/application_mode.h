#pragma once
#include <cstdint>
#include <functional>
#include <memory>

#include "hal/neopixel.h"
#include "hal/rtc_compat.h"
#include "led_patterns.h"
#include "lora/event_log_transmitter.h"
#include "lora/message.h"
#include "periodic_task_manager.h"
#include "util/base_state_machine.h"
#include "util/event_log.h"

// Forward declarations
class ReliableMessenger;
class RadioInterface;
class AddressManager;
class HubRouter;
class NetworkStats;

namespace PMU {
class ReliablePmuClient;
}

/**
 * @brief Update pull state tracking
 *
 * Tracks sequence numbers and message state for the CHECK_UPDATES /
 * UPDATE_AVAILABLE protocol between nodes and hub.
 */
struct UpdatePullState {
    uint8_t current_sequence;   // Node's current sequence number
    uint8_t pending_check_seq;  // Sequence number of pending CHECK_UPDATES message

    UpdatePullState() : current_sequence(0), pending_check_seq(0) {}

    void reset() { pending_check_seq = 0; }
};

/**
 * @brief Base class for different application modes (demo, production, hub)
 *
 * This provides a common framework for the main loop, reducing code duplication
 */
class ApplicationMode {
public:
    static constexpr uint16_t KEEP_AWAKE_PROCESSING_SECONDS = 10;

    // Callback type for reregistration required (hub doesn't recognize node)
    using ReregistrationCallback = std::function<void()>;

    ApplicationMode(ReliableMessenger &messenger, RadioInterface &lora, NeoPixel &led,
                    AddressManager *address_manager = nullptr, HubRouter *hub_router = nullptr,
                    NetworkStats *network_stats = nullptr)
        : messenger_(messenger), lora_(lora), led_(led), address_manager_(address_manager),
          hub_router_(hub_router), network_stats_(network_stats)
    {
    }

    virtual ~ApplicationMode() = default;

    /**
     * @brief Set callback for reregistration required
     * @param callback Function to call when hub requests re-registration via heartbeat flag
     */
    void setReregistrationCallback(ReregistrationCallback callback)
    {
        reregistration_callback_ = callback;
    }

    /**
     * @brief Main loop execution
     */
    void run();

protected:
    ReliableMessenger &messenger_;
    RadioInterface &lora_;
    NeoPixel &led_;
    AddressManager *address_manager_;
    HubRouter *hub_router_;
    NetworkStats *network_stats_;
    PMU::ReliablePmuClient *reliable_pmu_ = nullptr;
    EventLog<64> event_log_;
    std::unique_ptr<EventLogTransmitter> event_log_transmitter_;
    UpdatePullState update_state_;
    std::unique_ptr<LEDPattern> led_pattern_;
    std::unique_ptr<LEDPattern> operational_pattern_;  // Pattern to switch to after RTC sync
    PeriodicTaskManager task_manager_;
    BaseStateMachine state_machine_;  // Centralized state management
    ReregistrationCallback reregistration_callback_;

    /**
     * @brief Update LED pattern for this mode
     * @param current_time Current system time in milliseconds
     */
    virtual void updateLED(uint32_t current_time)
    {
        if (led_pattern_) {
            led_pattern_->update(current_time);
        }
    }

    /**
     * @brief Process incoming message specific to this mode
     * @param rx_buffer Received message buffer
     * @param rx_len Length of received message
     * @param current_time Current system time
     */
    virtual void processIncomingMessage(uint8_t *rx_buffer, int rx_len, uint32_t current_time);

    /**
     * @brief Called once when the mode starts
     */
    virtual void onStart() {}

    /**
     * @brief Whether this mode defers the non-watchdog boot event to its own logic.
     *
     * Default: false — ApplicationMode::run() records BOOT_COLD for non-watchdog
     * boots. PMU-using modes override to return true and record WAKE (warm boot
     * from PMU sleep) or BOOT_COLD (no valid PMU state) themselves once state
     * restore completes.
     */
    virtual bool defersBootEvent() const { return false; }

    /**
     * @brief Called on each iteration of the main loop
     */
    virtual void onLoop() {}

    /**
     * @brief Handle incoming actuator commands (optional override)
     * @param payload Actuator command payload
     */
    virtual void onActuatorCommand(const ActuatorPayload *payload) {}

    /**
     * @brief Handle incoming update available messages
     *
     * Base implementation handles the update pull protocol (sequence tracking,
     * dedup, cancel pending CHECK_UPDATES) and dispatches generic update types
     * (SET_WAKE_INTERVAL, SET_DATETIME) via the PMU client. Mode-specific
     * update types are forwarded to onModeSpecificUpdate().
     *
     * @param payload Update available payload
     */
    virtual void onUpdateAvailable(const UpdateAvailablePayload *payload);

    void handleSetWakeInterval(const UpdateAvailablePayload *payload, uint8_t hub_sequence);
    void handleSetDatetime(const UpdateAvailablePayload *payload, uint8_t hub_sequence);

    /**
     * @brief Handle mode-specific update types
     *
     * Called by the base onUpdateAvailable() for update types not handled
     * generically (e.g. SET_SCHEDULE, REMOVE_SCHEDULE, ACTUATOR_COMMAND).
     * Default implementation logs an error and calls onUpdateFailed().
     *
     * @param payload Full update payload
     * @param hub_sequence Sequence number from hub for this update
     */
    virtual void onModeSpecificUpdate(const UpdateAvailablePayload *payload, uint8_t hub_sequence);

    /**
     * @brief Called when an update is successfully applied
     *
     * Base implementation advances the update sequence number.
     * Modes should override to also report to their state machines.
     *
     * @param hub_sequence The sequence number of the applied update
     */
    virtual void onUpdateApplied(uint8_t hub_sequence);

    /**
     * @brief Called when an update fails to apply
     *
     * Base implementation resets the pending check state.
     * Modes should override to also report to their state machines.
     */
    virtual void onUpdateFailed();

    /**
     * @brief Called when an UPDATE_AVAILABLE message is received
     *
     * Notifies the mode whether there are updates to apply.
     * Modes should override to report to their state machines.
     *
     * @param has_updates true if an update follows, false if no more updates
     */
    virtual void onUpdateReceived(bool has_updates) {}

    /**
     * @brief Handle incoming heartbeat response messages (optional override)
     * Default implementation updates RP2040 RTC
     * @param payload Heartbeat response payload with datetime
     */
    virtual void onHeartbeatResponse(const HeartbeatResponsePayload *payload);

    /**
     * @brief Handle reboot request from hub via PENDING_FLAG_REBOOT
     *
     * Default implementation performs RP2040-only watchdog reboot.
     * Modes with PMU access should override to send SystemReset to PMU
     * for a full system reset (STM32 + RP2040).
     */
    virtual void onRebootRequested();

    /**
     * @brief Handle factory reset request from hub via PENDING_FLAG_FACTORY_RESET
     *
     * Default implementation performs RP2040-only watchdog reboot (no FRAM wipe).
     * Modes with PMU access should override to send FactoryReset to PMU
     * for a full FRAM wipe + system reset.
     */
    virtual void onFactoryResetRequested();

    /**
     * @brief Handle re-registration request from hub
     *
     * Default implementation fires the reregistration_callback_ set via
     * setReregistrationCallback(). Modes with their own state machines
     * (e.g. IrrigationMode) can override to handle re-registration internally.
     */
    virtual void onReregistrationRequested();

    /**
     * @brief Set the PMU client for generic update handling
     *
     * Call from onStart() after creating the PMU client. Enables
     * base class handling of SET_WAKE_INTERVAL and SET_DATETIME updates.
     *
     * @param pmu Pointer to reliable PMU client (nullptr to disable)
     */
    void setReliablePmu(PMU::ReliablePmuClient *pmu) { reliable_pmu_ = pmu; }

    /**
     * @brief Send CHECK_UPDATES message to hub
     *
     * Sends a CHECK_UPDATES message with the current update sequence number.
     * Used by the update pull protocol to request pending updates from the hub.
     */
    void sendCheckUpdates();

    /**
     * @brief Initialize the event log transmitter
     *
     * Call from onStart() after messenger is ready. Creates the transmitter
     * that sends event log batches to the hub.
     */
    void initEventLogTransmitter();

    /**
     * @brief Lifecycle hook called before the node signals ready-for-sleep
     *
     * Default implementation transmits pending event log records.
     * Modes override to add mode-specific cleanup (e.g. flash persistence
     * for sensor node). Sensor mode should gate on radio activity to avoid
     * transmitting on non-radio wake cycles.
     */
    virtual void onBeforeSleep();

    /**
     * @brief Check if we should use interrupt-based sleep
     * @return true to use sleep, false to continue immediately
     */
    virtual bool shouldSleep() const { return true; }

    /**
     * @brief Get current Unix timestamp from RTC
     * @return Unix timestamp (seconds since 1970-01-01), or 0 if RTC not synced
     */
    uint32_t getUnixTimestamp() const;

    /**
     * @brief Switch from initialization pattern to operational pattern
     *
     * Called automatically after RTC sync. Modes should set operational_pattern_
     * in onStart() to define the pattern used after initialization completes.
     */
    void switchToOperationalPattern();

    /**
     * @brief Update state machine based on current hardware state
     *
     * Reads hardware state (rtc_running) and updates the state machine.
     * Call this after any hardware state changes (RTC set, etc).
     */
    void updateStateMachine();
};