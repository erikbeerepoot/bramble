#include "persistent_storage.h"

#include <cstring>

PersistentStorage::PersistentStorage(FRAM &fram) : fram_(fram), available_(false) {}

bool PersistentStorage::init()
{
    if (!fram_.isPresent()) {
        available_ = false;
        return false;
    }

    // Read magic number
    uint32_t magic = 0;
    if (!fram_.read(OFFSET_MAGIC, reinterpret_cast<uint8_t *>(&magic), sizeof(magic))) {
        available_ = false;
        return false;
    }

    if (magic != MAGIC) {
        if (!formatStorage()) {
            available_ = false;
            return false;
        }
    } else {
        uint32_t version = 0;
        if (!fram_.read(OFFSET_VERSION, reinterpret_cast<uint8_t *>(&version), sizeof(version))) {
            available_ = false;
            return false;
        }
        if (version != FORMAT_VERSION) {
            if (!formatStorage()) {
                available_ = false;
                return false;
            }
        }
    }

    // Initialize blob directory (migrates node state on first run)
    if (!initBlobDirectory()) {
        available_ = false;
        return false;
    }

    available_ = true;
    return true;
}

bool PersistentStorage::formatStorage()
{
    // Zero out the entire used area (including blob region)
    uint16_t usedSize = TOTAL_USED;
    // Write in chunks to avoid large stack allocation
    uint8_t zeros[64] = {};
    for (uint16_t offset = 0; offset < usedSize; offset += sizeof(zeros)) {
        uint16_t chunk = usedSize - offset;
        if (chunk > sizeof(zeros))
            chunk = sizeof(zeros);
        if (!fram_.write(offset, zeros, chunk)) {
            return false;
        }
    }

    // Write magic and version
    uint32_t magic = MAGIC;
    uint32_t version = FORMAT_VERSION;
    if (!fram_.write(OFFSET_MAGIC, reinterpret_cast<uint8_t *>(&magic), sizeof(magic))) {
        return false;
    }
    if (!fram_.write(OFFSET_VERSION, reinterpret_cast<uint8_t *>(&version), sizeof(version))) {
        return false;
    }

    // Default wake interval: 60 seconds (matches Protocol constructor default)
    uint32_t defaultInterval = 60;
    if (!fram_.write(OFFSET_WAKE_INTERVAL, reinterpret_cast<uint8_t *>(&defaultInterval),
                     sizeof(defaultInterval))) {
        return false;
    }

    return true;
}

// --- Wake interval ---

bool PersistentStorage::loadWakeInterval(uint32_t &seconds)
{
    if (!available_)
        return false;
    return fram_.read(OFFSET_WAKE_INTERVAL, reinterpret_cast<uint8_t *>(&seconds), sizeof(seconds));
}

bool PersistentStorage::saveWakeInterval(uint32_t seconds)
{
    if (!available_)
        return false;
    return fram_.write(OFFSET_WAKE_INTERVAL, reinterpret_cast<const uint8_t *>(&seconds),
                       sizeof(seconds));
}

// --- Schedules (per-entry access) ---

uint8_t PersistentStorage::getScheduleCount()
{
    if (!available_)
        return 0;

    uint8_t count = 0;
    if (!fram_.read(OFFSET_SCHEDULE_COUNT, &count, 1)) {
        return 0;
    }
    if (count > PMU::MAX_SCHEDULE_ENTRIES) {
        return 0;  // Corrupt — treat as empty
    }
    return count;
}

bool PersistentStorage::setScheduleCount(uint8_t count)
{
    if (!available_)
        return false;
    if (count > PMU::MAX_SCHEDULE_ENTRIES)
        return false;
    return fram_.write(OFFSET_SCHEDULE_COUNT, &count, 1);
}

bool PersistentStorage::loadScheduleEntry(uint8_t index, PMU::ScheduleEntry &entry)
{
    if (!available_ || index >= PMU::MAX_SCHEDULE_ENTRIES)
        return false;

    uint8_t raw[PMU::SCHEDULE_ENTRY_SIZE];
    if (!fram_.read(scheduleEntryOffset(index), raw, PMU::SCHEDULE_ENTRY_SIZE)) {
        return false;
    }

    entry.hour = raw[0];
    entry.minute = raw[1];
    entry.duration = raw[2] | (raw[3] << 8);
    entry.daysMask = static_cast<PMU::DayOfWeek>(raw[4]);
    entry.valveId = raw[5];
    entry.enabled = (raw[6] != 0);
    return true;
}

bool PersistentStorage::saveScheduleEntry(uint8_t index, const PMU::ScheduleEntry &entry)
{
    if (!available_ || index >= PMU::MAX_SCHEDULE_ENTRIES)
        return false;

    uint8_t raw[PMU::SCHEDULE_ENTRY_SIZE];
    raw[0] = entry.hour;
    raw[1] = entry.minute;
    raw[2] = entry.duration & 0xFF;
    raw[3] = (entry.duration >> 8) & 0xFF;
    raw[4] = static_cast<uint8_t>(entry.daysMask);
    raw[5] = entry.valveId;
    raw[6] = entry.enabled ? 1 : 0;

    return fram_.write(scheduleEntryOffset(index), raw, PMU::SCHEDULE_ENTRY_SIZE);
}

// --- Node state ---

bool PersistentStorage::loadNodeState(uint8_t *state, uint8_t length)
{
    if (!available_)
        return false;
    if (length > PMU::NODE_STATE_SIZE)
        return false;
    return fram_.read(OFFSET_NODE_STATE, state, length);
}

bool PersistentStorage::saveNodeState(const uint8_t *state, uint8_t length)
{
    if (!available_)
        return false;
    if (length > PMU::NODE_STATE_SIZE)
        return false;

    if (!fram_.write(OFFSET_NODE_STATE, state, length)) {
        return false;
    }

    uint32_t validFlag = 1;
    return fram_.write(OFFSET_NODE_STATE_FLAG, reinterpret_cast<uint8_t *>(&validFlag),
                       sizeof(validFlag));
}

bool PersistentStorage::isNodeStateValid()
{
    if (!available_)
        return false;

    uint32_t flag = 0;
    if (!fram_.read(OFFSET_NODE_STATE_FLAG, reinterpret_cast<uint8_t *>(&flag), sizeof(flag))) {
        return false;
    }
    return flag == 1;
}

bool PersistentStorage::factoryReset()
{
    if (!available_)
        return false;
    return formatStorage();
}

bool PersistentStorage::invalidateNodeState()
{
    if (!available_)
        return false;

    uint32_t flag = 0;
    return fram_.write(OFFSET_NODE_STATE_FLAG, reinterpret_cast<uint8_t *>(&flag), sizeof(flag));
}

// --- Blob storage ---

bool PersistentStorage::initBlobDirectory()
{
    // Check if blob directory is already initialized
    uint16_t magic = 0;
    if (!fram_.read(OFFSET_BLOB_DIR, reinterpret_cast<uint8_t *>(&magic), sizeof(magic))) {
        return false;
    }

    if (magic == BLOB_MAGIC) {
        return true;  // Already initialized
    }

    // First time: write blob directory header
    magic = BLOB_MAGIC;
    if (!fram_.write(OFFSET_BLOB_DIR, reinterpret_cast<const uint8_t *>(&magic), sizeof(magic))) {
        return false;
    }
    uint8_t slot_count = 2;
    if (!fram_.write(OFFSET_BLOB_DIR + 2, &slot_count, 1)) {
        return false;
    }
    uint8_t reserved = 0;
    if (!fram_.write(OFFSET_BLOB_DIR + 3, &reserved, 1)) {
        return false;
    }

    // Write slot 0 descriptor (Node State)
    uint16_t desc0[3] = {BLOB_SLOT0_DATA_OFFSET, BLOB_SLOT0_CAPACITY, 0};
    if (!fram_.write(blobSlotDescOffset(0), reinterpret_cast<uint8_t *>(desc0),
                     BLOB_SLOT_DESC_SIZE)) {
        return false;
    }

    // Write slot 1 descriptor (Event Log)
    uint16_t desc1[3] = {BLOB_SLOT1_DATA_OFFSET, BLOB_SLOT1_CAPACITY, 0};
    if (!fram_.write(blobSlotDescOffset(1), reinterpret_cast<uint8_t *>(desc1),
                     BLOB_SLOT_DESC_SIZE)) {
        return false;
    }

    // Migrate existing node state from legacy location to blob slot 0
    if (isNodeStateValid()) {
        uint8_t state[PMU::NODE_STATE_SIZE];
        if (fram_.read(OFFSET_NODE_STATE, state, PMU::NODE_STATE_SIZE)) {
            // Write to blob slot 0 data region
            if (fram_.write(BLOB_SLOT0_DATA_OFFSET, state, PMU::NODE_STATE_SIZE)) {
                // Update slot 0 used_length
                uint16_t used = PMU::NODE_STATE_SIZE;
                fram_.write(blobSlotDescOffset(0) + 4, reinterpret_cast<uint8_t *>(&used),
                            sizeof(used));
            }
        }
    }

    return true;
}

bool PersistentStorage::getBlobSlotInfo(uint8_t slot, uint16_t &dataOffset,
                                        uint16_t &capacity) const
{
    if (slot >= MAX_BLOB_SLOTS)
        return false;

    uint16_t desc[3];
    if (!fram_.read(blobSlotDescOffset(slot), reinterpret_cast<uint8_t *>(desc),
                    BLOB_SLOT_DESC_SIZE)) {
        return false;
    }
    dataOffset = desc[0];
    capacity = desc[1];
    return true;
}

bool PersistentStorage::saveBlobChunk(uint8_t slot, uint16_t offset, const uint8_t *data,
                                      uint8_t length)
{
    if (!available_ || slot >= MAX_BLOB_SLOTS)
        return false;

    uint16_t dataOffset, capacity;
    if (!getBlobSlotInfo(slot, dataOffset, capacity))
        return false;
    if (offset + length > capacity)
        return false;

    return fram_.write(dataOffset + offset, data, length);
}

bool PersistentStorage::setBlobLength(uint8_t slot, uint16_t length)
{
    if (!available_ || slot >= MAX_BLOB_SLOTS)
        return false;

    uint16_t dataOffset, capacity;
    if (!getBlobSlotInfo(slot, dataOffset, capacity))
        return false;
    if (length > capacity)
        return false;

    return fram_.write(blobSlotDescOffset(slot) + 4, reinterpret_cast<uint8_t *>(&length),
                       sizeof(length));
}

uint16_t PersistentStorage::getBlobLength(uint8_t slot)
{
    if (!available_ || slot >= MAX_BLOB_SLOTS)
        return 0;

    uint16_t used = 0;
    fram_.read(blobSlotDescOffset(slot) + 4, reinterpret_cast<uint8_t *>(&used), sizeof(used));
    return used;
}

uint8_t PersistentStorage::loadBlobChunk(uint8_t slot, uint16_t offset, uint8_t *data,
                                         uint8_t maxLength)
{
    if (!available_ || slot >= MAX_BLOB_SLOTS)
        return 0;

    uint16_t dataOffset, capacity;
    if (!getBlobSlotInfo(slot, dataOffset, capacity))
        return 0;

    uint16_t used = getBlobLength(slot);
    if (offset >= used)
        return 0;

    uint8_t toRead = maxLength;
    if (offset + toRead > used) {
        toRead = static_cast<uint8_t>(used - offset);
    }

    if (!fram_.read(dataOffset + offset, data, toRead))
        return 0;
    return toRead;
}

bool PersistentStorage::clearBlob(uint8_t slot)
{
    return setBlobLength(slot, 0);
}
