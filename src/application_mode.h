#pragma once
#include <cstdint>
#include "lora/message.h"
#include "hal/neopixel.h"

// Forward declarations
class ReliableMessenger;
class SX1276;
class AddressManager;
class HubRouter;
class NetworkStats;

/**
 * @brief Base class for different application modes (demo, production, hub)
 * 
 * This provides a common framework for the main loop, reducing code duplication
 */
class ApplicationMode {
protected:
    ReliableMessenger& messenger_;
    SX1276& lora_;
    NeoPixel& led_;
    AddressManager* address_manager_;
    HubRouter* hub_router_;
    NetworkStats* network_stats_;
    
public:
    ApplicationMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                   AddressManager* address_manager = nullptr,
                   HubRouter* hub_router = nullptr,
                   NetworkStats* network_stats = nullptr)
        : messenger_(messenger), lora_(lora), led_(led),
          address_manager_(address_manager), hub_router_(hub_router),
          network_stats_(network_stats) {}
    
    virtual ~ApplicationMode() = default;
    
    /**
     * @brief Main loop execution
     */
    void run();
    
protected:
    /**
     * @brief Update LED pattern for this mode
     * @param current_time Current system time in milliseconds
     */
    virtual void updateLED(uint32_t current_time) = 0;
    
    /**
     * @brief Handle periodic tasks specific to this mode
     * @param current_time Current system time in milliseconds
     */
    virtual void handlePeriodicTasks(uint32_t current_time) = 0;
    
    /**
     * @brief Process incoming message specific to this mode
     * @param rx_buffer Received message buffer
     * @param rx_len Length of received message
     * @param current_time Current system time
     */
    virtual void processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time);
    
    /**
     * @brief Called once when the mode starts
     */
    virtual void onStart() {}
    
    /**
     * @brief Check if we should use interrupt-based sleep
     * @return true to use sleep, false to continue immediately
     */
    virtual bool shouldSleep() const { return true; }
};