#pragma once

#include <stdint.h>

/**
 * @brief Record flags bit definitions
 */
constexpr uint8_t RECORD_FLAG_VALID = 0x02;  // Bit 1: Record contains valid data

/**
 * @brief Transmission status values (NOR-flash-friendly)
 *
 * Uses a dedicated byte instead of a flag bit so that marking a record as
 * transmitted is a single in-place write (0xFF -> 0x00) with no sector erase.
 * NOR flash can only clear bits (1->0), so writing 0x00 over the erased
 * state (0xFF) works without erasing the sector first.
 */
constexpr uint8_t RECORD_NOT_TRANSMITTED = 0xFF;  // Erased state: not yet transmitted
constexpr uint8_t RECORD_TRANSMITTED = 0x00;      // Cleared state: successfully ACK'd

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
    uint32_t timestamp;           // Unix timestamp (seconds since epoch)
    int16_t temperature;          // Temperature in 0.01°C units (e.g., 2350 = 23.50°C)
    uint16_t humidity;            // Humidity in 0.01% units (e.g., 6500 = 65.00%)
    uint8_t flags;                // Status flags (RECORD_FLAG_VALID)
    uint8_t transmission_status;  // 0xFF = not transmitted, 0x00 = transmitted (NOR-flash-friendly)
    uint16_t crc16;               // CRC16 of all fields above
};

static_assert(sizeof(SensorDataRecord) == 12, "SensorDataRecord must be exactly 12 bytes");

/**
 * @brief Check if a record has been successfully transmitted and ACK'd
 */
inline bool isRecordTransmitted(const SensorDataRecord &record)
{
    return record.transmission_status != RECORD_NOT_TRANSMITTED;
}
