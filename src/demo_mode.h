#pragma once
#include "application_mode.h"

/**
 * @brief Demo mode for testing and development
 */
class DemoMode : public ApplicationMode {
private:
    uint32_t last_demo_time_ = 0;
    uint32_t last_heartbeat_time_ = 0;
    uint8_t led_counter_ = 0;
    
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
    void updateLED(uint32_t current_time) override;
    void handlePeriodicTasks(uint32_t current_time) override;
};