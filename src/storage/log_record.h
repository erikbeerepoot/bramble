#pragma once

#include <stdint.h>

/**
 * @brief Individual log record stored in flash
 *
 * Fixed 128-byte records for alignment with flash pages (2 per page, 32 per sector).
 */
struct __attribute__((packed)) LogRecord {
    uint32_t timestamp;    // ms since boot (or unix time if RTC valid)
    uint8_t level;         // LogLevel enum value
    char module[11];       // Module name, null-terminated
    char message[108];     // Message text, null-terminated
    uint16_t sequence;     // Rolling sequence number
    uint16_t crc16;        // CRC16-CCITT integrity check
};

static_assert(sizeof(LogRecord) == 128, "LogRecord must be 128 bytes");
