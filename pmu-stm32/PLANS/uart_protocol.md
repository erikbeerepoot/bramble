# UART Protocol for PMU-RP2040 Communication

## Overview

This document describes the UART communication protocol between the STM32L010 Power Management Unit (PMU) and the RP2040 main controller for the Bramble irrigation system.

## Protocol Design

### Message Format

All messages follow this structure:
```
[START_BYTE][LENGTH][COMMAND][DATA...][CHECKSUM][END_BYTE]
```

- **START_BYTE**: `0xAA` - Message start marker
- **LENGTH**: 1 byte - Length of COMMAND + DATA (not including START, LENGTH, CHECKSUM, END)
- **COMMAND**: 1 byte - Command/response code
- **DATA**: 0-N bytes - Command-specific payload
- **CHECKSUM**: 1 byte - XOR of LENGTH + COMMAND + DATA bytes
- **END_BYTE**: `0x55` - Message end marker

### Command Set

#### Commands (RP2040 → STM32)

| Command | Code | Description | Data Format |
|---------|------|-------------|-------------|
| SET_WAKE_INTERVAL | 0x10 | Set periodic wake interval | uint32_t seconds |
| GET_WAKE_INTERVAL | 0x11 | Request current wake interval | None |
| SET_SCHEDULE | 0x12 | Add/update schedule entry | ScheduleEntry (7 bytes) |
| GET_SCHEDULE | 0x13 | Request schedule entry | uint8_t index |
| CLEAR_SCHEDULE | 0x14 | Clear schedule entry or all | uint8_t index (0xFF = all) |
| KEEP_AWAKE | 0x15 | Extend wake period | uint16_t seconds |

#### Responses (STM32 → RP2040)

| Response | Code | Description | Data Format |
|----------|------|-------------|-------------|
| ACK | 0x80 | Command successful | None |
| NACK | 0x81 | Command failed | uint8_t error_code |
| WAKE_INTERVAL | 0x82 | Wake interval response | uint32_t seconds |
| SCHEDULE_ENTRY | 0x83 | Schedule entry response | ScheduleEntry (7 bytes) |
| WAKE_REASON | 0x84 | Unsolicited wake notification | uint8_t reason |
| STATUS | 0x85 | General status update | TBD |
| SCHEDULE_COMPLETE | 0x86 | Watering complete, power down imminent | None |

### Data Structures

#### ScheduleEntry (7 bytes)

```cpp
struct ScheduleEntry {
    uint8_t hour;        // 0-23
    uint8_t minute;      // 0-59
    uint16_t duration;   // Duration in seconds (0-65535, ~18 hours max)
    uint8_t daysMask;    // Bitmask of days (see below)
    uint8_t valveId;     // Valve identifier (0-255)
    uint8_t enabled;     // 0=disabled, 1=enabled
};
```

#### Days of Week Bitmask

```cpp
enum class DayOfWeek : uint8_t {
    Sunday    = 0x01,  // Bit 0
    Monday    = 0x02,  // Bit 1
    Tuesday   = 0x04,  // Bit 2
    Wednesday = 0x08,  // Bit 3
    Thursday  = 0x10,  // Bit 4
    Friday    = 0x20,  // Bit 5
    Saturday  = 0x40,  // Bit 6
    EveryDay  = 0x7F   // All bits set
};
```

Examples:
- Weekdays only: `0x3E` (Mon+Tue+Wed+Thu+Fri)
- Weekends only: `0x41` (Sun+Sat)
- Monday, Wednesday, Friday: `0x2A`

### Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x00 | NO_ERROR | Success |
| 0x01 | INVALID_PARAM | Parameter out of range |
| 0x02 | SCHEDULE_FULL | No space for new entry |
| 0x03 | INVALID_INDEX | Schedule index out of range |
| 0x04 | OVERLAP | Schedule overlaps with existing entry |
| 0x05 | CHECKSUM_ERROR | Message checksum mismatch |

### Wake Reasons

| Code | Name | Description |
|------|------|-------------|
| 0x00 | PERIODIC | Normal periodic wake |
| 0x01 | SCHEDULED | Scheduled event (watering) |
| 0x02 | EXTERNAL | External trigger |

## Message Examples

### Example 1: Set Wake Interval to 300 seconds (5 minutes)

**RP2040 → STM32:**
```
[0xAA][0x05][0x10][0x2C][0x01][0x00][0x00][0x18][0x55]
                   └─────── 300 as uint32_t LE ──┘
```
- LENGTH: 0x05 (1 cmd + 4 data)
- COMMAND: 0x10 (SET_WAKE_INTERVAL)
- DATA: 0x2C 0x01 0x00 0x00 (300 in little-endian)
- CHECKSUM: 0x18 (0x05 ^ 0x10 ^ 0x2C ^ 0x01 ^ 0x00 ^ 0x00)

**STM32 → RP2040:**
```
[0xAA][0x01][0x80][0x81][0x55]
```
- LENGTH: 0x01
- RESPONSE: 0x80 (ACK)
- CHECKSUM: 0x81 (0x01 ^ 0x80)

### Example 2: Add Schedule Entry (Water valve 1, Mon/Wed/Fri at 6:00 AM for 30 min)

**RP2040 → STM32:**
```
[0xAA][0x08][0x12][0x06][0x00][0x08][0x07][0x2A][0x01][0x01][0x29][0x55]
                   │     │     └───┬───┘   │     │     │
                   │     │      duration   days  valve enabled
                   │     minute (0)      (1800s)
                   hour (6)
```
- LENGTH: 0x08 (1 cmd + 7 data)
- COMMAND: 0x12 (SET_SCHEDULE)
- DATA: hour=6, min=0, duration=1800s, days=0x2A (Mon/Wed/Fri), valve=1, enabled=1
- CHECKSUM: 0x29

**STM32 → RP2040:**
```
[0xAA][0x01][0x80][0x81][0x55]  // ACK
```

or if there's an overlap:

```
[0xAA][0x02][0x81][0x04][0x87][0x55]  // NACK with OVERLAP error
```

### Example 3: Wake Notification (Scheduled Event)

**STM32 → RP2040 (Unsolicited):**
```
[0xAA][0x02][0x84][0x01][0x87][0x55]
```
- LENGTH: 0x02 (1 cmd + 1 data)
- RESPONSE: 0x84 (WAKE_REASON)
- DATA: 0x01 (SCHEDULED)
- CHECKSUM: 0x87 (0x02 ^ 0x84 ^ 0x01)

## Schedule Management

### Schedule Capacity
- Maximum 8 schedule entries
- Each entry can have multiple days via bitmask
- Entries are indexed 0-7

### Validation Rules

1. **Time Validation**
   - Hour: 0-23
   - Minute: 0-59
   - Duration: 1-65535 seconds

2. **Overlap Detection**
   - Two entries overlap if they share any common days AND their time windows overlap
   - Time window = [start_time, start_time + duration]
   - Only enabled entries are checked for overlaps

3. **Missed Schedule Handling**
   - If wake time is missed, skip that occurrence
   - Calculate next occurrence from current time
   - Never water "late" - always skip to next scheduled time

### Finding Next Schedule Entry

Algorithm:
```
1. Get current time (day of week, hour, minute)
2. For each enabled schedule entry:
   a. Check if today matches daysMask
   b. If today and time hasn't passed: calculate minutes until trigger
   c. If not today or time passed: find next matching day
3. Return entry with minimum time until trigger
```

## Watering Cycle Flow

### Scheduled Watering Event

1. **RTC Wakeup** (at scheduled time)
   - STM32 wakes from STOP mode
   - Checks if current time matches a schedule entry
   - If match: enables DC/DC converter

2. **Power-Up Notification**
   - Sends `WAKE_REASON::Scheduled` to RP2040
   - STM32 enters active watering mode (blinks RED LED)
   - Stays awake for `duration + 5` seconds

3. **RP2040 Watering**
   - RP2040 receives scheduled wake notification
   - Controls valves (250ms pulse per valve per spec)
   - Completes watering within scheduled duration

4. **Completion & Power Down**
   - After `duration + 5` seconds, STM32 sends `SCHEDULE_COMPLETE`
   - Waits 5 seconds for RP2040 to process message
   - Disables DC/DC converter (powers down RP2040)
   - Returns to normal sleep/wake cycle

**Note**: The 5-second grace period ensures RP2040 has time to complete watering even if slightly delayed. The 5-second message delivery window ensures clean shutdown.

## Implementation Status

**STM32 Implementation**: ✅ Complete (see `pmu-stm32/Core/Inc/pmu_protocol.h` and `pmu-stm32/Core/Src/pmu_protocol.cpp`)

**Integration**: ✅ Complete (see `pmu-stm32/Core/Src/main.cpp`)

**Watering Cycle**: ✅ Complete (stay-awake with duration tracking)

**Outstanding TODOs**:
1. Test protocol with actual RP2040 communication
2. Add EEPROM persistence for schedules (future enhancement)
3. Implement dynamic KEEP_AWAKE extension (if RP2040 needs more time)

## RP2040 Implementation Notes

When implementing the RP2040 side:

1. **Library Considerations**
   - Use Pico SDK UART functions
   - Implement matching MessageParser/Builder classes
   - Consider using DMA for UART if sending large messages

2. **Watering Control**
   - On receiving WAKE_REASON=SCHEDULED, check which valve
   - Query schedule entry to get valveId and duration
   - Control appropriate valve for specified duration
   - Send KEEP_AWAKE if processing takes longer

3. **Schedule Management UI**
   - Provide commands/interface to set schedules
   - Validate entries before sending to PMU
   - Store schedule backup in RP2040 flash if desired

4. **Time Synchronization**
   - Future enhancement: SET_TIME/GET_TIME commands
   - RP2040 can sync time from NTP or GPS
   - Send time updates to STM32 RTC periodically

## Future Enhancements

1. **Time Setting**
   - Add SET_TIME command to set RTC date/time
   - Add GET_TIME to query current RTC value

2. **Persistence**
   - Save schedules to EEPROM on STM32
   - Restore on boot

3. **Status Reporting**
   - Battery voltage monitoring
   - Schedule execution history
   - Error logging

4. **Advanced Scheduling**
   - Variable interval scheduling (every N days)
   - Seasonal schedules
   - Conditional execution (based on sensors)

## Notes

- Little-endian byte order for multi-byte integers
- Keep UART baud rate low (9600) for reliability
- Use XOR checksum for simplicity (sufficient for this application)
- Parser uses state machine for robustness against partial/corrupt messages
- Schedule overlap detection only applies to entries sharing common days
- Maximum message size: ~64 bytes (START + 1 + 1 + up to 58 + 1 + END)
