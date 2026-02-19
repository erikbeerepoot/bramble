#pragma once

#include <cstdint>
#include <cstring>

#include "pico/stdlib.h"

// Event severity levels (stored in bits 1:0 of severity field)
enum class EventSeverity : uint8_t {
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    CRITICAL = 3,
};

// Event types: high nibble = category, low nibble = specific event
enum class EventType : uint8_t {
    // 0x0_ Lifecycle
    BOOT_COLD = 0x00,
    BOOT_WATCHDOG = 0x01,
    SLEEP_ENTER = 0x02,
    WAKE = 0x03,

    // 0x1_ State transitions
    STATE_SENSOR = 0x10,      // detail = prev_state << 4 | new_state
    STATE_IRRIGATION = 0x11,  // detail = prev_state << 4 | new_state
    STATE_BASE = 0x12,        // detail = prev_state << 4 | new_state

    // 0x2_ Sensor
    SENSOR_INIT_OK = 0x20,
    SENSOR_INIT_FAIL = 0x21,
    SENSOR_READ_OK = 0x22,
    SENSOR_READ_FAIL = 0x23,

    // 0x3_ Storage
    FLASH_INIT_OK = 0x30,
    FLASH_INIT_FAIL = 0x31,
    FLASH_WRITE_FAIL = 0x32,
    FLASH_FULL = 0x33,

    // 0x4_ Radio
    REGISTRATION_OK = 0x40,
    REGISTRATION_FAIL = 0x41,
    TX_BATCH_OK = 0x42,
    TX_BATCH_FAIL = 0x43,
    TX_HEARTBEAT_OK = 0x44,
    TX_HEARTBEAT_FAIL = 0x45,
    TIME_SYNC_OK = 0x46,
    TIME_SYNC_TIMEOUT = 0x47,

    // 0x5_ Power
    BATTERY_LOW = 0x50,
    BATTERY_CRITICAL = 0x51,
    BATTERY_OK = 0x52,
    PMU_INIT_OK = 0x53,
    PMU_INIT_FAIL = 0x54,
    PMU_LOST = 0x55,

    // 0x6_ System
    RTC_SYNCED = 0x60,
    RTC_LOST = 0x61,
    VALVE_OPEN = 0x62,
    VALVE_CLOSE = 0x63,
};

// 6-byte packed event record
struct __attribute__((packed)) EventRecord {
    uint16_t uptime_seconds;  // Seconds since boot (from to_ms_since_boot()/1000)
    uint8_t event_type;       // EventType (high nibble = category, low nibble = event)
    uint8_t severity;         // EventSeverity in bits 1:0
    uint8_t detail;           // Event-specific context byte
    uint8_t sequence;         // Monotonic counter (0-255, wraps)
};

static_assert(sizeof(EventRecord) == 6, "EventRecord must be 6 bytes");

/**
 * @brief RAM-based circular event log
 *
 * Records notable events (state transitions, errors, boot/sleep) in a fixed-size
 * ring buffer. Events are read out for transmission to the hub, then advanced.
 * No flash, no SPI - pure RAM to avoid contention with SX1276.
 *
 * @tparam CAPACITY Maximum number of events in the ring buffer (default 64)
 */
template <size_t CAPACITY = 64>
class EventLog {
public:
    /**
     * @brief Record an event
     * @param type Event type
     * @param severity Event severity
     * @param detail Event-specific context byte (default 0)
     */
    void record(EventType type, EventSeverity severity, uint8_t detail = 0)
    {
        EventRecord &event = buffer_[write_index_];
        event.uptime_seconds = static_cast<uint16_t>(to_ms_since_boot(get_absolute_time()) / 1000);
        event.event_type = static_cast<uint8_t>(type);
        event.severity = static_cast<uint8_t>(severity);
        event.detail = detail;
        event.sequence = next_sequence_++;

        write_index_ = (write_index_ + 1) % CAPACITY;

        if (count_ < CAPACITY) {
            count_++;
        } else {
            // Overflow: oldest event overwritten, advance read index
            read_index_ = (read_index_ + 1) % CAPACITY;
            total_lost_++;
        }
    }

    /**
     * @brief Record a state transition event
     * @param type State transition event type (STATE_SENSOR, STATE_IRRIGATION, etc.)
     * @param previous_state Previous state value (0-15)
     * @param new_state New state value (0-15)
     */
    void recordStateTransition(EventType type, uint8_t previous_state, uint8_t new_state)
    {
        uint8_t detail = (previous_state << 4) | (new_state & 0x0F);
        record(type, EventSeverity::INFO, detail);
    }

    /**
     * @brief Read pending events without advancing the read index
     * @param output Array to copy events into
     * @param max_count Maximum number of events to read
     * @return Number of events actually copied
     */
    size_t readPending(EventRecord *output, size_t max_count) const
    {
        size_t pending = pendingCount();
        size_t to_read = (pending < max_count) ? pending : max_count;

        for (size_t i = 0; i < to_read; i++) {
            size_t index = (read_index_ + i) % CAPACITY;
            output[i] = buffer_[index];
        }

        return to_read;
    }

    /**
     * @brief Advance the read index after successful transmission
     * @param count Number of events to advance past
     */
    void advanceReadIndex(size_t count)
    {
        size_t pending = pendingCount();
        size_t to_advance = (count < pending) ? count : pending;
        read_index_ = (read_index_ + to_advance) % CAPACITY;
        count_ -= to_advance;
    }

    /**
     * @brief Check if there are pending events to transmit
     */
    bool hasPending() const { return count_ > 0; }

    /**
     * @brief Get the number of pending events
     */
    size_t pendingCount() const { return count_; }

    /**
     * @brief Get total number of events lost due to ring buffer overflow
     */
    uint32_t totalLost() const { return total_lost_; }

    /**
     * @brief Set the time reference mapping (uptime -> Unix timestamp)
     *
     * Called when RTC syncs to establish the mapping between monotonic
     * uptime and wall-clock time. The transmitter includes this in the
     * batch header so the hub can reconstruct Unix timestamps.
     *
     * @param uptime_seconds Uptime at which RTC was synced
     * @param unix_timestamp Unix timestamp at that uptime
     */
    void setTimeReference(uint32_t uptime_seconds, uint32_t unix_timestamp)
    {
        reference_uptime_ = uptime_seconds;
        reference_timestamp_ = unix_timestamp;
    }

    /**
     * @brief Get the reference uptime (seconds since boot when RTC synced)
     */
    uint32_t getReferenceUptime() const { return reference_uptime_; }

    /**
     * @brief Get the reference Unix timestamp (wall-clock time when RTC synced)
     */
    uint32_t getReferenceTimestamp() const { return reference_timestamp_; }

    // =========================================================================
    // Flash Persistence
    // =========================================================================

    /**
     * @brief Flash-persisted header for event log state
     *
     * Written to a dedicated flash sector so event log survives sleep cycles.
     * Padded to flash page boundary (256 bytes) for efficient writes.
     */
    struct __attribute__((packed)) FlashHeader {
        uint32_t magic;  // 0x45564C47 ("EVLG")
        uint16_t write_index;
        uint16_t read_index;
        uint16_t count;
        uint8_t next_sequence;
        uint8_t reserved;
        uint32_t total_lost;
        uint32_t reference_uptime;
        uint32_t reference_timestamp;
        // 24 bytes total header
    };

    static constexpr uint32_t FLASH_MAGIC = 0x45564C47;  // "EVLG"

    /**
     * @brief Serialize event log state to a buffer for flash persistence
     *
     * Writes FlashHeader + event records. Buffer must be large enough
     * for sizeof(FlashHeader) + CAPACITY * sizeof(EventRecord).
     *
     * @param buffer Output buffer
     * @param max_length Buffer size
     * @return Number of bytes written, or 0 on error
     */
    size_t serialize(uint8_t *buffer, size_t max_length) const
    {
        size_t needed = sizeof(FlashHeader) + CAPACITY * sizeof(EventRecord);
        if (max_length < needed) {
            return 0;
        }

        FlashHeader header = {};
        header.magic = FLASH_MAGIC;
        header.write_index = static_cast<uint16_t>(write_index_);
        header.read_index = static_cast<uint16_t>(read_index_);
        header.count = static_cast<uint16_t>(count_);
        header.next_sequence = next_sequence_;
        header.reserved = 0;
        header.total_lost = total_lost_;
        header.reference_uptime = reference_uptime_;
        header.reference_timestamp = reference_timestamp_;

        memcpy(buffer, &header, sizeof(header));
        memcpy(buffer + sizeof(header), buffer_, CAPACITY * sizeof(EventRecord));

        return needed;
    }

    /**
     * @brief Deserialize event log state from a flash buffer
     *
     * Restores ring buffer state from a previously serialized buffer.
     * Validates magic number and basic consistency.
     *
     * @param buffer Input buffer
     * @param length Buffer size
     * @return true if restore succeeded, false if data invalid
     */
    bool deserialize(const uint8_t *buffer, size_t length)
    {
        size_t needed = sizeof(FlashHeader) + CAPACITY * sizeof(EventRecord);
        if (length < needed) {
            return false;
        }

        FlashHeader header;
        memcpy(&header, buffer, sizeof(header));

        if (header.magic != FLASH_MAGIC) {
            return false;
        }

        // Basic consistency checks
        if (header.write_index >= CAPACITY || header.read_index >= CAPACITY ||
            header.count > CAPACITY) {
            return false;
        }

        write_index_ = header.write_index;
        read_index_ = header.read_index;
        count_ = header.count;
        next_sequence_ = header.next_sequence;
        total_lost_ = header.total_lost;
        reference_uptime_ = header.reference_uptime;
        reference_timestamp_ = header.reference_timestamp;

        memcpy(buffer_, buffer + sizeof(header), CAPACITY * sizeof(EventRecord));

        return true;
    }

    /**
     * @brief Get the serialized size needed for flash persistence
     */
    static constexpr size_t serializedSize()
    {
        return sizeof(FlashHeader) + CAPACITY * sizeof(EventRecord);
    }

private:
    EventRecord buffer_[CAPACITY] = {};
    size_t write_index_ = 0;
    size_t read_index_ = 0;
    size_t count_ = 0;
    uint8_t next_sequence_ = 0;
    uint32_t total_lost_ = 0;

    // Time reference: maps uptime to Unix time
    uint32_t reference_uptime_ = 0;
    uint32_t reference_timestamp_ = 0;  // 0 = never synced
};
