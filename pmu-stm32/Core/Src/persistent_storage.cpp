#include "persistent_storage.h"

#include <cstring>

PersistentStorage::PersistentStorage(FRAM& fram) : fram_(fram), available_(false) {}

bool PersistentStorage::init()
{
    if (!fram_.isPresent()) {
        available_ = false;
        return false;
    }

    // Read magic number
    uint32_t magic = 0;
    if (!fram_.read(OFFSET_MAGIC, reinterpret_cast<uint8_t*>(&magic), sizeof(magic))) {
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
        if (!fram_.read(OFFSET_VERSION, reinterpret_cast<uint8_t*>(&version), sizeof(version))) {
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

    available_ = true;
    return true;
}

bool PersistentStorage::formatStorage()
{
    // Zero out the used area
    uint16_t usedSize = OFFSET_NODE_STATE_FLAG + sizeof(uint32_t);
    // Write in chunks to avoid large stack allocation
    uint8_t zeros[64] = {};
    for (uint16_t offset = 0; offset < usedSize; offset += sizeof(zeros)) {
        uint16_t chunk = usedSize - offset;
        if (chunk > sizeof(zeros)) chunk = sizeof(zeros);
        if (!fram_.write(offset, zeros, chunk)) {
            return false;
        }
    }

    // Write magic and version
    uint32_t magic = MAGIC;
    uint32_t version = FORMAT_VERSION;
    if (!fram_.write(OFFSET_MAGIC, reinterpret_cast<uint8_t*>(&magic), sizeof(magic))) {
        return false;
    }
    if (!fram_.write(OFFSET_VERSION, reinterpret_cast<uint8_t*>(&version), sizeof(version))) {
        return false;
    }

    // Default wake interval: 60 seconds (matches Protocol constructor default)
    uint32_t defaultInterval = 60;
    if (!fram_.write(OFFSET_WAKE_INTERVAL, reinterpret_cast<uint8_t*>(&defaultInterval),
                     sizeof(defaultInterval))) {
        return false;
    }

    return true;
}

// --- Wake interval ---

bool PersistentStorage::loadWakeInterval(uint32_t& seconds)
{
    if (!available_) return false;
    return fram_.read(OFFSET_WAKE_INTERVAL, reinterpret_cast<uint8_t*>(&seconds), sizeof(seconds));
}

bool PersistentStorage::saveWakeInterval(uint32_t seconds)
{
    if (!available_) return false;
    return fram_.write(OFFSET_WAKE_INTERVAL, reinterpret_cast<const uint8_t*>(&seconds),
                       sizeof(seconds));
}

// --- Schedules (per-entry access) ---

uint8_t PersistentStorage::getScheduleCount()
{
    if (!available_) return 0;

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
    if (!available_) return false;
    if (count > PMU::MAX_SCHEDULE_ENTRIES) return false;
    return fram_.write(OFFSET_SCHEDULE_COUNT, &count, 1);
}

bool PersistentStorage::loadScheduleEntry(uint8_t index, PMU::ScheduleEntry& entry)
{
    if (!available_ || index >= PMU::MAX_SCHEDULE_ENTRIES) return false;

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

bool PersistentStorage::saveScheduleEntry(uint8_t index, const PMU::ScheduleEntry& entry)
{
    if (!available_ || index >= PMU::MAX_SCHEDULE_ENTRIES) return false;

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

bool PersistentStorage::loadNodeState(uint8_t* state, uint8_t length)
{
    if (!available_) return false;
    if (length > PMU::NODE_STATE_SIZE) return false;
    return fram_.read(OFFSET_NODE_STATE, state, length);
}

bool PersistentStorage::saveNodeState(const uint8_t* state, uint8_t length)
{
    if (!available_) return false;
    if (length > PMU::NODE_STATE_SIZE) return false;

    if (!fram_.write(OFFSET_NODE_STATE, state, length)) {
        return false;
    }

    uint32_t validFlag = 1;
    return fram_.write(OFFSET_NODE_STATE_FLAG, reinterpret_cast<uint8_t*>(&validFlag),
                       sizeof(validFlag));
}

bool PersistentStorage::isNodeStateValid()
{
    if (!available_) return false;

    uint32_t flag = 0;
    if (!fram_.read(OFFSET_NODE_STATE_FLAG, reinterpret_cast<uint8_t*>(&flag), sizeof(flag))) {
        return false;
    }
    return flag == 1;
}

bool PersistentStorage::factoryReset()
{
    if (!available_) return false;
    return formatStorage();
}

bool PersistentStorage::invalidateNodeState()
{
    if (!available_) return false;

    uint32_t flag = 0;
    return fram_.write(OFFSET_NODE_STATE_FLAG, reinterpret_cast<uint8_t*>(&flag), sizeof(flag));
}
