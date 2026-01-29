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
 * - Sector 0 (0x00000000 - 0x00000FFF): Metadata (4KB)
 * - Data region (0x00001000 - 0x07FFFFFF): Sensor records (127.996 MB)
 *
 * Capacity: ~10.6 million records = ~12 years at 30-second intervals
 */
class SensorFlashBuffer {
public:
    // Flash layout constants
    static constexpr uint32_t METADATA_SECTOR = 0;
    static constexpr uint32_t METADATA_SIZE = 4096;
    static constexpr uint32_t DATA_START_OFFSET = 4096;
    static constexpr uint32_t DATA_REGION_SIZE = ExternalFlash::TOTAL_SIZE - DATA_START_OFFSET;
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
     * Sets the RECORD_FLAG_TRANSMITTED bit in the record's flags field.
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
     *
     * NOTE: This function does NOT advance read_index. The caller must
     * call advanceReadIndex(actual_count) after successful transmission
     * is confirmed via ACK callback to prevent data loss on failed transmissions.
     *
     * @param records Output buffer for records
     * @param max_count Maximum number of records to read
     * @param actual_count Output: actual number of records read
     * @return true if read successful
     */
    bool readUntransmittedRecords(SensorDataRecord *records, size_t max_count,
                                  size_t &actual_count);

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
     * @brief Get the persisted LoRa sequence number
     * @return Persisted next sequence number (0 if not set)
     */
    uint8_t getNextSeqNum() const;

    /**
     * @brief Save the LoRa sequence number to metadata (in-memory only)
     *
     * Call flush() to persist to flash. This avoids extra flash writes
     * since flush() is already called before sleep.
     *
     * @param seq_num Sequence number to save
     */
    void saveNextSeqNum(uint8_t seq_num);

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

    // Sector buffer for read-modify-write operations
    // Used by markTransmitted() to preserve other records when updating a single record
    uint8_t sector_buffer_[SECTOR_SIZE];

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
};
