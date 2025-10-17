#pragma once
#include "application_mode.h"

/**
 * @brief Hub mode for network management and routing
 */
class HubMode : public ApplicationMode {
private:
    // Serial command processing
    char serial_input_buffer_[256];
    size_t serial_input_pos_;
    uint32_t last_datetime_query_ms_;

    void processSerialInput();
    void handleSerialCommand(const char* cmd);
    void handleListNodes();
    void handleGetQueue(const char* args);
    void handleSetSchedule(const char* args);
    void handleRemoveSchedule(const char* args);
    void handleSetWakeInterval(const char* args);
    void handleDateTimeResponse(const char* args);
    bool parseScheduleArgs(const char* args, uint16_t& node_addr, uint8_t& index,
                          uint8_t& hour, uint8_t& minute, uint16_t& duration,
                          uint8_t& days, uint8_t& valve);

public:
    using ApplicationMode::ApplicationMode;

protected:
    void onStart() override;
    void onLoop() override;
    void processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) override;
};