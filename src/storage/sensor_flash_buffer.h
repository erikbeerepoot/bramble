#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../hal/external_flash.h"
#include "../hal/logger.h"
#include "sensor_data_record.h"
#include "sensor_flash_metadata.h"

/**
 * @brief CRC16-CCITT calculation utility
 */
class CRC16 {
public:
    /**
     * @brief Calculate CRC16-CCITT for data buffer
     * @param data Data buffer
     * @param length Length of data
     * @param initial_value Initial CRC value (default 0xFFFF)
     * @return CRC16 checksum
     */
    static uint16_t calculate(const uint8_t *data, size_t length, uint16_t initial_value = 0xFFFF);

    /**
     * @brief Calculate CRC16 for a sensor data record (excluding CRC field)
     * @param record Record to calculate CRC for
     * @return CRC16 checksum
     */
    static uint16_t calculateRecordCRC(const SensorDataRecord &record);

private:
    static const uint16_t crc16_table[256];
};

/**
 * @brief Circular buffer for sensor data in external flash
 *
 * Manages a circular buffer in external flash memory (MT25QL01GBBB 128MB).
 * The buffer stores sensor readings with automatic wraparound when full.
 *
 * Layout:
 * - Sector 0 (0x00000000 - 0x00000FFF): Sensor metadata (4KB)
 * - Data region (0x00001000 onwards): Sensor records
 *
 * Capacity: ~8 million sensor records = ~7.6 years at 30-second intervals
 */
class SensorFlashBuffer {
public:
    // Flash layout constants
    static constexpr uint32_t METADATA_SECTOR = 0;
    static constexpr uint32_t METADATA_SIZE = 4096;
    static constexpr uint32_t DATA_START_OFFSET = 0x00002000;  // After both metadata sectors
    static constexpr uint32_t DATA_END_OFFSET = 0x06000000;    // Log region starts here
    static constexpr uint32_t DATA_REGION_SIZE = DATA_END_OFFSET - DATA_START_OFFSET;
    static constexpr uint32_t MAX_RECORDS = DATA_REGION_SIZE / sizeof(SensorDataRecord);

    // Batch transmission constants (must match MAX_BATCH_RECORDS in message.h)
    static constexpr size_t BATCH_SIZE = 19;

    // Sector geometry
    static constexpr uint32_t SECTOR_SIZE = ExternalFlash::SECTOR_SIZE;                     // 4KB
    static constexpr uint32_t RECORDS_PER_SECTOR = SECTOR_SIZE / sizeof(SensorDataRecord);  // ~341

    /**
     * @brief Construct sensor flash buffer
     * @param flash External flash driver instance
     */
    explicit SensorFlashBuffer(ExternalFlash &flash);

    /**
     * @brief Initialize the flash buffer
     *
     * Loads metadata from flash. If metadata is invalid or missing,
     * initializes a new circular buffer with fresh metadata.
     *
     * @return true if initialization successful
     */
    bool init();

    /**
     * @brief Write a sensor data record to flash
     *
     * Writes the record to the current write position, calculates and stores
     * CRC16, then advances the write pointer. If buffer is full, overwrites
     * oldest untransmitted record and increments records_lost counter.
     *
     * @param record Record to write (timestamp, temp, humidity, flags)
     * @return true if write successful
     */
    bool writeRecord(const SensorDataRecord &record);

    /**
     * @brief Mark a record as transmitted
     *
     * Writes RECORD_TRANSMITTED (0x00) to the record's transmission_status byte.
     * This is a single in-place byte write — no sector erase required on NOR flash.
     * CRC remains valid because calculateRecordCRC() normalizes this field.
     *
     * @param index Record index in circular buffer (0 to MAX_RECORDS-1)
     * @return true if marking successful
     */
    bool markTransmitted(uint32_t index);

    /**
     * @brief Read pending records for batch transmission
     *
     * Reads up to max_count valid records starting from read_index.
     * Records between read_index and write_index are considered pending.
     * Includes retry logic (3 attempts) for transient flash read failures.
     *
     * NOTE: This function does NOT advance read_index. The caller must
     * call advanceReadIndex() after successful transmission is confirmed
     * via ACK callback to prevent data loss on failed transmissions.
     *
     * When valid_records_count == 0 but total_records_scanned > 0, all scanned records had
     * CRC errors and can be skipped. When total_records_scanned < untransmitted_count,
     * a read failure occurred and unscanned records should NOT be skipped.
     *
     * @param records Output buffer for records
     * @param max_count Maximum number of records to read
     * @param valid_records_count Output: number of valid records read
     * @param total_records_scanned Output: total records examined (including CRC errors)
     * @return true if read operation completed (even if no valid records found)
     */
    bool readUntransmittedRecords(SensorDataRecord *records, size_t max_count,
                                  size_t &valid_records_count, size_t &total_records_scanned);

    /**
     * @brief Get current buffer statistics
     *
     * Returns a copy of the current metadata including write/read positions,
     * record counts, and transmission statistics.
     *
     * @param stats Output: current statistics
     * @return true if retrieval successful
     */
    bool getStatistics(SensorFlashMetadata &stats) const;

    /**
     * @brief Get number of untransmitted records
     * @return Count of records not yet transmitted
     */
    uint32_t getUntransmittedCount() const;

    /**
     * @brief Advance read index after successful transmission
     *
     * Called by the caller after transmission is confirmed via ACK callback.
     * This ensures records are only skipped after successful delivery.
     *
     * @param count Number of records to advance past
     * @return true if advance successful
     */
    bool advanceReadIndex(uint32_t count);

    /**
     * @brief Update last sync timestamp
     *
     * Called when hub acknowledges batch transmission.
     *
     * @param timestamp Unix timestamp of sync
     * @return true if update successful
     */
    bool updateLastSync(uint32_t timestamp);

    /**
     * @brief Get the initial boot timestamp
     *
     * Returns the Unix timestamp of when the device first booted after a
     * power loss (battery disconnection). This timestamp is set once when
     * RTC first becomes valid after a cold start.
     *
     * @return Unix timestamp of initial boot, or 0 if not yet set
     */
    uint32_t getInitialBootTimestamp() const;

    /**
     * @brief Set the initial boot timestamp (only if not already set)
     *
     * Sets the initial boot timestamp for uptime calculation. This should
     * only be called once after RTC sync on first boot after power loss.
     * If the timestamp is already set (non-zero), this call is a no-op.
     *
     * @param timestamp Unix timestamp to set
     * @return true if timestamp was set, false if already set or error
     */
    bool setInitialBootTimestamp(uint32_t timestamp);

    /**
     * @brief Flush metadata to flash
     *
     * Call this before power down to ensure write_index is persisted.
     * Without this, records written since last flush may be overwritten
     * on next boot (NOR flash corruption).
     *
     * @return true if flush successful
     */
    bool flush();

    /**
     * @brief Reset the circular buffer (DANGEROUS - erases all data)
     *
     * Erases metadata sector and reinitializes with fresh metadata.
     * Does NOT erase data sectors (lazy overwrite on wraparound).
     *
     * @return true if reset successful
     */
    bool reset();

    /**
     * @brief Scan flash to find write index (cold start recovery)
     *
     * Used on cold start when PMU state is invalid. Scans the data region
     * to find the last valid record and sets write_index accordingly.
     * This is expensive (reads entire flash) but only happens on cold start.
     *
     * @return true if scan successful, write_index updated
     */
    bool scanForWriteIndex();

    /**
     * @brief Fast-forward read_index past already-transmitted records
     *
     * On cold start, read_index may be stale (from flash metadata). This method
     * efficiently advances read_index by reading only the 1-byte transmission_status
     * field per record, rather than the full 12-byte record.
     *
     * @return Number of records skipped
     */
    uint32_t fastForwardReadIndex();

    /**
     * @brief Set read index directly (restore from PMU state)
     *
     * Used when restoring state from PMU RAM on warm start. No validation
     * is performed - caller must ensure the index is valid.
     *
     * @param index Read index to set
     */
    void setReadIndex(uint32_t index) { metadata_.read_index = index; }

    /**
     * @brief Set write index directly (restore from PMU state)
     *
     * Used when restoring state from PMU RAM on warm start. No validation
     * is performed - caller must ensure the index is valid.
     *
     * @param index Write index to set
     */
    void setWriteIndex(uint32_t index) { metadata_.write_index = index; }

    /**
     * @brief Get current read index
     * @return Current read index
     */
    uint32_t getReadIndex() const { return metadata_.read_index; }

    /**
     * @brief Get current write index
     * @return Current write index
     */
    uint32_t getWriteIndex() const { return metadata_.write_index; }

    /**
     * @brief Check if write_index points to erased flash (ready for writing)
     *
     * Used to validate PMU-restored write_index. If the location isn't erased,
     * the PMU state is stale and scanForWriteIndex() should be used instead.
     *
     * @return true if the location at write_index is erased
     */
    bool isWriteLocationErased();

    /**
     * @brief Check if flash is healthy (no recent write failures)
     *
     * Returns false if the most recent write operation failed,
     * indicating flash may be unreliable and direct transmit
     * fallback should be used.
     *
     * @return true if flash is healthy, false if last write failed
     */
    bool isHealthy() const { return healthy_; }

private:
    ExternalFlash &flash_;
    SensorFlashMetadata metadata_;
    bool initialized_;
    bool healthy_ = true;  // Track flash write health for fallback decisions
    Logger logger_;

    /**
     * @brief Load metadata from flash
     * @return true if metadata is valid
     */
    bool loadMetadata();

    /**
     * @brief Save metadata to flash
     * @return true if save successful
     */
    bool saveMetadata();

    /**
     * @brief Initialize fresh metadata
     */
    void initializeMetadata();

    /**
     * @brief Calculate flash address for a record index
     * @param index Record index (0 to MAX_RECORDS-1)
     * @return Flash address
     */
    uint32_t getRecordAddress(uint32_t index) const;

    /**
     * @brief Validate metadata structure
     * @return true if metadata is valid
     */
    bool validateMetadata() const;

    /**
     * @brief Check if buffer is full (write_index caught up to read_index)
     * @return true if buffer is full
     */
    bool isFull() const;

    /**
     * @brief Check if a flash location is erased (all 0xFF)
     * @param address Flash address to check
     * @param length Number of bytes to verify
     * @return true if all bytes are 0xFF (erased state)
     */
    bool isLocationErased(uint32_t address, size_t length);
};
