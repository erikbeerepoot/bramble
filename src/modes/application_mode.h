#pragma once
#include <cstdint>
#include <memory>
#include "lora/message.h"
#include "hal/neopixel.h"
#include "led_patterns.h"
#include "periodic_task_manager.h"
#include "hardware/rtc.h"

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
    std::unique_ptr<LEDPattern> led_pattern_;
    std::unique_ptr<LEDPattern> operational_pattern_;  // Pattern to switch to after RTC sync
    PeriodicTaskManager task_manager_;
    bool rtc_synced_ = false;  // True after receiving heartbeat response with time
    
public:
    ApplicationMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                   AddressManager* address_manager = nullptr,
                   HubRouter* hub_router = nullptr,
                   NetworkStats* network_stats = nullptr,
                   bool use_multicore = true)
        : messenger_(messenger), lora_(lora), led_(led),
          address_manager_(address_manager), hub_router_(hub_router),
          network_stats_(network_stats), task_manager_(use_multicore) {}
    
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
    virtual void updateLED(uint32_t current_time) {
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
    virtual void processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time);
    
    /**
     * @brief Called once when the mode starts
     */
    virtual void onStart() {}

    /**
     * @brief Called on each iteration of the main loop
     */
    virtual void onLoop() {}

    /**
     * @brief Handle incoming actuator commands (optional override)
     * @param payload Actuator command payload
     */
    virtual void onActuatorCommand(const ActuatorPayload* payload) {}

    /**
     * @brief Handle incoming update available messages (optional override)
     * @param payload Update available payload
     */
    virtual void onUpdateAvailable(const UpdateAvailablePayload* payload) {}

    /**
     * @brief Handle incoming heartbeat response messages (optional override)
     * Default implementation updates RP2040 RTC
     * @param payload Heartbeat response payload with datetime
     */
    virtual void onHeartbeatResponse(const HeartbeatResponsePayload* payload);

    /**
     * @brief Check if we should use interrupt-based sleep
     * @return true to use sleep, false to continue immediately
     */
    virtual bool shouldSleep() const { return true; }

    /**
     * @brief Check if RTC has been synchronized via heartbeat response
     * @return true if RTC contains valid time from hub
     */
    bool isRtcSynced() const { return rtc_synced_; }

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
};