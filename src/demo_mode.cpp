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
}

void DemoMode::updateLED(uint32_t current_time) {
    if (hub_router_) {
        // Hub: Blue breathing pattern
        static uint8_t breath_counter = 0;
        uint8_t brightness = (breath_counter < 64) ? breath_counter * 2 : (128 - breath_counter) * 2;
        led_.setPixel(0, 0, 0, brightness);  // Blue breathing
        led_.show();
        breath_counter = (breath_counter + 1) % 128;
    } else {
        // Node: Green heartbeat
        uint8_t brightness = (led_counter_ < 10) ? led_counter_ * 5 : (20 - led_counter_) * 5;
        led_.setPixel(0, 0, brightness, 0);  // Green heartbeat
        led_.show();
        led_counter_ = (led_counter_ + 1) % 20;
    }
}

void DemoMode::handlePeriodicTasks(uint32_t current_time) {
    // Send test messages every 15 seconds
    if (current_time - last_demo_time_ >= DEMO_INTERVAL_MS) {
        printf("--- DEMO: Sending test messages ---\n");
        
        // Test temperature reading (best effort)
        uint8_t temp_data[] = {0x12, 0x34};
        messenger_.sendSensorData(HUB_ADDRESS, SENSOR_TEMPERATURE, 
                               temp_data, sizeof(temp_data), BEST_EFFORT);
        
        // Test moisture reading (reliable - critical for irrigation decisions)
        uint8_t moisture_data[] = {0x45, 0x67};
        messenger_.sendSensorData(HUB_ADDRESS, SENSOR_SOIL_MOISTURE, 
                               moisture_data, sizeof(moisture_data), RELIABLE);
        
        last_demo_time_ = current_time;
    }
    
    // Send heartbeat every minute
    if (current_time - last_heartbeat_time_ >= HEARTBEAT_INTERVAL_MS) {
        printf("--- DEMO: Sending heartbeat ---\n");
        
        // Calculate node status
        uint32_t uptime = current_time / 1000;  // Convert to seconds
        uint8_t battery_level = 255;  // External power for demo
        uint8_t signal_strength = 70;  // Simulated signal strength
        uint8_t active_sensors = CAP_TEMPERATURE | CAP_SOIL_MOISTURE;
        uint8_t error_flags = 0;  // No errors
        
        messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, 
                              signal_strength, active_sensors, error_flags);
        
        last_heartbeat_time_ = current_time;
    }
}