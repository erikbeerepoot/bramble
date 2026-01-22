#pragma once

#include "application_mode.h"
#include "../hal/cht832x.h"
#include "../hal/external_flash.h"
#include "../hal/pmu_client.h"
#include "../hal/pmu_reliability.h"
#include "../storage/sensor_flash_buffer.h"
#include "../util/work_tracker.h"
#include <memory>

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
    PmuClient* pmu_client_ = nullptr;
    PMU::ReliablePmuClient* reliable_pmu_ = nullptr;
    bool pmu_available_ = false;
    volatile bool sleep_requested_ = false;   // Deferred sleep signal flag
    WorkTracker work_tracker_;                // Tracks pending work, signals when idle

    // Hub sync timeout tracking - used to proceed with PMU time if hub doesn't respond
    uint32_t heartbeat_request_time_ = 0;
    static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 250;  // Aggressive timeout for low-power operation

    // I2C pin configuration for CHT832X sensor
    static constexpr uint PIN_I2C_SDA = 26;  // GPIO26 (A0)
    static constexpr uint PIN_I2C_SCL = 27;  // GPIO27 (A1)

    // Timing configuration
    static constexpr uint32_t SENSOR_READ_INTERVAL_MS = 30000;   // 30 seconds
    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;     // 60 seconds
    static constexpr uint32_t BACKLOG_TX_INTERVAL_MS = 120000;   // 2 minutes
    static constexpr uint32_t TRANSMIT_INTERVAL_S = 600;         // 10 minutes between transmissions

    /**
     * @brief Read sensor and store data to flash (no immediate TX)
     * @param current_time Current system time in milliseconds
     */
    void readAndStoreSensorData(uint32_t current_time);

    /**
     * @brief Send heartbeat with current status
     * @param current_time Current system time in milliseconds
     */
    void sendHeartbeat(uint32_t current_time);

    /**
     * @brief Check for untransmitted records and batch transmit them
     * @param current_time Current system time in milliseconds
     */
    void checkAndTransmitBacklog(uint32_t current_time);

    /**
     * @brief Transmit a batch of sensor records
     * @param records Array of records to transmit
     * @param count Number of records in batch
     * @return true if transmission successful
     */
    bool transmitBatch(const SensorDataRecord* records, size_t count);

    /**
     * @brief Signal to PMU that RP2040 is ready for sleep
     * Called when there's no more work to do (no backlog to transmit)
     */
    void signalReadyForSleep();

    /**
     * @brief Handle PMU wake notification (periodic/external only)
     * @param reason Wake reason from PMU (Periodic or External)
     *
     * Scheduled wakes are handled by IrrigationMode, not SensorMode.
     */
    void handlePmuWake(PMU::WakeReason reason);

    /**
     * @brief Handle heartbeat response - sync time to PMU
     * @param payload Heartbeat response with timestamp from hub
     */
    void onHeartbeatResponse(const HeartbeatResponsePayload* payload) override;

    /**
     * @brief Handle RTC synchronization completion
     * Centralizes the flow after RTC sync (from PMU or hub heartbeat):
     * - Completes RtcSync work
     * - Triggers backlog check flow
     */
    void onRtcSynced();
};
