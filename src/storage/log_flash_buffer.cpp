#include "log_flash_buffer.h"

#include <cstring>

#include "../config/config_base.h"
#include "sensor_flash_buffer.h"  // For CRC16

LogFlashBuffer::LogFlashBuffer(ExternalFlash &flash)
    : flash_(flash), initialized_(false), logger_("LogFlash"), ram_buffer_count_(0),
      flushing_(false), last_erased_sector_(UINT32_MAX)
{
    memset(&metadata_, 0, sizeof(metadata_));
    memset(ram_buffer_, 0, sizeof(ram_buffer_));
}

bool LogFlashBuffer::init()
{
    if (!flash_.isInitialized()) {
        logger_.error("External flash not initialized");
        return false;
    }

    if (loadMetadata()) {
        logger_.debug("Log flash: %lu records written", metadata_.total_records);
        initialized_ = true;
        return true;
    }

    logger_.warn("No valid log metadata, initializing fresh");
    initializeMetadata();
    if (!saveMetadata()) {
        logger_.error("Failed to save initial log metadata");
        return false;
    }

    initialized_ = true;
    return true;
}

bool LogFlashBuffer::writeLog(uint8_t level, const char *module, const char *message,
                              uint32_t timestamp)
{
    if (!initialized_)
        return false;

    // If buffer is full, drop the log (will be flushed before sleep)
    if (ram_buffer_count_ >= RAM_BUFFER_SIZE) {
        return false;
    }

    LogRecord &record = ram_buffer_[ram_buffer_count_];
    memset(&record, 0, sizeof(LogRecord));

    record.timestamp = timestamp;
    record.level = level;
    strncpy(record.module, module, sizeof(record.module) - 1);
    strncpy(record.message, message, sizeof(record.message) - 1);
    record.sequence = metadata_.next_sequence++;

    // CRC over everything except the CRC field
    record.crc16 = CRC16::calculate(reinterpret_cast<const uint8_t *>(&record),
                                    sizeof(LogRecord) - sizeof(record.crc16));

    ram_buffer_count_++;

    // Don't auto-flush - only flush on explicit flush() call before sleep
    return true;
}

bool LogFlashBuffer::flushPageBuffer()
{
    if (ram_buffer_count_ == 0)
        return true;

    // Prevent recursion during error logging
    if (flushing_) {
        return false;
    }
    flushing_ = true;

    // Write records in pages (2 records = 256 bytes = 1 flash page)
    uint8_t records_written = 0;
    while (records_written < ram_buffer_count_) {
        // Calculate how many records to write in this page (max 2)
        uint8_t records_in_page = ram_buffer_count_ - records_written;
        if (records_in_page > 2) {
            records_in_page = 2;
        }

        uint32_t address = getRecordAddress(metadata_.write_index);

        // Ensure the target sector is erased
        if (!ensureSectorErased(address)) {
            flushing_ = false;
            return false;
        }

        // Also check if the page spans into the next sector
        uint32_t page_size = records_in_page * sizeof(LogRecord);
        uint32_t page_end = address + page_size - 1;
        if (!ensureSectorErased(page_end)) {
            flushing_ = false;
            return false;
        }

        // Prepare page buffer (pad with 0xFF if only 1 record)
        uint8_t page_data[256];
        memset(page_data, 0xFF, sizeof(page_data));
        memcpy(page_data, &ram_buffer_[records_written], records_in_page * sizeof(LogRecord));

        // Write the full page (256 bytes)
        ExternalFlashResult result = flash_.write(address, page_data, 256);
        if (result != ExternalFlashResult::Success) {
            flushing_ = false;
            return false;
        }

        // Advance write index
        metadata_.write_index = (metadata_.write_index + records_in_page) % MAX_RECORDS;
        metadata_.total_records += records_in_page;
        records_written += records_in_page;
    }

    ram_buffer_count_ = 0;
    flushing_ = false;
    return true;
}

bool LogFlashBuffer::ensureSectorErased(uint32_t address)
{
    uint32_t sector_start = (address / SECTOR_SIZE) * SECTOR_SIZE;

    if (sector_start == last_erased_sector_) {
        return true;  // Already erased this sector
    }

    // Check if sector needs erasing by reading first few bytes
    uint8_t check[4];
    ExternalFlashResult result = flash_.read(sector_start, check, sizeof(check));
    if (result == ExternalFlashResult::Success) {
        bool erased =
            (check[0] == 0xFF && check[1] == 0xFF && check[2] == 0xFF && check[3] == 0xFF);
        if (erased) {
            last_erased_sector_ = sector_start;
            return true;
        }
    }

    result = flash_.eraseSector(sector_start);
    if (result != ExternalFlashResult::Success) {
        // Don't log to flash during flush operation to prevent recursion
        if (!flushing_) {
            logger_.error("Failed to erase log sector 0x%08X", sector_start);
        }
        return false;
    }

    last_erased_sector_ = sector_start;
    return true;
}

bool LogFlashBuffer::readRecord(uint32_t index, LogRecord &record) const
{
    if (!initialized_ || index >= MAX_RECORDS)
        return false;

    uint32_t address = getRecordAddress(index);
    ExternalFlashResult result =
        flash_.read(address, reinterpret_cast<uint8_t *>(&record), sizeof(record));
    if (result != ExternalFlashResult::Success)
        return false;

    // Verify CRC
    uint16_t calculated = CRC16::calculate(reinterpret_cast<const uint8_t *>(&record),
                                           sizeof(LogRecord) - sizeof(record.crc16));
    return (calculated == record.crc16);
}

uint32_t LogFlashBuffer::getStoredCount() const
{
    if (!initialized_)
        return 0;
    if (metadata_.total_records >= MAX_RECORDS)
        return MAX_RECORDS;
    return metadata_.total_records;
}

uint32_t LogFlashBuffer::getOldestIndex() const
{
    if (!initialized_)
        return 0;
    if (metadata_.total_records <= MAX_RECORDS) {
        return 0;  // Haven't wrapped yet
    }
    // Buffer has wrapped - oldest is at write_index (about to be overwritten)
    return metadata_.write_index;
}

bool LogFlashBuffer::flush()
{
    if (!initialized_)
        return false;
    // Flush any buffered records
    if (ram_buffer_count_ > 0) {
        if (!flushPageBuffer())
            return false;
    }
    return saveMetadata();
}

bool LogFlashBuffer::reset()
{
    ExternalFlashResult result = flash_.eraseSector(METADATA_OFFSET);
    if (result != ExternalFlashResult::Success)
        return false;

    initializeMetadata();
    if (!saveMetadata())
        return false;

    ram_buffer_count_ = 0;
    last_erased_sector_ = UINT32_MAX;
    initialized_ = true;
    return true;
}

uint32_t LogFlashBuffer::getRecordAddress(uint32_t index) const
{
    return DATA_START + (index * sizeof(LogRecord));
}

bool LogFlashBuffer::loadMetadata()
{
    ExternalFlashResult result =
        flash_.read(METADATA_OFFSET, reinterpret_cast<uint8_t *>(&metadata_), sizeof(metadata_));
    if (result != ExternalFlashResult::Success)
        return false;
    return validateMetadata();
}

bool LogFlashBuffer::saveMetadata()
{
    metadata_.crc32 = ConfigurationBase::calculateCRC32(
        reinterpret_cast<const uint8_t *>(&metadata_), sizeof(metadata_) - sizeof(metadata_.crc32));

    ExternalFlashResult result = flash_.eraseSector(METADATA_OFFSET);
    if (result != ExternalFlashResult::Success)
        return false;

    result = flash_.write(METADATA_OFFSET, reinterpret_cast<const uint8_t *>(&metadata_),
                          sizeof(metadata_));
    return result == ExternalFlashResult::Success;
}

void LogFlashBuffer::initializeMetadata()
{
    memset(&metadata_, 0, sizeof(metadata_));
    metadata_.magic = LOG_FLASH_MAGIC;
    metadata_.version = LOG_FLASH_VERSION;
}

bool LogFlashBuffer::validateMetadata() const
{
    if (metadata_.magic != LOG_FLASH_MAGIC)
        return false;
    if (metadata_.version != LOG_FLASH_VERSION)
        return false;

    uint32_t calculated = ConfigurationBase::calculateCRC32(
        reinterpret_cast<const uint8_t *>(&metadata_), sizeof(metadata_) - sizeof(metadata_.crc32));
    if (calculated != metadata_.crc32)
        return false;

    if (metadata_.write_index >= MAX_RECORDS)
        return false;
    return true;
}
