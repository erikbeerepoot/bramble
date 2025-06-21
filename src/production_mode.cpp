#include "production_mode.h"
#include <cstdio>
#include "lora/reliable_messenger.h"
#include "lora/message.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t SENSOR_INTERVAL_MS = 30000;      // 30 seconds
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;   // 60 seconds

void ProductionMode::onStart() {
    printf("=== PRODUCTION MODE ACTIVE ===\n");
    printf("- Green LED heartbeat\n");
    printf("- Sensor readings every %d seconds\n", SENSOR_INTERVAL_MS / 1000);
    printf("- Minimal power consumption\n");
    
    // Production nodes use green heartbeat pattern
    led_pattern_ = std::make_unique<HeartbeatPattern>(led_, 0, 255, 0);
    
    // Add sensor reading task
    task_manager_.addTask(
        [this](uint32_t time) {
            // TODO: Replace with actual sensor readings
            // Example: Read temperature, humidity, soil moisture, battery level
            printf("Sensor reading cycle\n");
        },
        SENSOR_INTERVAL_MS,
        "Sensor Reading"
    );
    
    // Add heartbeat task
    task_manager_.addTask(
        [this](uint32_t time) {
            printf("Heartbeat\n");
            
            // Calculate real node status
            uint32_t uptime = time / 1000;  // Convert to seconds
            uint8_t battery_level = 85;  // Example battery level
            uint8_t signal_strength = 65;  // Example signal strength
            uint8_t active_sensors = CAP_TEMPERATURE | CAP_HUMIDITY | CAP_SOIL_MOISTURE;
            uint8_t error_flags = 0;  // No errors in production
            
            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, 
                                  signal_strength, active_sensors, error_flags);
        },
        HEARTBEAT_INTERVAL_MS,
        "Heartbeat"
    );
}