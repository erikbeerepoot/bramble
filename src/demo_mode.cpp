#include "demo_mode.h"
#include <cstdio>
#include "lora/reliable_messenger.h"
#include "lora/message.h"
#include "lora/hub_router.h"

// Constants from main file
constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t DEMO_INTERVAL_MS = 15000;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;

void DemoMode::onStart() {
    printf("=== DEMO MODE ACTIVE ===\n");
    printf("- Test messages every 15 seconds\n");
    printf("- Verbose debug output\n");
    
    // Set up LED pattern based on role
    if (hub_router_) {
        // Hub: Blue breathing
        led_pattern_ = std::make_unique<BreathingPattern>(led_, 0, 0, 255);
    } else {
        // Node: Green heartbeat
        led_pattern_ = std::make_unique<HeartbeatPattern>(led_, 0, 255, 0);
    }
    
    // Add periodic tasks to task manager
    task_manager_.addTask(
        [this](uint32_t time) {
            printf("--- DEMO: Sending test messages ---\n");
            
            // Test temperature reading (best effort)
            uint8_t temp_data[] = {0x12, 0x34};
            messenger_.sendSensorData(HUB_ADDRESS, SENSOR_TEMPERATURE, 
                                   temp_data, sizeof(temp_data), BEST_EFFORT);
            
            // Test moisture reading (reliable - critical for irrigation decisions)
            uint8_t moisture_data[] = {0x45, 0x67};
            messenger_.sendSensorData(HUB_ADDRESS, SENSOR_SOIL_MOISTURE, 
                                   moisture_data, sizeof(moisture_data), RELIABLE);
        },
        DEMO_INTERVAL_MS,
        "Demo Messages"
    );
    
    task_manager_.addTask(
        [this](uint32_t time) {
            printf("--- DEMO: Sending heartbeat ---\n");
            
            // Calculate node status
            uint32_t uptime = time / 1000;  // Convert to seconds
            uint8_t battery_level = 255;  // External power for demo
            uint8_t signal_strength = 70;  // Simulated signal strength
            uint8_t active_sensors = CAP_TEMPERATURE | CAP_SOIL_MOISTURE;
            uint8_t error_flags = 0;  // No errors
            
            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, 
                                  signal_strength, active_sensors, error_flags);
        },
        HEARTBEAT_INTERVAL_MS,
        "Heartbeat"
    );
}