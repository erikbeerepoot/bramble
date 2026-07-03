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
        uint32_t offset_ms = 0;
        if (now_ms >= time_ref_uptime_ms_) {
            offset_ms = now_ms - time_ref_uptime_ms_;
        }

        EventRecord &rec = records_[write_index_];
        rec.uptime_offset = offset_ms;
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

    /**
     * @brief Load previously-persisted records into the ring buffer as pending
     *
     * Used on wake to restore events that failed to transmit in the previous cycle.
     * Records are inserted at the current write position and become pending for TX.
     *
     * @param records Array of EventRecords to load
     * @param count Number of records
     * @param time_ref_uptime Uptime reference from when records were created
     * @param time_ref_unix Unix timestamp reference from when records were created
     */
    void loadPersisted(const EventRecord *records, uint8_t count, uint32_t time_ref_uptime,
                       uint32_t time_ref_unix)
    {
        // Set time reference from persisted data (will be overwritten when RTC syncs)
        if (time_ref_unix > 0) {
            time_ref_uptime_ms_ = time_ref_uptime;
            time_ref_unix_ = time_ref_unix;
        }

        for (uint8_t i = 0; i < count && count_ < N; i++) {
            records_[write_index_] = records[i];
            write_index_ = (write_index_ + 1) % N;
            count_++;
        }
    }

    /**
     * @brief Serialize pending events into a blob buffer
     *
     * Format: {time_ref_unix:4, time_ref_uptime:4, record_count:1, records[]}
     *
     * @param buffer Output buffer
     * @param max_length Maximum buffer size
     * @return Number of bytes written (0 if nothing pending or buffer too small)
     */
    uint16_t serializeToBlob(uint8_t *buffer, uint16_t max_length) const
    {
        if (!hasPending() || max_length < 9) {
            return 0;
        }

        // Header: time refs + count
        uint32_t unix_ts = time_ref_unix_;
        uint32_t uptime = time_ref_uptime_ms_;
        memcpy(buffer, &unix_ts, 4);
        memcpy(buffer + 4, &uptime, 4);

        uint8_t max_records = static_cast<uint8_t>((max_length - 9) / sizeof(EventRecord));
        uint8_t to_write = (count_ < max_records) ? count_ : max_records;
        buffer[8] = to_write;

        // Records
        for (uint8_t i = 0; i < to_write; i++) {
            memcpy(buffer + 9 + i * sizeof(EventRecord), &records_[(read_index_ + i) % N],
                   sizeof(EventRecord));
        }

        return 9 + to_write * sizeof(EventRecord);
    }

    /**
     * @brief Deserialize a blob and load records into this event log
     *
     * @param data Blob data in the format written by serializeToBlob
     * @param length Length of blob data
     */
    void deserializeFromBlob(const uint8_t *data, uint16_t length)
    {
        if (length < 9) {
            return;
        }

        uint32_t unix_ts;
        uint32_t uptime;
        memcpy(&unix_ts, data, 4);
        memcpy(&uptime, data + 4, 4);

        uint8_t record_count = data[8];
        uint16_t expected = 9 + record_count * sizeof(EventRecord);
        if (expected > length) {
            record_count = static_cast<uint8_t>((length - 9) / sizeof(EventRecord));
        }

        const EventRecord *records = reinterpret_cast<const EventRecord *>(data + 9);
        loadPersisted(records, record_count, uptime, unix_ts);
    }

private:
    EventRecord records_[N] = {};
    uint8_t write_index_ = 0;
    uint8_t read_index_ = 0;
    uint8_t count_ = 0;
    uint32_t time_ref_uptime_ms_ = 0;
    uint32_t time_ref_unix_ = 0;
};
