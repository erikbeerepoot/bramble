#include "sensor_mode.h"
#include "../hal/cht832x.h"
#include "../hal/logger.h"
#include "../lora/reliable_messenger.h"
#include "../lora/message.h"
#include "../led_patterns.h"
#include "hardware/i2c.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;

static Logger logger("SENSOR");

SensorMode::~SensorMode() = default;

void SensorMode::onStart() {
    logger.info("=== SENSOR MODE ACTIVE ===");
    logger.info("- Temperature/humidity data logger");
    logger.info("- 30 second reading interval");
    logger.info("- Cyan LED breathing pattern");

    // Initialize CHT832X temperature/humidity sensor
    sensor_ = std::make_unique<CHT832X>(i2c1, PIN_I2C_SDA, PIN_I2C_SCL);

    if (sensor_->init()) {
        logger.info("CHT832X sensor initialized on I2C1 (SDA=%d, SCL=%d)",
                   PIN_I2C_SDA, PIN_I2C_SCL);

        // Take an initial reading to verify sensor is working
        auto reading = sensor_->read();
        if (reading.valid) {
            logger.info("Initial reading: %.2fC, %.2f%%RH",
                       reading.temperature, reading.humidity);
        }
    } else {
        logger.error("Failed to initialize CHT832X sensor!");
        logger.error("Check wiring: Red=3.3V, Black=GND, Green=GPIO%d, Yellow=GPIO%d",
                    PIN_I2C_SCL, PIN_I2C_SDA);
    }

    // Cyan breathing pattern for sensor nodes
    led_pattern_ = std::make_unique<BreathingPattern>(led_, 0, 255, 255);

    // Add periodic sensor reading task
    task_manager_.addTask(
        [this](uint32_t time) {
            readAndTransmitSensorData(time);
        },
        SENSOR_READ_INTERVAL_MS,
        "Sensor Read"
    );

    // Add heartbeat task
    task_manager_.addTask(
        [this](uint32_t time) {
            sendHeartbeat(time);
        },
        HEARTBEAT_INTERVAL_MS,
        "Heartbeat"
    );
}

void SensorMode::onLoop() {
    // Nothing special needed in the main loop
    // Sensor reading is handled by periodic task
}

void SensorMode::readAndTransmitSensorData(uint32_t current_time) {
    if (!sensor_) {
        logger.error("Sensor not initialized");
        return;
    }

    auto reading = sensor_->read();

    if (!reading.valid) {
        logger.error("Failed to read sensor");
        return;
    }

    logger.info("Sensor reading: %.2fC, %.2f%%RH", reading.temperature, reading.humidity);

    // Convert to fixed-point format for transmission
    // Temperature: int16_t in 0.01C units (e.g., 2350 = 23.50C)
    // Humidity: uint16_t in 0.01% units (e.g., 6500 = 65.00%)
    int16_t temp_fixed = static_cast<int16_t>(reading.temperature * 100.0f);
    uint16_t hum_fixed = static_cast<uint16_t>(reading.humidity * 100.0f);

    // Send temperature (2 bytes, little-endian)
    uint8_t temp_data[2] = {
        static_cast<uint8_t>(temp_fixed & 0xFF),
        static_cast<uint8_t>((temp_fixed >> 8) & 0xFF)
    };
    messenger_.sendSensorData(HUB_ADDRESS, SENSOR_TEMPERATURE,
                             temp_data, sizeof(temp_data), BEST_EFFORT);

    // Send humidity (2 bytes, little-endian)
    uint8_t hum_data[2] = {
        static_cast<uint8_t>(hum_fixed & 0xFF),
        static_cast<uint8_t>((hum_fixed >> 8) & 0xFF)
    };
    messenger_.sendSensorData(HUB_ADDRESS, SENSOR_HUMIDITY,
                             hum_data, sizeof(hum_data), BEST_EFFORT);

    logger.debug("Transmitted: temp=%d (0.01C), hum=%d (0.01%%)",
                temp_fixed, hum_fixed);
}

void SensorMode::sendHeartbeat(uint32_t current_time) {
    uint32_t uptime = current_time / 1000;  // Convert to seconds
    uint8_t battery_level = 255;            // External power (no battery monitoring yet)
    uint8_t signal_strength = 70;           // TODO: Get actual RSSI from LoRa
    uint8_t active_sensors = CAP_TEMPERATURE | CAP_HUMIDITY;
    uint8_t error_flags = 0;                // No errors

    logger.debug("Sending heartbeat (uptime=%lu s)", uptime);

    messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level,
                            signal_strength, active_sensors, error_flags);
}
