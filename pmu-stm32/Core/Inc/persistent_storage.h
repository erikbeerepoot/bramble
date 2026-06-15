#ifndef PERSISTENT_STORAGE_H
#define PERSISTENT_STORAGE_H

#include <cstdint>

#include "fram.h"
#include "pmu_protocol.h"

// Persistent storage layer on top of FRAM.
// Manages a memory map with magic/version header and typed accessors
// for wake interval, watering schedules, and node state.
//
// If FRAM is not present, all operations are no-ops that return false,
// allowing the system to fall back to RAM-only behavior.
class PersistentStorage {
public:
    explicit PersistentStorage(FRAM &fram);

    // Check magic number and format version. Formats storage if uninitialised.
    // Returns true if storage is usable (FRAM present and formatted).
    bool init();

    bool isAvailable() const { return available_; }

    // Wake interval
    bool loadWakeInterval(uint32_t &seconds);
    bool saveWakeInterval(uint32_t seconds);

    // Watering schedules — per-entry access (no bulk load needed)
    uint8_t getScheduleCount();
    bool loadScheduleEntry(uint8_t index, PMU::ScheduleEntry &entry);
    bool saveScheduleEntry(uint8_t index, const PMU::ScheduleEntry &entry);
    bool setScheduleCount(uint8_t count);

    // Opaque node state blob (legacy — prefer blob slot API below)
    bool loadNodeState(uint8_t *state, uint8_t length);
    bool saveNodeState(const uint8_t *state, uint8_t length);
    bool isNodeStateValid();
    bool invalidateNodeState();

    // --- Generic blob storage ---
    // Slots are fixed-capacity FRAM regions identified by slot ID.
    // The STM32 treats blob data as opaque bytes.
    static constexpr uint8_t MAX_BLOB_SLOTS = 4;
    static constexpr uint8_t BLOB_SLOT_NODE_STATE = 0;
    static constexpr uint8_t BLOB_SLOT_EVENT_LOG = 1;

    // Write a chunk of data into a blob slot at the given offset.
    bool saveBlobChunk(uint8_t slot, uint16_t offset, const uint8_t *data, uint8_t length);

    // Set the used length of a blob slot (called on first chunk).
    bool setBlobLength(uint8_t slot, uint16_t length);

    // Get the used length of a blob slot (0 = empty).
    uint16_t getBlobLength(uint8_t slot);

    // Read a chunk of data from a blob slot at the given offset.
    // Returns number of bytes actually read.
    uint8_t loadBlobChunk(uint8_t slot, uint16_t offset, uint8_t *data, uint8_t maxLength);

    // Clear a blob slot (sets used_length to 0).
    bool clearBlob(uint8_t slot);

    // Wipe all persistent state (schedules, wake interval, node state, blobs) and re-initialise
    // with fresh magic/version. Returns true on success.
    bool factoryReset();

private:
    FRAM &fram_;
    bool available_;

    bool formatStorage();
    bool initBlobDirectory();

    // Memory map — offsets derived from MAX_SCHEDULE_ENTRIES so they
    // adjust automatically if the schedule capacity changes.
    //
    // Layout (legacy region, preserved for backward compat):
    //   [0x0000] Magic (4)
    //   [0x0004] Version (4)
    //   [0x0008] Wake interval (4)
    //   [0x000C] Schedule count (1)
    //   [0x000D] Reserved (3)
    //   [0x0010] Schedule entries (SCHEDULE_ENTRY_SIZE × MAX_SCHEDULE_ENTRIES)
    //   [...]    Node state (NODE_STATE_SIZE) — legacy location, migrated to blob slot 0
    //   [...]    Node state valid flag (4)
    //
    // Blob directory (appended after legacy region):
    //   [0x006C] Blob magic (2) — 0x424C ("BL")
    //   [0x006E] Slot count (1)
    //   [0x006F] Reserved (1)
    //   [0x0070] Slot descriptors (6 bytes each × MAX_BLOB_SLOTS)
    //   [0x0088] Slot 0 data: Node State (capacity 48)
    //   [0x00B8] Slot 1 data: Event Log (capacity 140)
    static constexpr uint16_t OFFSET_MAGIC = 0x0000;
    static constexpr uint16_t OFFSET_VERSION = 0x0004;
    static constexpr uint16_t OFFSET_WAKE_INTERVAL = 0x0008;
    static constexpr uint16_t OFFSET_SCHEDULE_COUNT = 0x000C;
    static constexpr uint16_t OFFSET_SCHEDULES = 0x0010;
    static constexpr uint16_t OFFSET_NODE_STATE =
        OFFSET_SCHEDULES + PMU::SCHEDULE_ENTRY_SIZE * PMU::MAX_SCHEDULE_ENTRIES;
    static constexpr uint16_t OFFSET_NODE_STATE_FLAG = OFFSET_NODE_STATE + PMU::NODE_STATE_SIZE;
    static constexpr uint16_t LEGACY_TOTAL_USED = OFFSET_NODE_STATE_FLAG + sizeof(uint32_t);

    // Blob directory layout
    static constexpr uint16_t OFFSET_BLOB_DIR = LEGACY_TOTAL_USED;  // 0x006C
    static constexpr uint16_t BLOB_MAGIC = 0x424C;                  // "BL"
    static constexpr uint8_t BLOB_DIR_HEADER_SIZE = 4;  // magic(2) + count(1) + reserved(1)
    static constexpr uint8_t BLOB_SLOT_DESC_SIZE = 6;   // offset(2) + capacity(2) + used_length(2)
    static constexpr uint16_t OFFSET_BLOB_SLOTS = OFFSET_BLOB_DIR + BLOB_DIR_HEADER_SIZE;
    static constexpr uint16_t OFFSET_BLOB_DATA_START =
        OFFSET_BLOB_SLOTS + BLOB_SLOT_DESC_SIZE * MAX_BLOB_SLOTS;  // 0x0088

    // Slot 0: Node State
    static constexpr uint16_t BLOB_SLOT0_CAPACITY = 48;
    static constexpr uint16_t BLOB_SLOT0_DATA_OFFSET = OFFSET_BLOB_DATA_START;  // 0x0088

    // Slot 1: Event Log
    static constexpr uint16_t BLOB_SLOT1_CAPACITY = 140;
    static constexpr uint16_t BLOB_SLOT1_DATA_OFFSET =
        BLOB_SLOT0_DATA_OFFSET + BLOB_SLOT0_CAPACITY;  // 0x00B8

    static constexpr uint16_t TOTAL_USED = BLOB_SLOT1_DATA_OFFSET + BLOB_SLOT1_CAPACITY;
    static_assert(TOTAL_USED <= FRAM::CAPACITY, "Persistent storage layout exceeds FRAM capacity");

    static constexpr uint32_t MAGIC = 0x4652414D;  // "FRAM" in ASCII
    static constexpr uint32_t FORMAT_VERSION = 3;  // Bumped: MAX_SCHEDULE_ENTRIES 8->100 shifts layout

    uint16_t scheduleEntryOffset(uint8_t index) const
    {
        return OFFSET_SCHEDULES + index * PMU::SCHEDULE_ENTRY_SIZE;
    }

    // Get FRAM offset and capacity for a blob slot
    bool getBlobSlotInfo(uint8_t slot, uint16_t &dataOffset, uint16_t &capacity) const;
    uint16_t blobSlotDescOffset(uint8_t slot) const
    {
        return OFFSET_BLOB_SLOTS + slot * BLOB_SLOT_DESC_SIZE;
    }
};

#endif  // PERSISTENT_STORAGE_H
