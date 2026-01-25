#pragma once

#include <stdint.h>

/**
 * @brief Magic number for sensor flash metadata validation
 */
constexpr uint32_t SENSOR_FLASH_MAGIC = 0x53454E53;  // "SENS" in ASCII

/**
 * @brief Current metadata format version
 */
constexpr uint32_t SENSOR_FLASH_VERSION = 1;

/**
 * @brief Metadata structure stored in sector 0 of external flash
 *
 * This structure tracks the state of the circular buffer including
 * write/read positions and statistics about data transmission.
 *
 * Size: 4096 bytes (one flash sector)
 */
struct __attribute__((packed)) SensorFlashMetadata {
    uint32_t magic;                   // Magic number (0x53454E53 = "SENS")
    uint32_t version;                 // Format version (1)
    uint32_t write_index;             // Next record index to write (circular)
    uint32_t read_index;              // Next record index to transmit (circular)
    uint32_t total_records;           // Total records written (wraps at UINT32_MAX)
    uint32_t records_transmitted;     // Records successfully transmitted
    uint32_t records_lost;            // Records overwritten before transmission
    uint32_t last_sync_timestamp;     // Unix timestamp of last hub sync
    uint32_t initial_boot_timestamp;  // Unix timestamp of first boot after power loss
    uint8_t reserved[4056];           // Pad to 4KB sector size (reduced by 4 bytes)
    uint32_t crc32;                   // CRC32 of all fields above
};

static_assert(sizeof(SensorFlashMetadata) == 4096,
              "SensorFlashMetadata must be exactly 4096 bytes (one sector)");
