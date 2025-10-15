# LoRa Schedule Management - Pull-Based Architecture

## Overview

Implement remote schedule management for irrigation nodes over LoRa using a **pull-based architecture**. Irrigation nodes wake periodically, query the hub for pending updates (schedules, datetime sync, config), apply them to the PMU, and return to sleep. The hub maintains per-node update queues and serves them when nodes request.

## Architecture

### Pull Model vs Push Model

**Why Pull Model?**
- ✅ **Power efficient**: Node controls when radio is active
- ✅ **Simple**: No need for hub to track node sleep schedules
- ✅ **Reliable**: Node retries queries if hub doesn't respond
- ✅ **Natural fit**: Nodes already wake periodically for heartbeats

**How it works:**
1. Hub receives schedule command from Raspberry Pi (via serial)
2. Hub queues update for target node
3. Node wakes on periodic interval (e.g., 30s)
4. Node sends CHECK_UPDATES message to hub
5. Hub responds with UPDATE_AVAILABLE (or "no updates")
6. Node applies update to PMU
7. Node sends UPDATE_ACK back to hub
8. Hub removes update from queue
9. Node checks for more updates (loop until queue empty)
10. Node allows sleep

### System Components

```
┌─────────────┐ Serial ┌──────────┐ LoRa   ┌──────────────┐
│ Raspberry Pi├────────►│   Hub    ├────────►│ Irrigation   │
│  REST API   │        │  Router  │◄────────┤    Node      │
└─────────────┘        │  + Queue │        └──────┬───────┘
                       └──────────┘               │
                                                  │ UART
                                                  │
                                              ┌───▼────┐
                                              │  PMU   │
                                              │ (STM32)│
                                              └────────┘
```

## Code Audit Results

### ✅ What's Already Implemented

1. **LoRa Message Infrastructure** ([src/lora/message.h](../src/lora/message.h))
   - Complete message framing with header/payload
   - 247 byte max payload (sufficient for schedule data)
   - Reliable delivery with ACK/retry mechanism
   - **Missing**: Schedule-specific message types

2. **Hub Router with Queuing** ([src/lora/hub_router.h](../src/lora/hub_router.h))
   - `QueuedMessage` struct and `message_queue_`
   - `queueMessage()` and `processQueuedMessages()`
   - Node online/offline tracking via `RouteEntry`
   - **Issue**: Current queue is for forwarding failures, not for storing pending commands
   - **Need**: Separate per-node update queue

3. **PMU Protocol Structures** ([src/hal/pmu_protocol.h](../src/hal/pmu_protocol.h))
   - ✅ `DateTime` struct (7 bytes): year, month, day, weekday, hour, minute, second
   - ✅ `ScheduleEntry` struct (7 bytes): hour, minute, duration, daysMask, valveId, enabled
   - ✅ Commands: SetDateTime (0x16), SetSchedule (0x12), ClearSchedule (0x14)
   - **Ready**: Structures can be directly embedded in LoRa payloads

4. **Irrigation Node - PMU Integration** ([src/modes/irrigation_mode.cpp](../src/modes/irrigation_mode.cpp))
   - ✅ PMU client with UART communication
   - ✅ Wake notification handler (Periodic, Scheduled, External)
   - ✅ `onLoop()` calls `pmu_client_->process()`
   - ✅ Callback-based async PMU protocol
   - **Current**: Hardcoded test schedule in `onStart()`
   - **Need**: Replace with dynamic update processing

5. **Hub Mode** ([src/modes/hub_mode.cpp](../src/modes/hub_mode.cpp))
   - ✅ Uses HubRouter for message routing
   - ✅ Periodic maintenance tasks
   - **Missing**: Serial command interface, update queue management

### 🔨 What Needs Implementation

#### 1. New LoRa Message Types
**File**: [src/lora/message.h](../src/lora/message.h)

```cpp
// Add to MessageType enum (around line 30)
enum MessageType {
    // ... existing types ...
    MSG_TYPE_CHECK_UPDATES  = 0x09,  // Node → Hub: Check for pending updates
    MSG_TYPE_UPDATE_AVAILABLE = 0x0A, // Hub → Node: Update response
    MSG_TYPE_UPDATE_ACK     = 0x0B   // Node → Hub: Acknowledge update applied
};

// Update type classification
enum class UpdateType : uint8_t {
    SET_SCHEDULE = 0x01,        // Add/modify schedule entry
    REMOVE_SCHEDULE = 0x02,     // Remove schedule entry
    SET_DATETIME = 0x03,        // Sync RTC date/time
    SET_WAKE_INTERVAL = 0x04    // Change periodic wake interval
};

// Payload structures
struct CheckUpdatesPayload {
    uint8_t node_sequence;  // Node's current update sequence number
} __attribute__((packed));

struct UpdateAvailablePayload {
    uint8_t has_update;         // 0=no updates, 1=update follows
    uint8_t update_type;        // UpdateType enum
    uint8_t sequence;           // Hub's sequence for this update
    uint8_t payload_data[24];   // Type-specific data (schedules, datetime, etc.)
} __attribute__((packed));

struct UpdateAckPayload {
    uint8_t sequence;       // Sequence number being acknowledged
    uint8_t success;        // 1=success, 0=failed
    uint8_t error_code;     // PMU::ErrorCode if failed
} __attribute__((packed));

// Add to message payload union
union {
    // ... existing payloads ...
    CheckUpdatesPayload check_updates;
    UpdateAvailablePayload update_available;
    UpdateAckPayload update_ack;
} payload;
```

**Payload format for UpdateType::SET_SCHEDULE** (in `payload_data[24]`):
```
Byte 0:       index (0-7)
Byte 1:       hour (0-23)
Byte 2:       minute (0-59)
Byte 3-4:     duration (uint16_t, seconds)
Byte 5:       daysMask (bitmask)
Byte 6:       valveId (0-N)
Byte 7:       enabled (1/0)
```

**Payload format for UpdateType::SET_DATETIME**:
```
Byte 0:       year (since 2000)
Byte 1:       month (1-12)
Byte 2:       day (1-31)
Byte 3:       weekday (0=Sun, 1=Mon, ..., 6=Sat)
Byte 4:       hour (0-23)
Byte 5:       minute (0-59)
Byte 6:       second (0-59)
```

**Payload format for UpdateType::SET_WAKE_INTERVAL**:
```
Byte 0-1:     interval_seconds (uint16_t)
```

#### 2. Hub Update Queue System
**File**: [src/lora/hub_router.h](../src/lora/hub_router.h)

```cpp
// Add to HubRouter class

struct PendingUpdate {
    UpdateType type;
    uint32_t queued_at_ms;      // When update was queued
    uint8_t sequence;           // Hub assigns sequence numbers
    uint8_t data[32];           // Update payload (type-specific)
    uint8_t data_length;        // Length of data
};

struct NodeUpdateState {
    std::queue<PendingUpdate> pending_updates;
    uint8_t next_sequence;      // Next sequence number to assign
    uint32_t last_check_time;   // When node last checked for updates
};

private:
    // Per-node update queues (node_address -> update state)
    std::map<uint16_t, NodeUpdateState> node_updates_;

public:
    // Queue management methods
    void queueScheduleUpdate(uint16_t node_addr, uint8_t index,
                            const PMU::ScheduleEntry& entry);
    void queueRemoveSchedule(uint16_t node_addr, uint8_t index);
    void queueDateTimeUpdate(uint16_t node_addr, const PMU::DateTime& datetime);
    void queueWakeIntervalUpdate(uint16_t node_addr, uint16_t interval_seconds);

    // Message handlers
    void handleCheckUpdates(uint16_t node_addr, uint8_t node_sequence);
    void handleUpdateAck(uint16_t node_addr, uint8_t sequence,
                        bool success, uint8_t error_code);

    // Query methods
    size_t getPendingUpdateCount(uint16_t node_addr);
    void clearPendingUpdates(uint16_t node_addr);
```

**Implementation notes**:
- Each node has independent update queue
- Hub assigns monotonically increasing sequence numbers per node
- Queue has configurable max size (e.g., 10 updates per node)
- Old updates can be expired after timeout (e.g., 1 hour)

#### 3. Hub Serial Command Interface
**NEW FILE**: `src/hub/serial_interface.h`

```cpp
#pragma once
#include "../lora/hub_router.h"
#include <cstddef>

/**
 * @brief Serial command interface for hub control
 *
 * Provides text-based command interface for Raspberry Pi to manage
 * irrigation nodes remotely. Commands are queued and delivered when
 * nodes wake and check for updates.
 */
class HubSerialInterface {
public:
    explicit HubSerialInterface(HubRouter& router);

    /**
     * @brief Process incoming serial data
     * Call from main loop to handle commands
     */
    void processInput();

    /**
     * @brief Send response to Raspberry Pi
     */
    void sendResponse(const char* response);

    /**
     * @brief Send unsolicited event notification
     */
    void sendEvent(const char* event);

private:
    HubRouter& router_;
    char input_buffer_[256];
    size_t input_pos_;

    void handleCommand(const char* cmd);
    void parseSetSchedule(const char* args);
    void parseSetDateTime(const char* args);
    void parseRemoveSchedule(const char* args);
    void parseSetWakeInterval(const char* args);
    void listNodes();
    void getQueue(const char* args);
};
```

**Command Protocol**:

**Commands (Raspberry Pi → Hub)**:
```
LIST_NODES
  Response: NODE_LIST <count>
            NODE <addr> <type> <online> <last_seen_sec>
            ...

SET_SCHEDULE <addr> <index> <hour>:<min> <duration> <days> <valve>
  Example: SET_SCHEDULE 42 0 14:30 900 127 0
  Response: QUEUED SET_SCHEDULE <addr> <position>

REMOVE_SCHEDULE <addr> <index>
  Response: QUEUED REMOVE_SCHEDULE <addr> <position>

SET_DATETIME <addr> <YYYY-MM-DD> <HH:MM:SS> <weekday>
  Example: SET_DATETIME 42 2025-01-15 14:30:00 3
  Response: QUEUED SET_DATETIME <addr> <position>

SET_WAKE_INTERVAL <addr> <seconds>
  Example: SET_WAKE_INTERVAL 42 60
  Response: QUEUED SET_WAKE_INTERVAL <addr> <position>

GET_QUEUE <addr>
  Response: QUEUE <addr> <count>
            UPDATE <seq> <type> <age_sec>
            ...
```

**Events (Hub → Raspberry Pi, unsolicited)**:
```
NODE_ONLINE <addr>
NODE_OFFLINE <addr>
UPDATE_DELIVERED <addr> <seq> <type> SUCCESS
UPDATE_FAILED <addr> <seq> <type> <error_code>
HEARTBEAT <addr> <battery> <rssi>
```

#### 4. Irrigation Node Update Handlers
**File**: [src/modes/irrigation_mode.h](../src/modes/irrigation_mode.h)

```cpp
class IrrigationMode : public ApplicationMode {
private:
    // ... existing members ...

    // Update handling state
    bool awaiting_updates_;
    uint32_t update_check_time_;
    uint32_t last_keep_awake_ms_;
    uint8_t update_sequence_;

    // New methods
    void sendCheckUpdates();
    void handleUpdateAvailable(const UpdateAvailablePayload* payload);
    void applyScheduleUpdate(const uint8_t* data);
    void applyRemoveSchedule(const uint8_t* data);
    void applyDateTimeUpdate(const uint8_t* data);
    void applyWakeIntervalUpdate(const uint8_t* data);
    void sendUpdateAck(uint8_t sequence, bool success, uint8_t error_code);

protected:
    // Override from base class
    void onUpdateAvailable(const UpdateAvailablePayload* payload) override;
};
```

**File**: [src/modes/irrigation_mode.cpp](../src/modes/irrigation_mode.cpp)

**Modify `handlePmuWake()` around line 141**:
```cpp
void IrrigationMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry* entry) {
    switch (reason) {
        case PMU::WakeReason::Periodic:
            pmu_logger.info("Periodic wake - checking for updates");

            // Keep awake while processing updates
            pmu_client_->keepAwake();
            last_keep_awake_ms_ = to_ms_since_boot(get_absolute_time());

            // Send heartbeat first
            uint32_t uptime = to_ms_since_boot(get_absolute_time()) / 1000;
            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, 85, 65,
                                   CAP_VALVE_CONTROL, 0);

            // Check for pending updates from hub
            sendCheckUpdates();

            // Set flag to keep awake
            awaiting_updates_ = true;
            update_check_time_ = to_ms_since_boot(get_absolute_time());
            break;

        // ... existing Scheduled and External cases ...
    }
}
```

**Add new method**:
```cpp
void IrrigationMode::sendCheckUpdates() {
    // Build CHECK_UPDATES message
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length = MessageHandler::createCheckUpdatesMessage(
        messenger_.getAddress(),
        HUB_ADDRESS,
        messenger_.getNextSequenceNumber(),
        update_sequence_,
        buffer
    );

    // Send with reliable delivery
    messenger_.send(buffer, length, RELIABLE);

    pmu_logger.debug("Sent CHECK_UPDATES (seq=%d)", update_sequence_);
}

void IrrigationMode::onUpdateAvailable(const UpdateAvailablePayload* payload) {
    if (!payload->has_update) {
        // No updates - can sleep
        pmu_logger.debug("No updates available");
        awaiting_updates_ = false;
        return;
    }

    pmu_logger.info("Update available: type=%d, seq=%d",
                   payload->update_type, payload->sequence);

    // Process update based on type
    bool success = false;
    PMU::ErrorCode error = PMU::ErrorCode::NoError;

    switch (static_cast<UpdateType>(payload->update_type)) {
        case UpdateType::SET_SCHEDULE:
            applyScheduleUpdate(payload->payload_data);
            break;

        case UpdateType::REMOVE_SCHEDULE:
            applyRemoveSchedule(payload->payload_data);
            break;

        case UpdateType::SET_DATETIME:
            applyDateTimeUpdate(payload->payload_data);
            break;

        case UpdateType::SET_WAKE_INTERVAL:
            applyWakeIntervalUpdate(payload->payload_data);
            break;

        default:
            pmu_logger.error("Unknown update type: %d", payload->update_type);
            error = PMU::ErrorCode::InvalidParam;
            break;
    }

    // Note: Actual ACK sent from PMU callback after command completes
    // Store sequence number for callback
    update_sequence_ = payload->sequence;
}

void IrrigationMode::applyScheduleUpdate(const uint8_t* data) {
    // Parse schedule data from payload
    PMU::ScheduleEntry entry;
    uint8_t index = data[0];
    entry.hour = data[1];
    entry.minute = data[2];
    entry.duration = (data[4] << 8) | data[3];  // uint16_t
    entry.daysMask = static_cast<PMU::DayOfWeek>(data[5]);
    entry.valveId = data[6];
    entry.enabled = (data[7] != 0);

    pmu_logger.info("Applying schedule: idx=%d, time=%02d:%02d, dur=%d, valve=%d",
                   index, entry.hour, entry.minute, entry.duration, entry.valveId);

    // Apply to PMU (async with callback)
    auto& protocol = pmu_client_->getProtocol();
    protocol.setSchedule(entry, [this](bool success, PMU::ErrorCode error) {
        // Send ACK/NACK back to hub
        sendUpdateAck(update_sequence_, success, static_cast<uint8_t>(error));

        if (success) {
            pmu_logger.info("Schedule update applied successfully");
        } else {
            pmu_logger.error("Schedule update failed: error=%d",
                           static_cast<int>(error));
        }

        // Check for more updates
        sendCheckUpdates();
    });
}

// Similar implementations for other update types...

void IrrigationMode::sendUpdateAck(uint8_t sequence, bool success, uint8_t error_code) {
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length = MessageHandler::createUpdateAckMessage(
        messenger_.getAddress(),
        HUB_ADDRESS,
        messenger_.getNextSequenceNumber(),
        sequence,
        success ? 1 : 0,
        error_code,
        buffer
    );

    messenger_.send(buffer, length, RELIABLE);

    pmu_logger.debug("Sent UPDATE_ACK (seq=%d, success=%d)", sequence, success);
}
```

**Update `onLoop()` for timeout handling**:
```cpp
void IrrigationMode::onLoop() {
    // Process any pending PMU messages (moved out of IRQ context)
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }

    // Check if we're waiting for updates and need to keep awake
    if (awaiting_updates_) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Keep calling keepAwake every second to prevent sleep
        if (now - last_keep_awake_ms_ > 1000) {
            pmu_client_->keepAwake();
            last_keep_awake_ms_ = now;
        }

        // Timeout after 5 seconds - allow sleep
        if (now - update_check_time_ > 5000) {
            awaiting_updates_ = false;
            pmu_logger.info("Update check timeout - allowing sleep");
        }
    }
}
```

#### 5. Message Handler Registration
**File**: [src/modes/application_mode.cpp](../src/modes/application_mode.cpp)

Add handlers in `processIncomingMessage()`:
```cpp
void ApplicationMode::processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) {
    // ... existing parsing ...

    switch (header->type) {
        // ... existing cases ...

        case MSG_TYPE_CHECK_UPDATES:
            if (IS_HUB) {
                const CheckUpdatesPayload* payload =
                    reinterpret_cast<const CheckUpdatesPayload*>(message.payload);
                hub_router_->handleCheckUpdates(header->src_addr,
                                               payload->node_sequence);
            }
            break;

        case MSG_TYPE_UPDATE_AVAILABLE:
            if (!IS_HUB) {
                const UpdateAvailablePayload* payload =
                    reinterpret_cast<const UpdateAvailablePayload*>(message.payload);
                onUpdateAvailable(payload);  // Virtual method
            }
            break;

        case MSG_TYPE_UPDATE_ACK:
            if (IS_HUB) {
                const UpdateAckPayload* payload =
                    reinterpret_cast<const UpdateAckPayload*>(message.payload);
                hub_router_->handleUpdateAck(header->src_addr,
                                            payload->sequence,
                                            payload->success != 0,
                                            payload->error_code);
            }
            break;
    }
}
```

**Add virtual method to base class** ([src/modes/application_mode.h](../src/modes/application_mode.h)):
```cpp
virtual void onUpdateAvailable(const UpdateAvailablePayload* payload) {}
```

## Implementation Plan

### Phase 1: Message Protocol Foundation
**Estimated time**: 2-3 hours

1. Add new message types to [src/lora/message.h](../src/lora/message.h)
   - `MSG_TYPE_CHECK_UPDATES`, `UPDATE_AVAILABLE`, `UPDATE_ACK`
   - `UpdateType` enum
   - Payload structures: `CheckUpdatesPayload`, `UpdateAvailablePayload`, `UpdateAckPayload`
   - Add to message payload union

2. Add helper functions to `MessageHandler` class
   - `createCheckUpdatesMessage()`
   - `createUpdateAvailableMessage()`
   - `createUpdateAckMessage()`
   - Payload parsing helpers

3. **Test**: Build to verify no compilation errors

### Phase 2: Hub Update Queue Infrastructure
**Estimated time**: 3-4 hours

1. Add structures to [src/lora/hub_router.h](../src/lora/hub_router.h)
   - `PendingUpdate` struct
   - `NodeUpdateState` struct
   - `node_updates_` map

2. Implement queue methods in [src/lora/hub_router.cpp](../src/lora/hub_router.cpp)
   - `queueScheduleUpdate()` - pack ScheduleEntry into PendingUpdate
   - `queueDateTimeUpdate()` - pack DateTime into PendingUpdate
   - `queueRemoveSchedule()`, `queueWakeIntervalUpdate()`
   - `getPendingUpdateCount()`, `clearPendingUpdates()`

3. Implement message handlers
   - `handleCheckUpdates()` - check queue, send UPDATE_AVAILABLE or "no updates"
   - `handleUpdateAck()` - remove from queue on success, handle errors

4. **Test**: Unit test queue operations (queue, retrieve, ack)

### Phase 3: Irrigation Node Pull Logic
**Estimated time**: 3-4 hours

1. Add members to [src/modes/irrigation_mode.h](../src/modes/irrigation_mode.h)
   - State variables: `awaiting_updates_`, `update_check_time_`, etc.
   - Method declarations

2. Modify [src/modes/irrigation_mode.cpp](../src/modes/irrigation_mode.cpp)
   - Update `handlePmuWake()` to send CHECK_UPDATES on periodic wake
   - Implement `sendCheckUpdates()`
   - Implement `onUpdateAvailable()` dispatcher
   - Implement update application methods (`applyScheduleUpdate()`, etc.)
   - Implement `sendUpdateAck()`
   - Update `onLoop()` for keep-awake and timeout logic

3. Add virtual method override to base class
   - [src/modes/application_mode.h](../src/modes/application_mode.h): `virtual void onUpdateAvailable()`
   - [src/modes/application_mode.cpp](../src/modes/application_mode.cpp): Add message type handlers

4. **Remove hardcoded test schedule** from `IrrigationMode::onStart()`

5. **Test**:
   - Queue update while node asleep
   - Verify node queries hub on wake
   - Verify update applied to PMU
   - Verify ACK sent back

### Phase 4: Serial Command Interface
**Estimated time**: 3-4 hours

1. Create [src/hub/serial_interface.h](../src/hub/serial_interface.h)
   - Class declaration with command parsing

2. Create [src/hub/serial_interface.cpp](../src/hub/serial_interface.cpp)
   - Implement `processInput()` - read serial, parse commands
   - Implement command parsers:
     - `parseSetSchedule()` - parse args, call `hub_router_->queueScheduleUpdate()`
     - `parseSetDateTime()` - parse ISO date/time, call `queueDateTimeUpdate()`
     - `parseRemoveSchedule()`, `parseSetWakeInterval()`
   - Implement query commands:
     - `listNodes()` - iterate address_manager, print node info
     - `getQueue()` - show pending updates for node
   - Implement `sendResponse()`, `sendEvent()`

3. Integrate into [src/modes/hub_mode.cpp](../src/modes/hub_mode.cpp)
   - Create `HubSerialInterface` instance in `HubMode::onStart()`
   - Call `serial_interface_->processInput()` in main loop
   - Send events on update delivery success/failure

4. **Test**: Manual serial commands
   - `LIST_NODES` - verify node list
   - `SET_SCHEDULE 42 0 14:30 60 127 0` - verify queued
   - `GET_QUEUE 42` - verify shows pending update
   - Wait for node wake, verify `UPDATE_DELIVERED` event

### Phase 5: Integration & Testing
**Estimated time**: 2-3 hours

1. **End-to-end flow test**:
   - Start hub and irrigation node
   - Node registers and goes into periodic wake cycle
   - Send `SET_SCHEDULE` via serial while node asleep
   - Verify update queued
   - Wait for node periodic wake (~30s)
   - Verify node queries, receives update, applies to PMU
   - Verify `UPDATE_DELIVERED` event printed to serial
   - Verify schedule executes at configured time

2. **Multiple updates test**:
   - Queue 3 schedule updates while node asleep
   - Verify node processes all 3 in sequence before sleeping
   - Verify all 3 ACKs received

3. **Error handling test**:
   - Send invalid schedule (bad time, valve ID)
   - Verify PMU rejects, node sends NACK
   - Verify hub removes from queue and reports failure

4. **Timeout test**:
   - Simulate hub offline (don't respond to CHECK_UPDATES)
   - Verify node times out after 5 seconds and allows sleep

5. **DateTime sync test**:
   - Send `SET_DATETIME` command
   - Verify RTC synchronized on node
   - Send schedule for near-future time
   - Verify scheduled wake triggers correctly

## File Summary

### Files to Create
- `src/hub/serial_interface.h` - Serial command interface header
- `src/hub/serial_interface.cpp` - Serial command implementation

### Files to Modify
- `src/lora/message.h` - Add message types, payloads, UpdateType enum
- `src/lora/message.cpp` - Add MessageHandler helper functions
- `src/lora/hub_router.h` - Add update queue structures and methods
- `src/lora/hub_router.cpp` - Implement queue management and handlers
- `src/modes/irrigation_mode.h` - Add update handling members and methods
- `src/modes/irrigation_mode.cpp` - Implement pull logic and update handlers
- `src/modes/application_mode.h` - Add virtual `onUpdateAvailable()` method
- `src/modes/application_mode.cpp` - Add message type handlers
- `src/modes/hub_mode.h` - Add serial_interface_ member
- `src/modes/hub_mode.cpp` - Initialize and process serial interface

**Total: 2 new files, 9 modified files**

## Configuration & Tuning

### Timing Parameters
```cpp
// In irrigation_mode.cpp
constexpr uint32_t UPDATE_CHECK_TIMEOUT_MS = 5000;  // Allow sleep after 5s
constexpr uint32_t KEEP_AWAKE_INTERVAL_MS = 1000;   // Call keepAwake() every 1s

// In hub_router.cpp
constexpr uint32_t UPDATE_QUEUE_MAX_SIZE = 10;      // Max updates per node
constexpr uint32_t UPDATE_EXPIRE_TIME_MS = 3600000; // Expire after 1 hour
```

### Wake Interval Configuration
- **Development/testing**: 30 seconds (quick feedback)
- **Production**: 60-300 seconds (balance latency vs power)
- Configurable via `SET_WAKE_INTERVAL` command

## Success Criteria

- ✅ Hub can queue schedule updates while nodes are asleep
- ✅ Nodes query hub on periodic wake and receive updates
- ✅ Updates are applied to PMU successfully
- ✅ ACKs are sent back to hub and updates removed from queue
- ✅ Multiple updates are batched and processed before sleep
- ✅ Timeout allows node to sleep if hub doesn't respond
- ✅ Serial interface provides manual control for testing
- ✅ DateTime synchronization works correctly
- ✅ Scheduled watering executes at configured times

## Future Enhancements

1. **Update priorities**: Priority queue for critical updates (e.g., emergency valve closure)
2. **Update expiration**: Auto-remove stale updates after timeout
3. **Query schedules**: Node can request current schedule from hub
4. **Bulk schedule upload**: Send multiple schedules in single UPDATE_AVAILABLE message
5. **Update checksums**: Verify integrity of multi-packet updates
6. **Persistent queue**: Save pending updates to flash, survive hub reboot
7. **REST API**: Replace serial interface with HTTP REST API on Raspberry Pi

## Design Rationale

### Why Not Push Model?
**Push model issues**:
- Hub must know when node will be awake
- Requires hub to buffer messages during node sleep
- Node must stay awake listening for potential messages
- Higher power consumption
- More complex synchronization

### Why Sequence Numbers?
- Detect missed updates
- Allow node to request resend if sequence gap detected
- Hub can track "last delivered" sequence per node

### Why Keep-Awake Pattern?
- PMU will put node to sleep if no `keepAwake()` calls received
- While processing updates, node must prevent sleep
- Timeout ensures node doesn't stay awake forever if hub fails

### Why 5-Second Timeout?
- Long enough for LoRa message exchange (typ. 1-2 seconds)
- Short enough to minimize wasted wake time
- Allows 2-3 retry attempts if first message lost

## Testing Strategy

1. **Unit tests**: Queue operations, message parsing
2. **Component tests**: Hub queue, node update application
3. **Integration tests**: Full end-to-end flow with real PMU
4. **Power tests**: Measure current consumption during update cycle
5. **Reliability tests**: Packet loss scenarios, hub failure recovery
6. **Load tests**: Multiple nodes, concurrent updates

## Power Consumption Analysis

**Typical periodic wake cycle**:
- Wake: 50ms (PMU startup)
- LoRa TX (heartbeat): 100ms @ 100mA
- LoRa TX (CHECK_UPDATES): 100ms @ 100mA
- LoRa RX (UPDATE_AVAILABLE): 200ms @ 15mA
- PMU command processing: 50ms @ 20mA
- LoRa TX (UPDATE_ACK): 100ms @ 100mA
- **Total**: ~600ms active time per 30s cycle
- **Duty cycle**: 2%

**With update available**:
- Add 3 more TX/RX cycles: +600ms
- **Total**: ~1200ms active time
- **Duty cycle**: 4%

**Still excellent power efficiency for battery operation**

## Risk Mitigation

### Risk: Update queue overflow
**Mitigation**: Max queue size per node (10), FIFO eviction

### Risk: Node never checks updates (offline)
**Mitigation**: Update expiration (1 hour), report stale updates to serial

### Risk: Hub crashes, loses queue
**Mitigation**: (Future) Persist queue to flash, load on restart

### Risk: LoRa packet loss during update transfer
**Mitigation**: Reliable delivery (ACK/retry), timeout and retry

### Risk: PMU rejects update
**Mitigation**: Node sends NACK with error code, hub removes from queue and reports

## Development Timeline

- **Phase 1**: Message Protocol - 0.5 days
- **Phase 2**: Hub Queue - 0.5 days
- **Phase 3**: Node Pull Logic - 0.5 days
- **Phase 4**: Serial Interface - 0.5 days
- **Phase 5**: Integration & Testing - 0.5 days

**Total estimated time**: 2.5 days

## Next Steps After Completion

1. **Raspberry Pi REST API**: Replace serial with HTTP REST interface
2. **Web UI**: Build dashboard for schedule management
3. **Multiple irrigation nodes**: Test with 2-3 nodes simultaneously
4. **Persistent storage**: Save schedules on hub, restore on reboot
5. **Schedule templates**: Pre-defined watering schedules for common crops
