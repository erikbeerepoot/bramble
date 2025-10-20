#pragma once
#include "application_mode.h"
#include "hardware/rtc.h"

/**
 * @brief Hub mode for network management and routing
 */
class HubMode : public ApplicationMode {
private:
    // Serial command processing
    char serial_input_buffer_[256];
    size_t serial_input_pos_;
    uint32_t last_datetime_sync_ms_;  // Last time we synced with RasPi

    void processSerialInput();
    void handleSerialCommand(const char* cmd);
    void handleListNodes();
    void handleGetQueue(const char* args);
    void handleSetSchedule(const char* args);
    void handleRemoveSchedule(const char* args);
    void handleSetWakeInterval(const char* args);
    void handleSetDateTime(const char* args);
    void handleDateTimeResponse(const char* args);
    void handleGetDateTime();  // Request datetime from RasPi
    bool parseScheduleArgs(const char* args, uint16_t& node_addr, uint8_t& index,
                          uint8_t& hour, uint8_t& minute, uint16_t& duration,
                          uint8_t& days, uint8_t& valve);

    void syncTimeFromRaspberryPi();  // Initiate time sync

public:
    using ApplicationMode::ApplicationMode;

    /**
     * @brief Handle heartbeat from node and send time response
     * @param source_addr Node address
     * @param payload Heartbeat payload
     */
    void handleHeartbeat(uint16_t source_addr, const HeartbeatPayload* payload);

protected:
    void onStart() override;
    void onLoop() override;
    void processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) override;
};