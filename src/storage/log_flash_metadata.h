#pragma once

#include <stdint.h>

constexpr uint32_t LOG_FLASH_MAGIC = 0x4C4F4753;    // "LOGS" in ASCII
constexpr uint32_t LOG_FLASH_VERSION = 1;

/**
 * @brief Metadata for the log circular buffer, stored in its own flash sector
 */
struct __attribute__((packed)) LogFlashMetadata {
    uint32_t magic;
    uint32_t version;
    uint32_t write_index;      // Next record index to write
    uint32_t total_records;    // Total records written (lifetime, wraps)
    uint16_t next_sequence;    // Next sequence number for ordering
    uint8_t reserved[4074];    // Pad to 4KB
    uint32_t crc32;
};

static_assert(sizeof(LogFlashMetadata) == 4096, "LogFlashMetadata must be exactly 4096 bytes");
