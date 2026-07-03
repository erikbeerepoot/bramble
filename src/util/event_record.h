#pragma once

#include <cstdint>

/**
 * @brief Event type categories
 *
 * Organized by functional area. Each category gets a 0xN0 range.
 */
enum class EventType : uint8_t {
    // 0x1_ System
    BOOT_COLD = 0x10,
    BOOT_WATCHDOG = 0x11,
    WAKE = 0x12,  // Warm boot from PMU sleep (RP2350 was power-cycled but PMU state restored)

    // 0x2_ Sensor
    SENSOR_INIT_OK = 0x20,
    SENSOR_INIT_FAIL = 0x21,
    SENSOR_READ_OK = 0x22,
    SENSOR_READ_FAIL = 0x23,

    // 0x3_ Network
    REGISTRATION_OK = 0x30,
    REGISTRATION_FAIL = 0x31,
    TIME_SYNC_OK = 0x32,
    TIME_SYNC_TIMEOUT = 0x33,
    TX_BATCH_OK = 0x34,
    TX_BATCH_FAIL = 0x35,

    // 0x5_ Power
    SLEEP_ENTER = 0x50,

    // 0x7_ Irrigation
    SCHEDULE_APPLIED = 0x70,
    SCHEDULE_REMOVED = 0x71,
    SCHEDULE_FAILED = 0x72,
    VALVE_TIMER_SET = 0x73,
    VALVE_TIMER_CLOSE = 0x74,
    VALVE_OPEN = 0x75,
    VALVE_CLOSE = 0x76,
};

/**
 * @brief Compact 8-byte event record for logging notable node events
 */
struct __attribute__((packed)) EventRecord {
    uint32_t uptime_offset;  // Milliseconds since time reference
    uint8_t event_type;      // EventType enum value
    uint8_t severity;        // 0=info, 1=warn, 2=error
    uint16_t detail;         // Event-specific detail value
};
static_assert(sizeof(EventRecord) == 8, "EventRecord must be 8 bytes");

/**
 * @brief Legacy 6-byte wire format, used only by hub_mode.cpp to parse
 * EVENT_LOG batches from nodes still running pre-ms-precision firmware.
 * Not used for new records — do not construct this on the TX side.
 *
 * TODO(remove-after-fleet-upgrade): once every deployed node has been
 * reflashed past the ms-precision change (this struct's introduction),
 * delete this struct and the legacy-format branch in
 * HubMode::handleEventLogBatch (src/modes/hub_mode.cpp). Check for
 * "sent legacy-format event log batch" in hub logs across the fleet to
 * confirm no node is still sending this format before removing it.
 */
struct __attribute__((packed)) EventRecordV1 {
    uint16_t uptime_offset;  // Seconds since time reference (wraps at 65535)
    uint8_t event_type;      // EventType enum value
    uint8_t severity;        // 0=info, 1=warn, 2=error
    uint16_t detail;         // Event-specific detail value
};
static_assert(sizeof(EventRecordV1) == 6, "EventRecordV1 must be 6 bytes");
