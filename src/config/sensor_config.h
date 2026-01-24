#pragma once

#include <stdint.h>

#include "hal/flash.h"

/**
 * @brief Configuration parameter IDs for sensor nodes
 *
 * These IDs are used in SET_CONFIG messages to identify which
 * parameter to update. Values must remain stable across firmware versions.
 */
enum class ConfigParamId : uint8_t {
    SENSOR_READ_INTERVAL_S = 0x01,  // How often to sample sensors (seconds)
    TRANSMIT_INTERVAL_S = 0x02,     // Minimum interval between transmissions (seconds)
    TEMPERATURE_OFFSET = 0x03,      // Calibration offset (0.01°C units, signed)
    HUMIDITY_OFFSET = 0x04,         // Calibration offset (0.01% units, signed)
    TX_POWER = 0x05,                // LoRa TX power in dBm
    LED_ENABLED = 0x06,             // Enable/disable status LED
};

/**
 * @brief Sensor node configuration stored in flash
 *
 * Configurable runtime parameters for sensor nodes.
 * Stored in a dedicated flash sector separate from NodeConfiguration.
 *
 * Size: 32 bytes (fits in single flash page with room for growth)
 */
struct __attribute__((packed)) SensorNodeConfig {
    uint32_t magic;  // Magic number to validate data (0xCAFE5678)

    // Timing configuration (stored in seconds for efficient storage)
    uint16_t sensor_read_interval_s;  // 10-3600s (default: 30s)
    uint16_t transmit_interval_s;     // 60-86400s (default: 600s = 10min)

    // Calibration offsets (in 0.01 units, signed)
    int16_t temperature_offset;  // -1000 to 1000 (= -10.00°C to +10.00°C)
    int16_t humidity_offset;     // -1000 to 1000 (= -10.00% to +10.00%)

    // Radio configuration
    uint8_t tx_power;  // 2-20 dBm (default: 20)

    // Display configuration
    uint8_t led_enabled;  // 0=disabled, 1=enabled (default: 1)

    // Reserved for future parameters
    uint8_t reserved[14];

    // Integrity check
    uint32_t crc32;  // CRC32 of all fields above
};

static_assert(sizeof(SensorNodeConfig) == 32, "SensorNodeConfig must be exactly 32 bytes");

// Magic number for valid sensor configuration
constexpr uint32_t SENSOR_CONFIG_MAGIC = 0xCAFE5678;

// Default values matching current compile-time constants
constexpr uint16_t DEFAULT_SENSOR_READ_INTERVAL_S = 30;
constexpr uint16_t DEFAULT_TRANSMIT_INTERVAL_S = 600;
constexpr int16_t DEFAULT_TEMPERATURE_OFFSET = 0;
constexpr int16_t DEFAULT_HUMIDITY_OFFSET = 0;
constexpr uint8_t DEFAULT_TX_POWER = 20;
constexpr uint8_t DEFAULT_LED_ENABLED = 1;

// Parameter validation ranges
constexpr uint16_t MIN_SENSOR_READ_INTERVAL_S = 10;
constexpr uint16_t MAX_SENSOR_READ_INTERVAL_S = 3600;
constexpr uint16_t MIN_TRANSMIT_INTERVAL_S = 60;
constexpr uint16_t MAX_TRANSMIT_INTERVAL_S = 86400;
constexpr int16_t MIN_CALIBRATION_OFFSET = -1000;
constexpr int16_t MAX_CALIBRATION_OFFSET = 1000;
constexpr uint8_t MIN_TX_POWER = 2;
constexpr uint8_t MAX_TX_POWER = 20;
