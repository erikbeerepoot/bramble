# Time Synchronization via Heartbeat Response

## Overview
Add datetime synchronization using existing heartbeat infrastructure. Hub responds to heartbeats with current time, nodes update their RP2040 RTC. Irrigation nodes also forward time to PMU.

## Background

### Current Situation
- RP2040 has hardware RTC but it's not currently used
- RTC resets on power loss (not battery-backed on RP2040)
- Irrigation nodes have PMU (STM32) with battery-backed RTC
- Hub runs continuously powered by RasPi
- Heartbeat infrastructure exists but hub doesn't respond
- Logs have no timestamps, making debugging difficult

### Problem
- No way to correlate events across nodes (no timestamps)
- RP2040 doesn't know current time for logging
- PMU RTC may drift over time without sync
- Debugging timing issues requires manual correlation

## Architecture

### Time Authority Hierarchy
```
RasPi System Time (NTP-synced)
    ↓ (serial: GET_DATETIME)
Hub RP2040 RTC
    ↓ (LoRa: HEARTBEAT_RESPONSE)
Node RP2040 RTC
    ↓ (UART: SetDateTime)
PMU STM32 RTC (irrigation nodes only)
```

### Message Flow

#### Irrigation Node Sync
```
1. Node → Hub: HEARTBEAT (uptime, battery, etc)
2. Hub → Node: HEARTBEAT_RESPONSE (current_datetime)
3. Node: rtc_set_datetime() on RP2040
4. Node: pmu_protocol.setDateTime() to PMU
5. PMU: Decides whether to update RTC based on drift
```

#### Hub Time Sync
```
1. Hub boots → Serial: GET_DATETIME
2. RasPi → Hub: DATETIME 2025-10-19 14:30:00 6 (ISO 8601 + day of week)
3. Hub: rtc_set_datetime()
4. [Every hour: repeat sync for drift correction]
```

#### Other Nodes (Controller, Sensor)
```
1. Node → Hub: HEARTBEAT
2. Hub → Node: HEARTBEAT_RESPONSE (current_datetime)
3. Node: rtc_set_datetime() on RP2040
```

## Implementation Plan

### Phase 1: Add Heartbeat Response Message (LoRa Protocol)

**Files**:
- `src/lora/message.h`
- `src/lora/message.cpp`
- `src/lora/message_validator.h`
- `src/lora/reliable_messenger.h`
- `src/lora/reliable_messenger.cpp`

**Tasks**:
1. Add `MSG_TYPE_HEARTBEAT_RESPONSE = 0x0B` to message type enum
2. Add `HeartbeatResponsePayload` struct:
   ```cpp
   struct __attribute__((packed)) HeartbeatResponsePayload {
       int16_t year;      // 0..4095
       int8_t  month;     // 1..12
       int8_t  day;       // 1..31
       int8_t  dotw;      // 0..6 (0=Sunday)
       int8_t  hour;      // 0..23
       int8_t  min;       // 0..59
       int8_t  sec;       // 0..59
   };
   ```
3. Add `sendHeartbeatResponse(dst_addr, datetime_t*)` to ReliableMessenger
4. Add `MSG_TYPE_HEARTBEAT_RESPONSE` case to MessageValidator
5. Add `createHeartbeatResponseMessage()` to MessageHandler

### Phase 2: Hub Time Source & Response

**Files**:
- `src/modes/hub_mode.h`
- `src/modes/hub_mode.cpp`
- `main.cpp`

**Tasks**:
1. Add `#include "hardware/rtc.h"` to hub_mode.cpp
2. Add `handleGetDateTime()` serial command handler
3. Add `syncTimeFromRaspberryPi()` function (calls GET_DATETIME via serial)
4. Call `rtc_init()` in HubMode constructor
5. Call `syncTimeFromRaspberryPi()` on hub startup
6. Add hourly timer to re-sync with RasPi
7. Modify main.cpp heartbeat handler to call `hub_mode->handleHeartbeat(source_addr, payload)`
8. Implement `handleHeartbeat()` to send HEARTBEAT_RESPONSE with current RTC time

### Phase 3: RasPi Time Interface

**Files**:
- `api/app.py`
- `api/serial_interface.py`

**Tasks**:
1. Add GET_DATETIME command handler in hub_mode.cpp
2. Add Python serial response handler
3. Hub sends: `GET_DATETIME\n`
4. RasPi responds: `DATETIME YYYY-MM-DD HH:MM:SS DOW\n` (ISO 8601)
5. Update hub to parse ISO 8601 format with sscanf

### Phase 4: Node Time Handling

**Files**:
- `src/modes/application_mode.h`
- `src/modes/application_mode.cpp`
- `src/modes/irrigation_mode.h`
- `src/modes/irrigation_mode.cpp`

**Tasks**:
1. Add `#include "hardware/rtc.h"` to application_mode.cpp
2. Call `rtc_init()` in ApplicationMode constructor
3. Add `onHeartbeatResponse(const HeartbeatResponsePayload*)` virtual method
4. Set up callback in ReliableMessenger for HEARTBEAT_RESPONSE messages
5. Default implementation: calls `rtc_set_datetime()`
6. IrrigationMode override:
   - Calls base class implementation (sets RP2040 RTC)
   - Also calls `pmu_protocol.setDateTime()` to sync PMU
   - PMU firmware decides if update needed based on drift

### Phase 5: Logger Timestamps

**Files**:
- `src/hal/logger.h`

**Tasks**:
1. Add `#include "hardware/rtc.h"` and `#include "pico/util/datetime.h"`
2. Modify `log()` template method to:
   - Call `rtc_get_datetime(&dt)`
   - If RTC running: format `[YYYY-MM-DD HH:MM:SS]` prefix
   - If RTC not running: format `[+12345ms]` prefix using `to_ms_since_boot()`
3. Keep header-only for performance (no .cpp file needed)

### Phase 6: CMakeLists Integration

**Files**:
- `CMakeLists.txt`

**Tasks**:
1. Add `hardware_rtc` to target_link_libraries for all variants
2. Add `pico_util` for datetime formatting utilities

## Data Structures

### HeartbeatResponsePayload
```cpp
struct __attribute__((packed)) HeartbeatResponsePayload {
    int16_t year;      // 0..4095 (e.g., 2025)
    int8_t  month;     // 1..12 (1=January)
    int8_t  day;       // 1..28,29,30,31
    int8_t  dotw;      // 0..6 (0=Sunday, 1=Monday, ..., 6=Saturday)
    int8_t  hour;      // 0..23
    int8_t  min;       // 0..59
    int8_t  sec;       // 0..59
};
// Total: 8 bytes
```

### Pico SDK datetime_t (already exists)
```cpp
typedef struct {
    int16_t year;    ///< 0..4095
    int8_t month;    ///< 1..12, 1 is January
    int8_t day;      ///< 1..28,29,30,31 depending on month
    int8_t dotw;     ///< 0..6, 0 is Sunday
    int8_t hour;     ///< 0..23
    int8_t min;      ///< 0..59
    int8_t sec;      ///< 0..59
} datetime_t;
```

## Serial Protocol Extensions

### New Command: GET_DATETIME
**Direction**: Hub → RasPi
**Format**: `GET_DATETIME\n`
**Response**: `DATETIME YYYY-MM-DD HH:MM:SS DOW\n` (ISO 8601 + day of week)
**Example**:
```
Hub sends: GET_DATETIME
RasPi responds: DATETIME 2025-10-19 14:30:00 6
```

**Field meanings**:
- YYYY-MM-DD HH:MM:SS: ISO 8601 datetime format
- DOW: Day of week (0=Sunday, 1=Monday, ..., 6=Saturday)

## Benefits

### Operational
- ✅ Works for all node types (irrigation, controller, sensor, hub)
- ✅ Leverages existing heartbeat infrastructure (no new protocol)
- ✅ Hub is authoritative time source (synced from RasPi system time)
- ✅ Automatic sync via regular heartbeats (no manual intervention)
- ✅ PMU retains autonomy to decide if RTC update needed

### Development & Debugging
- ✅ All logs timestamped with actual datetime
- ✅ Easy correlation of events across multiple nodes
- ✅ Falls back gracefully to boot time if RTC not synced yet
- ✅ No GetDateTime command needed for PMU (simpler protocol)
- ✅ Minimal overhead (piggybacks on existing heartbeat responses)

### Accuracy
- ✅ Hub syncs from RasPi system time (likely NTP-synced)
- ✅ Hourly re-sync corrects drift on hub
- ✅ Nodes sync on every heartbeat (typically every 30-60 seconds)
- ✅ PMU RTC provides backup for irrigation nodes during RP2040 power loss

## Edge Cases & Considerations

### RTC Not Yet Synced
- Logger falls back to `[+12345ms]` format (milliseconds since boot)
- First heartbeat response sets RTC, subsequent logs have datetime

### Time Going Backwards
- Can happen if hub RTC was wrong and gets corrected
- RP2040 RTC has no timezone support, all times are UTC/local time
- Application code should handle time changes gracefully

### Power Loss
- RP2040 RTC resets (not battery-backed)
- Next heartbeat re-syncs RTC
- Irrigation nodes: PMU RTC preserved (battery-backed)

### Network Partitions
- Nodes isolated from hub won't receive time updates
- RTC continues running but may drift
- Reconnection to hub triggers re-sync

### Hub Restart
- Hub loses RTC state
- On boot, queries RasPi for time
- Immediately synced to correct time

## Testing Plan

### Unit Tests
- MessageValidator accepts HEARTBEAT_RESPONSE
- HeartbeatResponsePayload serialization/deserialization
- Logger timestamp formatting (RTC running vs not running)

### Integration Tests
1. Hub queries RasPi for time and sets RTC
2. Node sends heartbeat, receives response with datetime
3. Node RTC updated with correct time
4. Irrigation node forwards time to PMU
5. Logger outputs correctly formatted timestamps

### Manual Testing
1. Flash hub, verify serial GET_DATETIME works
2. Flash irrigation node, verify heartbeat triggers time sync
3. Check logs show `[YYYY-MM-DD HH:MM:SS]` prefix
4. Power cycle node, verify time re-syncs after boot
5. Verify PMU RTC retains time after RP2040 power loss

## Future Enhancements

### Short Term
- Add time sync status to node status reporting
- Track "last time sync" age in diagnostics
- Alert if time hasn't synced in X hours

### Long Term
- Add timezone support (currently all UTC/local)
- NTP client directly on hub (eliminate RasPi dependency)
- GPS time source for outdoor deployments
- Time-based alarm scheduling using RTC alarms

## References
- [RP2040 RTC Documentation](https://www.raspberrypi.com/documentation/pico-sdk/hardware.html#hardware_rtc)
- [Pico SDK datetime utilities](https://www.raspberrypi.com/documentation/pico-sdk/high_level.html#pico_util)
- Existing heartbeat implementation: `src/lora/reliable_messenger.cpp:81`
- Existing message protocol: `src/lora/message.h`
