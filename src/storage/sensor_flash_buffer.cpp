#include "sensor_flash_buffer.h"

#include <cstring>

#include "../config/config_base.h"

// CRC16-CCITT lookup table (polynomial 0x1021)
const uint16_t CRC16::crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129, 0xA14A, 0xB16B,
    0xC18C, 0xD1AD, 0xE1CE, 0xF1EF, 0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE, 0x2462, 0x3443, 0x0420, 0x1401,
    0x64E6, 0x74C7, 0x44A4, 0x5485, 0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4, 0xB75B, 0xA77A, 0x9719, 0x8738,
    0xF7DF, 0xE7FE, 0xD79D, 0xC7BC, 0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B, 0x5AF5, 0x4AD4, 0x7AB7, 0x6A96,
    0x1A71, 0x0A50, 0x3A33, 0x2A12, 0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41, 0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD,
    0xAD2A, 0xBD0B, 0x8D68, 0x9D49, 0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78, 0x9188, 0x81A9, 0xB1CA, 0xA1EB,
    0xD10C, 0xC12D, 0xF14E, 0xE16F, 0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E, 0x02B1, 0x1290, 0x22F3, 0x32D2,
    0x4235, 0x5214, 0x6277, 0x7256, 0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405, 0xA7DB, 0xB7FA, 0x8799, 0x97B8,
    0xE75F, 0xF77E, 0xC71D, 0xD73C, 0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB, 0x5844, 0x4865, 0x7806, 0x6827,
    0x18C0, 0x08E1, 0x3882, 0x28A3, 0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92, 0xFD2E, 0xED0F, 0xDD6C, 0xCD4D,
    0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9, 0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8, 0x6E17, 0x7E36, 0x4E55, 0x5E74,
    0x2E93, 0x3EB2, 0x0ED1, 0x1EF0};

uint16_t CRC16::calculate(const uint8_t *data, size_t length, uint16_t initial_value)
{
    uint16_t crc = initial_value;
    for (size_t i = 0; i < length; i++) {
        crc = (crc << 8) ^ crc16_table[((crc >> 8) ^ data[i]) & 0xFF];
    }
    return crc;
}

uint16_t CRC16::calculateRecordCRC(const SensorDataRecord &record)
{
    // Calculate CRC over everything except the CRC field itself
    return calculate(reinterpret_cast<const uint8_t *>(&record),
                     sizeof(SensorDataRecord) - sizeof(record.crc16));
}

// SensorFlashBuffer implementation

SensorFlashBuffer::SensorFlashBuffer(ExternalFlash &flash)
    : flash_(flash), initialized_(false), logger_("SensorFlashBuffer")
{
    memset(&metadata_, 0, sizeof(metadata_));
}

bool SensorFlashBuffer::init()
{
    if (!flash_.isInitialized()) {
        logger_.error("External flash not initialized");
        return false;
    }

    // Try to load existing metadata
    if (loadMetadata()) {
        logger_.debug("Loaded existing metadata: %lu records (%lu transmitted, %lu lost)",
                      metadata_.total_records, metadata_.records_transmitted,
                      metadata_.records_lost);
        initialized_ = true;
        return true;
    }

    // No valid metadata found - initialize fresh
    logger_.warn("No valid metadata found, initializing fresh buffer");
    initializeMetadata();

    if (!saveMetadata()) {
        logger_.error("Failed to save initial metadata");
        return false;
    }

    initialized_ = true;
    logger_.info("Initialized fresh sensor flash buffer");
    return true;
}

bool SensorFlashBuffer::writeRecord(const SensorDataRecord &record)
{
    if (!initialized_) {
        logger_.error("Buffer not initialized");
        return false;
    }

    // Create record with CRC
    SensorDataRecord record_with_crc = record;
    record_with_crc.flags |= RECORD_FLAG_VALID;  // Mark as valid
    record_with_crc.crc16 = CRC16::calculateRecordCRC(record_with_crc);

    // Calculate flash address
    uint32_t address = getRecordAddress(metadata_.write_index);

    // Check if we need to erase the sector(s) this record will occupy
    // For MT25QL, we need to erase before writing (NOR flash can only flip 1s to 0s)
    //
    // Records (12 bytes) don't align with sector boundaries (4096 bytes), so we must
    // erase a sector when we're about to write the first byte into it, not just when
    // a record starts at offset 0.
    uint32_t sector_start = (address / SECTOR_SIZE) * SECTOR_SIZE;
    uint32_t record_end = address + sizeof(SensorDataRecord) - 1;
    uint32_t end_sector_start = (record_end / SECTOR_SIZE) * SECTOR_SIZE;

    // Determine if we're entering a new sector by checking where the previous record ended
    uint32_t prev_record_end_sector;
    if (metadata_.write_index == 0) {
        // First record ever - must erase the first data sector
        prev_record_end_sector = DATA_START_OFFSET - 1;  // Force sector mismatch
    } else {
        uint32_t prev_record_end =
            getRecordAddress(metadata_.write_index - 1) + sizeof(SensorDataRecord) - 1;
        prev_record_end_sector = (prev_record_end / SECTOR_SIZE) * SECTOR_SIZE;
    }

    // Erase the sector if this record starts in a different sector than where previous record ended
    bool need_erase_start_sector = (sector_start != prev_record_end_sector);

    // If record spans two sectors, also erase the second sector
    bool need_erase_end_sector =
        (end_sector_start != sector_start) && (end_sector_start != prev_record_end_sector);

    if (need_erase_start_sector) {
        logger_.info("Erasing sector 0x%08X for write_index %lu", sector_start,
                     metadata_.write_index);
        ExternalFlashResult result = flash_.eraseSector(sector_start);
        if (result != ExternalFlashResult::Success) {
            logger_.error("Failed to erase sector at 0x%08X", sector_start);
            return false;
        }
    }

    if (need_erase_end_sector) {
        logger_.info("Erasing end sector 0x%08X", end_sector_start);
        ExternalFlashResult result = flash_.eraseSector(end_sector_start);
        if (result != ExternalFlashResult::Success) {
            logger_.error("Failed to erase sector at 0x%08X", end_sector_start);
            return false;
        }
    }

    // Write the record
    logger_.debug("Writing record at index %lu, addr 0x%08X, CRC=0x%04X", metadata_.write_index,
                  address, record_with_crc.crc16);
    ExternalFlashResult result = flash_.write(
        address, reinterpret_cast<const uint8_t *>(&record_with_crc), sizeof(record_with_crc));
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to write record at index %lu", metadata_.write_index);
        return false;
    }

    // Verify write by reading back and checking CRC
    SensorDataRecord verify_record;
    result =
        flash_.read(address, reinterpret_cast<uint8_t *>(&verify_record), sizeof(verify_record));
    if (result == ExternalFlashResult::Success) {
        uint16_t verify_crc = CRC16::calculateRecordCRC(verify_record);
        if (verify_crc != verify_record.crc16) {
            logger_.error("Write verify FAILED at index %lu: expected CRC 0x%04X, got 0x%04X",
                          metadata_.write_index, verify_record.crc16, verify_crc);
        }
    }

    // Check if we're about to overwrite an untransmitted record
    if (isFull()) {
        metadata_.records_lost++;
        // Advance read index to skip the record we're about to overwrite
        metadata_.read_index = (metadata_.read_index + 1) % MAX_RECORDS;
    }

    // Update metadata
    metadata_.write_index = (metadata_.write_index + 1) % MAX_RECORDS;
    metadata_.total_records++;

    // Save metadata periodically (every 10 records to reduce flash wear)
    if (metadata_.total_records % 10 == 0) {
        if (!saveMetadata()) {
            logger_.warn("Failed to save metadata after write");
            // Continue anyway - metadata will be saved later
        }
    }

    return true;
}

bool SensorFlashBuffer::markTransmitted(uint32_t index)
{
    if (!initialized_) {
        logger_.error("Buffer not initialized");
        return false;
    }

    if (index >= MAX_RECORDS) {
        logger_.error("Invalid record index: %lu", index);
        return false;
    }

    // Read the record
    uint32_t address = getRecordAddress(index);
    SensorDataRecord record;

    ExternalFlashResult result =
        flash_.read(address, reinterpret_cast<uint8_t *>(&record), sizeof(record));
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to read record at index %lu", index);
        return false;
    }

    // Verify CRC
    uint16_t calculated_crc = CRC16::calculateRecordCRC(record);
    if (calculated_crc != record.crc16) {
        logger_.warn("CRC mismatch at index %lu", index);
        return false;
    }

    // Check if already marked
    if (record.flags & RECORD_FLAG_TRANSMITTED) {
        return true;  // Already marked, no need to update
    }

    // Mark as transmitted
    record.flags |= RECORD_FLAG_TRANSMITTED;
    record.crc16 = CRC16::calculateRecordCRC(record);

    // Read-modify-write pattern to preserve all records in the sector
    // Flash requires erase before write, so we must save all records
    uint32_t sector_start = (address / SECTOR_SIZE) * SECTOR_SIZE;
    uint32_t offset_in_sector = address - sector_start;

    // Step 1: Read entire sector into RAM buffer
    result = flash_.read(sector_start, sector_buffer_, SECTOR_SIZE);
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to read sector for mark transmitted");
        return false;
    }

    // Step 2: Modify the target record in the buffer
    memcpy(sector_buffer_ + offset_in_sector, &record, sizeof(record));

    // Step 3: Erase the sector
    result = flash_.eraseSector(sector_start);
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to erase sector for mark");
        return false;
    }

    // Step 4: Write entire sector back
    result = flash_.write(sector_start, sector_buffer_, SECTOR_SIZE);
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to write sector after mark");
        return false;
    }

    metadata_.records_transmitted++;
    return saveMetadata();
}

bool SensorFlashBuffer::readUntransmittedRecords(SensorDataRecord *records, size_t max_count,
                                                 size_t &actual_count, size_t &records_scanned)
{
    if (!initialized_) {
        logger_.error("Buffer not initialized");
        return false;
    }

    actual_count = 0;
    records_scanned = 0;
    uint32_t current_index = metadata_.read_index;
    size_t crc_errors = 0;
    bool had_read_failure = false;

    while (actual_count < max_count && current_index != metadata_.write_index) {
        uint32_t address = getRecordAddress(current_index);
        SensorDataRecord record;

        // Retry read up to 3 times on failure (transient flash issues)
        ExternalFlashResult result = ExternalFlashResult::ErrorHardware;
        for (int retry = 0; retry < 3; retry++) {
            result = flash_.read(address, reinterpret_cast<uint8_t *>(&record), sizeof(record));
            if (result == ExternalFlashResult::Success) {
                break;
            }
            if (retry < 2) {
                logger_.warn("Read retry %d for index %lu", retry + 1, current_index);
            }
        }

        if (result != ExternalFlashResult::Success) {
            // Persistent read failure after retries - stop here but don't fail
            // The caller should only skip records_scanned, not the entire backlog
            logger_.error("Failed to read record at index %lu after 3 retries", current_index);
            had_read_failure = true;
            break;
        }

        records_scanned++;

        // Verify CRC
        uint16_t calculated_crc = CRC16::calculateRecordCRC(record);
        if (calculated_crc != record.crc16) {
            logger_.warn("CRC mismatch at index %lu (expected 0x%04X, got 0x%04X), skipping",
                         current_index, record.crc16, calculated_crc);
            crc_errors++;
            current_index = (current_index + 1) % MAX_RECORDS;
            continue;
        }

        // Check if valid (we no longer filter by transmitted flag -
        // read_index tracks what's been sent, timestamps allow deduplication)
        if (record.flags & RECORD_FLAG_VALID) {
            records[actual_count++] = record;
        }

        current_index = (current_index + 1) % MAX_RECORDS;
    }

    // NOTE: We intentionally do NOT update read_index here.
    // The caller must call advanceReadIndex() after successful transmission
    // is confirmed via ACK callback. This prevents data loss if transmission fails.

    if (crc_errors > 0) {
        logger_.warn("Encountered %zu records with CRC errors", crc_errors);
    }

    if (had_read_failure) {
        logger_.warn("Read stopped early due to flash failure - %zu records scanned",
                     records_scanned);
    }

    // Return false only if we couldn't read anything (not initialized or immediate failure)
    // Return true if we scanned any records, even if all had CRC errors
    // The caller uses records_scanned to know how many to skip
    return true;
}

bool SensorFlashBuffer::getStatistics(SensorFlashMetadata &stats) const
{
    if (!initialized_) {
        return false;
    }
    stats = metadata_;
    return true;
}

uint32_t SensorFlashBuffer::getUntransmittedCount() const
{
    if (!initialized_) {
        return 0;
    }

    if (metadata_.write_index >= metadata_.read_index) {
        return metadata_.write_index - metadata_.read_index;
    } else {
        return MAX_RECORDS - metadata_.read_index + metadata_.write_index;
    }
}

bool SensorFlashBuffer::advanceReadIndex(uint32_t count)
{
    if (!initialized_) {
        logger_.error("Buffer not initialized");
        return false;
    }

    if (count == 0) {
        return true;  // Nothing to advance
    }

    // Calculate new read index with wraparound
    uint32_t new_read_index = (metadata_.read_index + count) % MAX_RECORDS;

    // Sanity check: don't advance past write index
    uint32_t available = getUntransmittedCount();
    if (count > available) {
        logger_.error("Cannot advance read_index by %lu, only %lu records available", count,
                      available);
        return false;
    }

    metadata_.read_index = new_read_index;
    logger_.debug("Advanced read_index by %lu to %lu", count, new_read_index);

    return saveMetadata();
}

bool SensorFlashBuffer::updateLastSync(uint32_t timestamp)
{
    if (!initialized_) {
        return false;
    }

    metadata_.last_sync_timestamp = timestamp;
    return saveMetadata();
}

uint32_t SensorFlashBuffer::getInitialBootTimestamp() const
{
    if (!initialized_) {
        return 0;
    }
    return metadata_.initial_boot_timestamp;
}

bool SensorFlashBuffer::setInitialBootTimestamp(uint32_t timestamp)
{
    if (!initialized_) {
        return false;
    }

    // Only set if not already set (non-zero)
    if (metadata_.initial_boot_timestamp != 0) {
        logger_.debug("Initial boot timestamp already set to %lu, not overwriting",
                      metadata_.initial_boot_timestamp);
        return false;
    }

    metadata_.initial_boot_timestamp = timestamp;
    logger_.info("Set initial boot timestamp to %lu", timestamp);
    return saveMetadata();
}

bool SensorFlashBuffer::flush()
{
    if (!initialized_) {
        return false;
    }
    return saveMetadata();
}

bool SensorFlashBuffer::reset()
{
    logger_.warn("Resetting sensor flash buffer - all data will be lost!");

    // Erase metadata sector
    ExternalFlashResult result = flash_.eraseSector(METADATA_SECTOR);
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to erase metadata sector");
        return false;
    }

    // Initialize fresh metadata
    initializeMetadata();

    if (!saveMetadata()) {
        logger_.error("Failed to save fresh metadata");
        return false;
    }

    initialized_ = true;
    logger_.info("Flash buffer reset complete");
    return true;
}

// Private methods

bool SensorFlashBuffer::loadMetadata()
{
    ExternalFlashResult result =
        flash_.read(METADATA_SECTOR, reinterpret_cast<uint8_t *>(&metadata_), sizeof(metadata_));
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to read metadata");
        return false;
    }

    if (!validateMetadata()) {
        return false;
    }

    return true;
}

bool SensorFlashBuffer::saveMetadata()
{
    // Calculate CRC
    metadata_.crc32 = ConfigurationBase::calculateCRC32(
        reinterpret_cast<const uint8_t *>(&metadata_), sizeof(metadata_) - sizeof(metadata_.crc32));

    // Erase metadata sector
    ExternalFlashResult result = flash_.eraseSector(METADATA_SECTOR);
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to erase metadata sector");
        return false;
    }

    // Write metadata
    result = flash_.write(METADATA_SECTOR, reinterpret_cast<const uint8_t *>(&metadata_),
                          sizeof(metadata_));
    if (result != ExternalFlashResult::Success) {
        logger_.error("Failed to write metadata");
        return false;
    }

    return true;
}

void SensorFlashBuffer::initializeMetadata()
{
    memset(&metadata_, 0, sizeof(metadata_));
    metadata_.magic = SENSOR_FLASH_MAGIC;
    metadata_.version = SENSOR_FLASH_VERSION;
    metadata_.write_index = 0;
    metadata_.read_index = 0;
    metadata_.total_records = 0;
    metadata_.records_transmitted = 0;
    metadata_.records_lost = 0;
    metadata_.last_sync_timestamp = 0;
    metadata_.initial_boot_timestamp = 0;
    metadata_.next_seq_num = 0;
}

uint32_t SensorFlashBuffer::getRecordAddress(uint32_t index) const
{
    return DATA_START_OFFSET + (index * sizeof(SensorDataRecord));
}

uint8_t SensorFlashBuffer::getNextSeqNum() const
{
    return metadata_.next_seq_num;
}

void SensorFlashBuffer::saveNextSeqNum(uint8_t seq_num)
{
    metadata_.next_seq_num = seq_num;
}

bool SensorFlashBuffer::validateMetadata() const
{
    // Check magic number
    if (metadata_.magic != SENSOR_FLASH_MAGIC) {
        logger_.warn("Invalid magic number: 0x%08X", metadata_.magic);
        return false;
    }

    // Check version
    if (metadata_.version != SENSOR_FLASH_VERSION) {
        logger_.warn("Unsupported version: %lu", metadata_.version);
        return false;
    }

    // Verify CRC
    uint32_t calculated_crc = ConfigurationBase::calculateCRC32(
        reinterpret_cast<const uint8_t *>(&metadata_), sizeof(metadata_) - sizeof(metadata_.crc32));

    if (calculated_crc != metadata_.crc32) {
        logger_.warn("CRC mismatch: expected 0x%08X, got 0x%08X", metadata_.crc32, calculated_crc);
        return false;
    }

    // Validate indices
    if (metadata_.write_index >= MAX_RECORDS || metadata_.read_index >= MAX_RECORDS) {
        logger_.warn("Invalid indices: write=%lu, read=%lu", metadata_.write_index,
                     metadata_.read_index);
        return false;
    }

    return true;
}

bool SensorFlashBuffer::isFull() const
{
    return ((metadata_.write_index + 1) % MAX_RECORDS) == metadata_.read_index;
}
