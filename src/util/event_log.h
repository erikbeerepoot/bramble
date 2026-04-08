#pragma once

#include <cstdint>
#include <cstring>

#include "pico/stdlib.h"

#include "event_record.h"

/**
 * @brief RAM ring buffer for event records
 *
 * Records notable events during a wake cycle. Events are transmitted
 * via EventLogTransmitter before sleep. No flash persistence — RAM is
 * lost on sleep, which is acceptable for diagnostic data.
 *
 * @tparam N Maximum number of records in the buffer
 */
template <size_t N>
class EventLog {
public:
    /**
     * @brief Record a new event
     * @param type Event type
     * @param severity 0=info, 1=warn, 2=error
     * @param detail Event-specific detail value
     */
    void record(EventType type, uint8_t severity, uint16_t detail)
    {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        uint32_t offset_s = 0;
        if (now_ms >= time_ref_uptime_ms_) {
            offset_s = (now_ms - time_ref_uptime_ms_) / 1000;
        }
        if (offset_s > 65535) {
            offset_s = 65535;
        }

        EventRecord &rec = records_[write_index_];
        rec.uptime_offset = static_cast<uint16_t>(offset_s);
        rec.event_type = static_cast<uint8_t>(type);
        rec.severity = severity;
        rec.detail = detail;

        write_index_ = (write_index_ + 1) % N;
        if (count_ < N) {
            count_++;
        } else {
            // Overwrite oldest — advance read pointer
            read_index_ = (read_index_ + 1) % N;
        }
    }

    /**
     * @brief Check if there are untransmitted events
     */
    bool hasPending() const { return count_ > 0; }

    /**
     * @brief Get number of pending events
     */
    uint8_t pendingCount() const { return count_; }

    /**
     * @brief Read pending records into output buffer
     * @param out Output buffer
     * @param max Maximum records to read
     * @return Number of records copied
     */
    uint8_t readPending(EventRecord *out, uint8_t max) const
    {
        uint8_t to_read = (count_ < max) ? count_ : max;
        for (uint8_t i = 0; i < to_read; i++) {
            out[i] = records_[(read_index_ + i) % N];
        }
        return to_read;
    }

    /**
     * @brief Advance read pointer after successful transmission
     * @param count Number of records to advance past
     */
    void advanceRead(uint8_t count)
    {
        if (count > count_) {
            count = count_;
        }
        read_index_ = (read_index_ + count) % N;
        count_ -= count;
    }

    /**
     * @brief Set time reference for uptime-to-unix conversion
     * @param uptime_ms Current uptime in milliseconds
     * @param unix_ts Current Unix timestamp (seconds)
     */
    void setTimeReference(uint32_t uptime_ms, uint32_t unix_ts)
    {
        time_ref_uptime_ms_ = uptime_ms;
        time_ref_unix_ = unix_ts;
    }

    uint32_t timeRefUnix() const { return time_ref_unix_; }
    uint32_t timeRefUptime() const { return time_ref_uptime_ms_; }

private:
    EventRecord records_[N] = {};
    uint8_t write_index_ = 0;
    uint8_t read_index_ = 0;
    uint8_t count_ = 0;
    uint32_t time_ref_uptime_ms_ = 0;
    uint32_t time_ref_unix_ = 0;
};
