#pragma once
#include "application_mode.h"

/**
 * @brief Hub mode for network management and routing
 */
class HubMode : public ApplicationMode {
private:
    uint32_t last_stats_time_ = 0;
    uint32_t last_maintenance_time_ = 0;
    uint8_t breath_counter_ = 0;
    
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
    void updateLED(uint32_t current_time) override;
    void handlePeriodicTasks(uint32_t current_time) override;
    void processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) override;
};