#pragma once

#include <memory>

#include "../hal/cht832x.h"
#include "../hal/external_flash.h"
#include "../lora/batch_transmitter.h"
#include "../lora/heartbeat_client.h"
#include "../storage/sensor_flash_buffer.h"
#include "../util/sensor_pmu_manager.h"
#include "../util/sensor_state_machine.h"
#include "../util/task_queue.h"
#include "application_mode.h"

/**
 * @brief Data logger sensor mode for temperature/humidity monitoring
 *
 * Reads temperature and humidity from CHT832X sensor (DFRobot SEN0546)
 * every 30 seconds and transmits via LoRa to the hub.
 */
class SensorMode : public ApplicationMode {
public:
    using ApplicationMode::ApplicationMode;

    ~SensorMode();

protected:
    void onStart() override;
    void onLoop() override;

private:
    std::unique_ptr<CHT832X> sensor_;
    std::unique_ptr<ExternalFlash> external_flash_;
    std::unique_ptr<SensorFlashBuffer> flash_buffer_;
    std::unique_ptr<SensorPmuManager> pmu_manager_;
    std::unique_ptr<HeartbeatClient> heartbeat_client_;
    TaskQueue task_queue_;  // Unified task coordination

    // Timeout task handles — cancelled when expected response arrives, fires if it doesn't
    uint16_t registration_timeout_id_ = 0;
    uint16_t syncing_time_timeout_id_ = 0;

    // Timeout durations
    static constexpr uint32_t REGISTRATION_TIMEOUT_MS = 5000;  // Allow time for RELIABLE retries
    static constexpr uint32_t SYNCING_TIME_TIMEOUT_MS = 5000;  // Class A receive window timeout
    static constexpr uint32_t LISTEN_WINDOW_MS = 500;

    // I2C pin configuration for CHT832X sensor
    static constexpr uint PIN_I2C_SDA = 26;  // GPIO26 (A0)
    static constexpr uint PIN_I2C_SCL = 27;  // GPIO27 (A1)

    // Timing configuration
    static constexpr uint32_t SENSOR_READ_INTERVAL_MS = 30000;
    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;
    static constexpr uint32_t BACKLOG_TX_INTERVAL_MS = 120000;
    static constexpr uint32_t TRANSMIT_INTERVAL_S = 600;

    /**
     * @brief Read sensor and store data to flash (no immediate TX)
     * @param current_time Current system time in milliseconds
     */
    void readAndStoreSensorData(uint32_t current_time);

    /**
     * @brief Collect current heartbeat status for the HeartbeatClient
     * @return HeartbeatStatus with uptime, battery, signal, sensors, errors, pending records
     */
    HeartbeatStatus collectHeartbeatStatus();

    /**
     * @brief Check if transmission is needed
     * @return true if transmission is needed, false if ready for sleep
     *
     * Pure function: only checks conditions, does not transmit.
     * Handles both flash backlog and direct transmit fallback cases.
     * Caller should use result to report to state machine.
     */
    bool checkNeedsTransmission();

    /**
     * @brief Transmit backlog to hub
     *
     * Reads untransmitted records and sends batch.
     * Reports reportTransmitComplete() when done (via callback).
     */
    void transmitBacklog();

    /**
     * @brief Direct transmit current reading (fallback when flash unavailable)
     *
     * Used when external flash fails - transmits the in-memory reading directly
     * without flash storage. Reports reportTransmitComplete() when done.
     */
    void transmitCurrentReading();

    /**
     * @brief Initialize flash timestamps on first boot
     * Sets initial_boot_timestamp and last_sync_timestamp if not set.
     */
    void initializeFlashTimestamps();

    /**
     * @brief Handle heartbeat response - sync time to PMU
     * @param payload Heartbeat response with timestamp from hub
     */
    void onHeartbeatResponse(const HeartbeatResponsePayload *payload) override;
    void onRebootRequested() override;

    /**
     * @brief Check if enough time has elapsed since last transmission
     * @param current_timestamp Unix timestamp to use for comparison (0 = use RTC)
     * @return true if it's time to transmit backlog, false otherwise
     *
     * Used to avoid unnecessary LoRa heartbeat sync when PMU has valid time
     * but we're between transmission intervals.
     */
    bool isTimeToTransmit(uint32_t current_timestamp = 0) const;

    /**
     * @brief Collect current error flags based on hardware status
     * @return Bitmask of ERR_FLAG_* constants from message.h
     */
    uint16_t collectErrorFlags();

    /**
     * @brief Get current battery level
     * @return Battery percentage (0-100), or 255 for external power
     */
    uint8_t getBatteryLevel();

    // Error tracking state
    bool last_sensor_read_valid_ = false;               // Track if last sensor read succeeded
    static constexpr uint8_t TX_FAILURE_THRESHOLD = 3;  // Failures before setting error flag

    // Batch transmission
    std::unique_ptr<BatchTransmitter> transmitter_;

    // Fallback storage for direct transmit when flash unavailable
    SensorDataRecord current_reading_ = {};

    // Event-driven state machine - reports events, queries state
    // No external flags needed - state machine tracks everything internally
    SensorStateMachine sensor_state_;

    /**
     * @brief Handle state machine state changes
     *
     * Centralizes all reactions to state changes:
     * - LED pattern updates
     * - Error handling
     * - Future: notifications, logging, etc.
     *
     * @param state New state after transition
     */
    void onStateChange(SensorState state);

    /**
     * @brief Try to initialize sensor with power-on delay
     * @return true if sensor initialized successfully
     */
    bool tryInitSensor();

    /**
     * @brief Request time sync from PMU or hub
     * Tries PMU's battery-backed RTC first. If PMU unavailable or has no valid time,
     * falls back to sending heartbeat to sync from hub.
     */
    void requestTimeSync();

    /**
     * @brief Send registration request to hub
     * Called from REGISTERING state callback when node has no assigned address.
     * On success, registration response callback transitions to AWAITING_TIME.
     * On failure to send, transitions to READY_FOR_SLEEP to retry next cycle.
     */
    void attemptRegistration();
};
