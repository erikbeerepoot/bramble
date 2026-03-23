#ifndef PERSISTENT_STORAGE_H
#define PERSISTENT_STORAGE_H

#include "fram.h"
#include "pmu_protocol.h"

#include <cstdint>

// Persistent storage layer on top of FRAM.
// Manages a memory map with magic/version header and typed accessors
// for wake interval, watering schedules, and node state.
//
// If FRAM is not present, all operations are no-ops that return false,
// allowing the system to fall back to RAM-only behavior.
class PersistentStorage {
public:
    explicit PersistentStorage(FRAM& fram);

    // Check magic number and format version. Formats storage if uninitialised.
    // Returns true if storage is usable (FRAM present and formatted).
    bool init();

    bool isAvailable() const { return available_; }

    // Wake interval
    bool loadWakeInterval(uint32_t& seconds);
    bool saveWakeInterval(uint32_t seconds);

    // Watering schedules — per-entry access (no bulk load needed)
    uint8_t getScheduleCount();
    bool loadScheduleEntry(uint8_t index, PMU::ScheduleEntry& entry);
    bool saveScheduleEntry(uint8_t index, const PMU::ScheduleEntry& entry);
    bool setScheduleCount(uint8_t count);

    // Opaque node state blob
    bool loadNodeState(uint8_t* state, uint8_t length);
    bool saveNodeState(const uint8_t* state, uint8_t length);
    bool isNodeStateValid();
    bool invalidateNodeState();

private:
    FRAM& fram_;
    bool available_;

    bool formatStorage();

    // Memory map — offsets derived from MAX_SCHEDULE_ENTRIES so they
    // adjust automatically if the schedule capacity changes.
    //
    // Layout:
    //   [0x0000] Magic (4)
    //   [0x0004] Version (4)
    //   [0x0008] Wake interval (4)
    //   [0x000C] Schedule count (1)
    //   [0x000D] Reserved (3)
    //   [0x0010] Schedule entries (SCHEDULE_ENTRY_SIZE × MAX_SCHEDULE_ENTRIES)
    //   [...]    Node state (NODE_STATE_SIZE)
    //   [...]    Node state valid flag (4)
    static constexpr uint16_t OFFSET_MAGIC           = 0x0000;
    static constexpr uint16_t OFFSET_VERSION          = 0x0004;
    static constexpr uint16_t OFFSET_WAKE_INTERVAL    = 0x0008;
    static constexpr uint16_t OFFSET_SCHEDULE_COUNT   = 0x000C;
    static constexpr uint16_t OFFSET_SCHEDULES        = 0x0010;
    static constexpr uint16_t OFFSET_NODE_STATE       = OFFSET_SCHEDULES
        + PMU::SCHEDULE_ENTRY_SIZE * PMU::MAX_SCHEDULE_ENTRIES;
    static constexpr uint16_t OFFSET_NODE_STATE_FLAG  = OFFSET_NODE_STATE + PMU::NODE_STATE_SIZE;
    static constexpr uint16_t TOTAL_USED              = OFFSET_NODE_STATE_FLAG + sizeof(uint32_t);
    static_assert(TOTAL_USED <= FRAM::CAPACITY, "Persistent storage layout exceeds FRAM capacity");

    static constexpr uint32_t MAGIC = 0x4652414D;   // "FRAM" in ASCII
    static constexpr uint32_t FORMAT_VERSION = 2;    // Bumped: dynamic schedule layout

    uint16_t scheduleEntryOffset(uint8_t index) const {
        return OFFSET_SCHEDULES + index * PMU::SCHEDULE_ENTRY_SIZE;
    }
};

#endif // PERSISTENT_STORAGE_H
