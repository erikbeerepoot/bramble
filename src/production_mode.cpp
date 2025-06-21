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
}

void ProductionMode::updateLED(uint32_t current_time) {
    // Production LED: Subtle green heartbeat
    uint8_t brightness = (led_counter_ < 10) ? led_counter_ * 5 : (20 - led_counter_) * 5;
    led_.setPixel(0, 0, brightness, 0);  // Green heartbeat
    led_.show();
    led_counter_ = (led_counter_ + 1) % 20;
}

void ProductionMode::handlePeriodicTasks(uint32_t current_time) {
    // Send sensor data periodically
    if (current_time - last_sensor_time_ >= SENSOR_INTERVAL_MS) {
        // TODO: Replace with actual sensor readings
        // Example: Read temperature, humidity, soil moisture, battery level
        printf("Sensor reading cycle\n");
        last_sensor_time_ = current_time;
    }
    
    // Send heartbeat to hub
    if (current_time - last_heartbeat_time_ >= HEARTBEAT_INTERVAL_MS) {
        printf("Heartbeat\n");
        
        // Calculate real node status
        uint32_t uptime = current_time / 1000;  // Convert to seconds
        uint8_t battery_level = 85;  // Example battery level
        uint8_t signal_strength = 65;  // Example signal strength
        uint8_t active_sensors = CAP_TEMPERATURE | CAP_HUMIDITY | CAP_SOIL_MOISTURE;
        uint8_t error_flags = 0;  // No errors in production
        
        messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, 
                              signal_strength, active_sensors, error_flags);
        
        last_heartbeat_time_ = current_time;
    }
}