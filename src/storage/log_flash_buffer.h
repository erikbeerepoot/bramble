#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../hal/external_flash.h"
#include "../hal/logger.h"
#include "log_flash_metadata.h"
#include "log_record.h"

/**
 * @brief Circular buffer for log records in external flash
 *
 * Layout on external flash:
 * - Metadata sector at LOG_METADATA_OFFSET (4KB)
 * - Data region from LOG_DATA_START to LOG_DATA_END (32MB)
 *
 * Capacity: 262,144 log records (32MB / 128 bytes)
 */
class LogFlashBuffer {
public:
    // Flash layout constants
    static constexpr uint32_t METADATA_OFFSET = 0x00001000;  // Sector 1
    static constexpr uint32_t DATA_START = 0x06000000;       // 96MB offset
    static constexpr uint32_t DATA_END = 0x08000000;         // 128MB boundary
    static constexpr uint32_t DATA_SIZE = DATA_END - DATA_START;
    static constexpr uint32_t MAX_RECORDS = DATA_SIZE / sizeof(LogRecord);
    static constexpr uint32_t SECTOR_SIZE = ExternalFlash::SECTOR_SIZE;
    static constexpr uint32_t RECORDS_PER_SECTOR = SECTOR_SIZE / sizeof(LogRecord);

    explicit LogFlashBuffer(ExternalFlash &flash);

    bool init();

    /**
     * @brief Write a log entry to flash
     * @return true on success
     */
    bool writeLog(uint8_t level, const char *module, const char *message, uint32_t timestamp);

    /**
     * @brief Read a record by absolute index in the circular buffer
     * @param index Index (0 to MAX_RECORDS-1)
     * @param record Output record
     * @return true if record is valid
     */
    bool readRecord(uint32_t index, LogRecord &record) const;

    /**
     * @brief Get total number of stored records (capped at MAX_RECORDS)
     */
    uint32_t getStoredCount() const;

    /**
     * @brief Get the oldest record index (start of readable range)
     */
    uint32_t getOldestIndex() const;

    /**
     * @brief Get the write index (one past newest)
     */
    uint32_t getWriteIndex() const { return metadata_.write_index; }

    /**
     * @brief Flush page buffer and metadata to flash
     */
    bool flush();

    bool reset();

private:
    ExternalFlash &flash_;
    LogFlashMetadata metadata_;
    bool initialized_;
    Logger logger_;

    // Page write buffer: accumulate records before writing a full page
    LogRecord page_buffer_[2];  // 256 bytes = 1 flash page
    uint8_t page_buffer_count_;

    // Re-entrancy guard to prevent recursive flash writes during error logging
    bool flushing_;

    bool loadMetadata();
    bool saveMetadata();
    void initializeMetadata();
    bool validateMetadata() const;

    uint32_t getRecordAddress(uint32_t index) const;
    bool flushPageBuffer();

    /**
     * @brief Erase sector if the write is entering a new one
     */
    bool ensureSectorErased(uint32_t address);

    // Track which sector was last erased to avoid redundant erases
    uint32_t last_erased_sector_;
};
