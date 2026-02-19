#pragma once
#include <map>
#include <set>

#include "hardware/rtc.h"

#include "application_mode.h"

/**
 * @brief Hub mode for network management and routing
 */
class HubMode : public ApplicationMode {
private:
    // Serial command processing
    char serial_input_buffer_[256];
    size_t serial_input_pos_;
    uint32_t last_datetime_sync_ms_;  // Last time we synced with RasPi

    // Track pending batch seq_num per node for BATCH_ACK response
    std::map<uint16_t, uint8_t> pending_batch_seq_num_;

    // Track nodes that need a full system reboot
    std::set<uint16_t> pending_reboots_;

    void processSerialInput();
    void handleSerialCommand(const char *cmd);
    void handleListNodes();
    void handleGetQueue(const char *args);
    void handleSetSchedule(const char *args);
    void handleRemoveSchedule(const char *args);
    void handleSetWakeInterval(const char *args);
    void handleSetDateTime(const char *args);
    void handleDateTimeResponse(const char *args);
    void handleGetDateTime();  // Request datetime from RasPi
    void handleDeleteNode(const char *args);
    void handleRebootNode(const char *args);
    bool parseScheduleArgs(const char *args, uint16_t &node_addr, uint8_t &index, uint8_t &hour,
                           uint8_t &minute, uint16_t &duration, uint8_t &days, uint8_t &valve);

    void syncTimeFromRaspberryPi();  // Initiate time sync

    // Sensor data forwarding to Raspberry Pi
    void handleSensorData(uint16_t source_addr, const SensorPayload *payload);
    void handleSensorDataBatch(uint16_t source_addr, const SensorDataBatchPayload *payload);
    void sendBatchAck(uint16_t dest_addr, uint8_t seq_num, uint8_t status,
                      uint8_t records_received);
    void handleBatchAckResponse(const char *args);  // Parse RasPi BATCH_ACK and forward to node
    void handleEventLogBatch(uint16_t source_addr, const uint8_t *payload, size_t payload_length);

public:
    using ApplicationMode::ApplicationMode;

    /**
     * @brief Handle heartbeat from node and send time response
     * @param source_addr Node address
     * @param payload Heartbeat payload
     * @param rssi RSSI measured by hub when receiving this heartbeat (dBm)
     */
    void handleHeartbeat(uint16_t source_addr, const HeartbeatPayload *payload, int16_t rssi);

protected:
    void onStart() override;
    void onLoop() override;
    void processIncomingMessage(uint8_t *rx_buffer, int rx_len, uint32_t current_time) override;
};