#pragma once

#include <stdint.h>

/**
 * @brief Record flags bit definitions
 */
constexpr uint8_t RECORD_FLAG_VALID = 0x02;        // Bit 1: Record contains valid data
constexpr uint8_t RECORD_FLAG_TRANSMITTED = 0x01;  // Bit 0: Record transmitted to hub

/**
 * @brief Individual sensor data record stored in flash
 *
 * Each record is 12 bytes and stores a single temperature/humidity reading
 * with timestamp and transmission status.
 *
 * Temperature and humidity use fixed-point format (value * 100) to preserve
 * precision while using integer storage.
 *
 * Size: 12 bytes per record
 * Capacity in 128MB flash: ~10.6 million records = ~10 years at 30s intervals
 */
struct __attribute__((packed)) SensorDataRecord {
    uint32_t timestamp;   // Unix timestamp (seconds since epoch)
    int16_t temperature;  // Temperature in 0.01°C units (e.g., 2350 = 23.50°C)
    uint16_t humidity;    // Humidity in 0.01% units (e.g., 6500 = 65.00%)
    uint8_t flags;        // Status flags (RECORD_FLAG_VALID | RECORD_FLAG_TRANSMITTED)
    uint8_t reserved;     // Reserved for future use
    uint16_t crc16;       // CRC16 of all fields above
};

static_assert(sizeof(SensorDataRecord) == 12, "SensorDataRecord must be exactly 12 bytes");
