#pragma once
#include "application_mode.h"

/**
 * @brief Hub mode for network management and routing
 */
class HubMode : public ApplicationMode {
private:
    
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
    void processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) override;
};