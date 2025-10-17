#include "application_mode.h"
#include "pico/stdlib.h"
#include "lora/sx1276.h"
#include "lora/reliable_messenger.h"
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"

// Forward declaration of common message processing function
extern void processIncomingMessage(uint8_t* rx_buffer, int rx_len, ReliableMessenger& messenger,
                                  AddressManager* address_manager, HubRouter* hub_router, 
                                  uint32_t current_time, NetworkStats* network_stats, SX1276* lora);

// Forward declaration of sleep function
extern void sleepUntilInterrupt();

void ApplicationMode::run() {
    // Set up actuator command callback
    messenger_.setActuatorCallback([this](const ActuatorPayload* payload) {
        onActuatorCommand(payload);
    });

    // Set up update available callback
    messenger_.setUpdateCallback([this](const UpdateAvailablePayload* payload) {
        onUpdateAvailable(payload);
    });

    // Call startup hook
    onStart();
    
    // If using multicore, start the task manager on Core 1
    task_manager_.start();
    
    // Add LED update task if pattern exists
    if (led_pattern_) {
        task_manager_.addTask(
            [this](uint32_t time) { updateLED(time); },
            10,  // Update every 10ms for smooth animation
            "LED Update"
        );
    }
    
    // Main loop
    while (true) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // Call mode-specific loop hook
        onLoop();

        // Update tasks (only if not using multicore)
        task_manager_.update(current_time);

        // Check for interrupts first (more efficient than polling)
        if (lora_.isInterruptPending()) {
            lora_.handleInterrupt();
        }
        
        // Check for incoming messages
        if (lora_.isMessageReady()) {
            uint8_t rx_buffer[MESSAGE_MAX_SIZE];
            int rx_len = lora_.receive(rx_buffer, sizeof(rx_buffer));
            
            if (rx_len > 0) {
                processIncomingMessage(rx_buffer, rx_len, current_time);
            } else if (rx_len < 0) {
                lora_.startReceive();
            }
        }
        
        // Update retry timers for reliable message delivery
        messenger_.update();
        
        // Update hub router if in hub mode
        if (hub_router_) {
            hub_router_->processQueuedMessages();
        }
        
        // Sleep efficiently between iterations
        if (shouldSleep() && !lora_.isInterruptPending()) {
            sleepUntilInterrupt();
        }
    }
}

void ApplicationMode::processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) {
    // Use the common message processing function
    ::processIncomingMessage(rx_buffer, rx_len, messenger_, address_manager_, 
                           hub_router_, current_time, network_stats_, &lora_);
}