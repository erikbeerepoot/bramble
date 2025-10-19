# Hub Serial Integration Plan

## Decision: Queue on Hub (Option A)

After analyzing options for queue location (hub vs RasPi), we've chosen to implement the update queue **on the hub** for the following reasons:

1. **Simplicity**: Self-contained, no external dependencies (Redis/Kafka)
2. **Low latency**: Direct LoRa response to node CHECK_UPDATES
3. **Fault tolerance**: Hub operates standalone without RasPi
4. **Right scale**: Perfect for single-farm deployment (1-10 nodes)
5. **Migration path**: Easy to add Redis persistence later if needed

## Architecture Overview

```
┌──────────────┐ UART1       ┌─────────────────┐ LoRa    ┌──────────────┐
│ Raspberry Pi │ (D24/D25)   │   RP2040 Hub    │◄───────►│  Irrigation  │
│  REST API    │◄───────────►│  - Queue (RAM)  │         │     Node     │
│  (Python)    │ 115200 baud │  - LoRa Radio   │         │              │
└──────────────┘             └─────────────────┘         └───────┬──────┘
                                                                  │ UART0
                                                              ┌───▼────┐
                                                              │  PMU   │
                                                              │ (STM32)│
                                                              └────────┘
```

**Data flow**:
1. User → REST API → Serial → Hub (queues update)
2. Node wakes → CHECK_UPDATES → Hub (serves from queue)
3. Node applies update → UPDATE_ACK → Hub (removes from queue)
4. Hub → Serial → REST API (event notification)

## What's Already Complete ✅

From the REST API side (`api/`):
- ✅ Flask REST API with all endpoints
- ✅ Serial interface with command/response protocol
- ✅ Data models (Node, Schedule, QueuedUpdate)
- ✅ GET_DATETIME query/response for RTC sync
- ✅ Systemd service configuration

From the hub firmware (existing):
- ✅ HubRouter with message routing and node tracking
- ✅ AddressManager with node registry
- ✅ HubMode main loop and maintenance tasks
- ✅ USB stdio configured (115200 baud)

## Implementation Tasks

### Phase 1: LoRa Message Protocol
**Estimated**: 2-3 hours

**File**: `src/lora/message.h`

Add message types (around line 38):
```cpp
enum MessageType {
    // ... existing types ...
    MSG_TYPE_CHECK_UPDATES    = 0x09,  // Node → Hub: Check for pending updates
    MSG_TYPE_UPDATE_AVAILABLE = 0x0A,  // Hub → Node: Update response
    MSG_TYPE_UPDATE_ACK       = 0x0B   // Node → Hub: Acknowledge update applied
};
```

Add update type enum:
```cpp
// Update types for MSG_TYPE_UPDATE_AVAILABLE
enum class UpdateType : uint8_t {
    SET_SCHEDULE        = 0x01,  // Add/modify schedule entry
    REMOVE_SCHEDULE     = 0x02,  // Remove schedule entry
    SET_DATETIME        = 0x03,  // Sync RTC date/time
    SET_WAKE_INTERVAL   = 0x04   // Change periodic wake interval
};
```

Add payload structures:
```cpp
// Node queries hub for updates
struct __attribute__((packed)) CheckUpdatesPayload {
    uint8_t node_sequence;  // Node's current sequence number
};

// Hub responds with update (or no update)
struct __attribute__((packed)) UpdateAvailablePayload {
    uint8_t has_update;         // 0=no updates, 1=update follows
    uint8_t update_type;        // UpdateType enum
    uint8_t sequence;           // Hub's sequence for this update
    uint8_t payload_data[24];   // Type-specific data
};

// Node confirms update applied
struct __attribute__((packed)) UpdateAckPayload {
    uint8_t sequence;       // Sequence being ACKed
    uint8_t success;        // 1=success, 0=failed
    uint8_t error_code;     // PMU::ErrorCode if failed
};
```

Add to message payload union:
```cpp
struct __attribute__((packed)) Message {
    MessageHeader header;
    union {
        // ... existing payloads ...
        CheckUpdatesPayload check_updates;
        UpdateAvailablePayload update_available;
        UpdateAckPayload update_ack;
        uint8_t raw[MESSAGE_MAX_PAYLOAD];
    } payload;
};
```

**File**: `src/lora/message.cpp`

Add helper functions to MessageHandler class:
```cpp
static size_t createCheckUpdatesMessage(uint16_t src_addr, uint16_t dst_addr,
                                       uint8_t seq_num, uint8_t node_sequence,
                                       uint8_t* buffer);

static size_t createUpdateAvailableMessage(uint16_t src_addr, uint16_t dst_addr,
                                          uint8_t seq_num, bool has_update,
                                          uint8_t update_type, uint8_t sequence,
                                          const uint8_t* payload_data,
                                          uint8_t* buffer);

static size_t createUpdateAckMessage(uint16_t src_addr, uint16_t dst_addr,
                                    uint8_t seq_num, uint8_t sequence,
                                    uint8_t success, uint8_t error_code,
                                    uint8_t* buffer);
```

**Testing**: Build to verify no compilation errors.

---

### Phase 2: Hub Update Queue
**Estimated**: 3-4 hours

**File**: `src/lora/hub_router.h`

Add structures (around line 35):
```cpp
// Pending update for a node
struct PendingUpdate {
    UpdateType type;
    uint32_t queued_at_ms;      // When update was queued
    uint8_t sequence;           // Hub-assigned sequence number
    uint8_t data[32];           // Update payload (type-specific)
    uint8_t data_length;        // Length of data
};

// Per-node update state
struct NodeUpdateState {
    std::queue<PendingUpdate> pending_updates;
    uint8_t next_sequence;      // Next sequence to assign
    uint32_t last_check_time;   // When node last checked
};
```

Add to HubRouter class (private section):
```cpp
private:
    // Per-node update queues (node_address -> update state)
    std::map<uint16_t, NodeUpdateState> node_updates_;

    // Configuration
    static constexpr size_t MAX_UPDATES_PER_NODE = 10;
    static constexpr uint32_t UPDATE_EXPIRE_MS = 3600000;  // 1 hour
```

Add to HubRouter class (public section):
```cpp
public:
    // Queue management
    bool queueScheduleUpdate(uint16_t node_addr, uint8_t index,
                            const PMU::ScheduleEntry& entry);
    bool queueRemoveSchedule(uint16_t node_addr, uint8_t index);
    bool queueDateTimeUpdate(uint16_t node_addr, const PMU::DateTime& datetime);
    bool queueWakeIntervalUpdate(uint16_t node_addr, uint16_t interval_seconds);

    // Message handlers (called from ApplicationMode)
    void handleCheckUpdates(uint16_t node_addr, uint8_t node_sequence);
    void handleUpdateAck(uint16_t node_addr, uint8_t sequence,
                        bool success, uint8_t error_code);

    // Query methods
    size_t getPendingUpdateCount(uint16_t node_addr) const;
    void clearPendingUpdates(uint16_t node_addr);
    void printQueueStats() const;
```

**File**: `src/lora/hub_router.cpp`

Implement queue methods:
```cpp
bool HubRouter::queueScheduleUpdate(uint16_t node_addr, uint8_t index,
                                   const PMU::ScheduleEntry& entry) {
    auto& state = node_updates_[node_addr];

    // Check queue size limit
    if (state.pending_updates.size() >= MAX_UPDATES_PER_NODE) {
        printf("Update queue full for node 0x%04X\n", node_addr);
        return false;
    }

    // Create update
    PendingUpdate update;
    update.type = UpdateType::SET_SCHEDULE;
    update.queued_at_ms = TimeUtils::getCurrentTimeMs();
    update.sequence = state.next_sequence++;

    // Pack schedule data (8 bytes)
    update.data[0] = index;
    update.data[1] = entry.hour;
    update.data[2] = entry.minute;
    update.data[3] = entry.duration & 0xFF;
    update.data[4] = (entry.duration >> 8) & 0xFF;
    update.data[5] = static_cast<uint8_t>(entry.daysMask);
    update.data[6] = entry.valveId;
    update.data[7] = entry.enabled ? 1 : 0;
    update.data_length = 8;

    // Add to queue
    state.pending_updates.push(update);

    printf("Queued schedule update for node 0x%04X (seq=%d, pos=%zu)\n",
           node_addr, update.sequence, state.pending_updates.size());

    return true;
}

void HubRouter::handleCheckUpdates(uint16_t node_addr, uint8_t node_sequence) {
    auto& state = node_updates_[node_addr];
    state.last_check_time = TimeUtils::getCurrentTimeMs();

    // Check if queue is empty
    if (state.pending_updates.empty()) {
        printf("Node 0x%04X: No updates available\n", node_addr);

        // Send empty response
        uint8_t buffer[MESSAGE_MAX_SIZE];
        size_t length = MessageHandler::createUpdateAvailableMessage(
            ADDRESS_HUB, node_addr, messenger_.getNextSequenceNumber(),
            false, 0, 0, nullptr, buffer
        );
        messenger_.send(buffer, length, RELIABLE);
        return;
    }

    // Get next update (peek, don't remove until ACK)
    const PendingUpdate& update = state.pending_updates.front();

    printf("Node 0x%04X: Sending update seq=%d type=%d\n",
           node_addr, update.sequence, static_cast<int>(update.type));

    // Send update
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length = MessageHandler::createUpdateAvailableMessage(
        ADDRESS_HUB, node_addr, messenger_.getNextSequenceNumber(),
        true, static_cast<uint8_t>(update.type), update.sequence,
        update.data, buffer
    );
    messenger_.send(buffer, length, RELIABLE);
}

void HubRouter::handleUpdateAck(uint16_t node_addr, uint8_t sequence,
                               bool success, uint8_t error_code) {
    auto it = node_updates_.find(node_addr);
    if (it == node_updates_.end() || it->second.pending_updates.empty()) {
        printf("Node 0x%04X: ACK for unknown update seq=%d\n", node_addr, sequence);
        return;
    }

    auto& state = it->second;
    const PendingUpdate& update = state.pending_updates.front();

    // Verify sequence matches
    if (update.sequence != sequence) {
        printf("Node 0x%04X: ACK sequence mismatch (expected %d, got %d)\n",
               node_addr, update.sequence, sequence);
        return;
    }

    if (success) {
        printf("Node 0x%04X: Update seq=%d applied successfully\n",
               node_addr, sequence);

        // Remove from queue
        state.pending_updates.pop();
    } else {
        printf("Node 0x%04X: Update seq=%d failed (error=%d)\n",
               node_addr, sequence, error_code);

        // TODO: Retry logic or remove and report failure
        state.pending_updates.pop();
    }
}

size_t HubRouter::getPendingUpdateCount(uint16_t node_addr) const {
    auto it = node_updates_.find(node_addr);
    return (it != node_updates_.end()) ? it->second.pending_updates.size() : 0;
}
```

**Testing**: Unit test queue operations (queue, peek, ack, remove).

---

### Phase 3: Serial Command Interface
**Estimated**: 4-5 hours

**NEW FILE**: `src/hub/serial_interface.h`

```cpp
#pragma once

#include <cstdint>
#include <cstddef>
#include "../lora/hub_router.h"
#include "../lora/address_manager.h"

/**
 * @brief Serial command interface for Raspberry Pi communication
 *
 * Implements text-based protocol for REST API to control hub:
 * - LIST_NODES: Query registered nodes
 * - GET_QUEUE: Query pending updates for node
 * - SET_SCHEDULE: Queue schedule update
 * - REMOVE_SCHEDULE: Queue schedule removal
 * - SET_WAKE_INTERVAL: Queue wake interval change
 * - GET_DATETIME: Hub queries API for current time
 */
class SerialInterface {
public:
    SerialInterface(HubRouter& router, AddressManager& addr_mgr);

    /**
     * @brief Process incoming serial input
     * Call from main loop to handle commands
     */
    void processInput();

    /**
     * @brief Send datetime query to API
     * Returns true if query sent, false if not ready
     */
    bool queryDateTime();

private:
    HubRouter& router_;
    AddressManager& addr_mgr_;

    char input_buffer_[256];
    size_t input_pos_;

    // Last datetime query
    uint32_t last_datetime_query_ms_;
    static constexpr uint32_t DATETIME_QUERY_INTERVAL_MS = 3600000;  // 1 hour

    // Command handlers
    void handleCommand(const char* cmd);
    void handleListNodes();
    void handleGetQueue(const char* args);
    void handleSetSchedule(const char* args);
    void handleRemoveSchedule(const char* args);
    void handleSetWakeInterval(const char* args);
    void handleDateTimeResponse(const char* args);

    // Helper functions
    void sendResponse(const char* response);
    void sendEvent(const char* event);
    bool parseScheduleArgs(const char* args, uint16_t& node_addr, uint8_t& index,
                          uint8_t& hour, uint8_t& minute, uint16_t& duration,
                          uint8_t& days, uint8_t& valve);
};
```

**NEW FILE**: `src/hub/serial_interface.cpp`

```cpp
#include "serial_interface.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "../utils/time_utils.h"
#include "../hal/pmu_protocol.h"

SerialInterface::SerialInterface(HubRouter& router, AddressManager& addr_mgr)
    : router_(router)
    , addr_mgr_(addr_mgr)
    , input_pos_(0)
    , last_datetime_query_ms_(0) {
    memset(input_buffer_, 0, sizeof(input_buffer_));
}

void SerialInterface::processInput() {
    // Read available characters
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\n' || c == '\r') {
            if (input_pos_ > 0) {
                input_buffer_[input_pos_] = '\0';
                handleCommand(input_buffer_);
                input_pos_ = 0;
            }
        } else if (input_pos_ < sizeof(input_buffer_) - 1) {
            input_buffer_[input_pos_++] = c;
        }
    }
}

bool SerialInterface::queryDateTime() {
    uint32_t now = TimeUtils::getCurrentTimeMs();

    // Rate limit queries
    if (now - last_datetime_query_ms_ < DATETIME_QUERY_INTERVAL_MS) {
        return false;
    }

    printf("GET_DATETIME\n");
    last_datetime_query_ms_ = now;
    return true;
}

void SerialInterface::handleCommand(const char* cmd) {
    // Skip empty lines
    if (!cmd || cmd[0] == '\0') {
        return;
    }

    // Parse command
    if (strcmp(cmd, "LIST_NODES") == 0) {
        handleListNodes();
    } else if (strncmp(cmd, "GET_QUEUE ", 10) == 0) {
        handleGetQueue(cmd + 10);
    } else if (strncmp(cmd, "SET_SCHEDULE ", 13) == 0) {
        handleSetSchedule(cmd + 13);
    } else if (strncmp(cmd, "REMOVE_SCHEDULE ", 16) == 0) {
        handleRemoveSchedule(cmd + 16);
    } else if (strncmp(cmd, "SET_WAKE_INTERVAL ", 18) == 0) {
        handleSetWakeInterval(cmd + 18);
    } else if (strncmp(cmd, "DATETIME ", 9) == 0) {
        handleDateTimeResponse(cmd + 9);
    } else {
        printf("ERROR Unknown command: %s\n", cmd);
    }
}

void SerialInterface::handleListNodes() {
    // Get node count
    uint32_t count = addr_mgr_.getRegisteredNodeCount();

    // Send header
    printf("NODE_LIST %lu\n", count);

    // Iterate all registered nodes
    for (uint16_t addr = ADDRESS_MIN_NODE; addr <= ADDRESS_MAX_NODE; addr++) {
        const NodeInfo* node = addr_mgr_.getNodeInfo(addr);
        if (node) {
            // Determine node type
            const char* type = "UNKNOWN";
            if (node->capabilities & CAP_VALVE_CONTROL) {
                type = "IRRIGATION";
            } else if (node->capabilities & CAP_TEMPERATURE) {
                type = "SENSOR";
            }

            // Calculate last seen
            uint32_t now = TimeUtils::getCurrentTimeMs();
            uint32_t last_seen_sec = (now - node->last_seen_time) / 1000;

            // Send node info
            printf("NODE %u %s %d %lu\n",
                   addr, type,
                   node->is_active ? 1 : 0,
                   last_seen_sec);
        }
    }
}

void SerialInterface::handleGetQueue(const char* args) {
    // Parse node address
    uint16_t node_addr = atoi(args);

    if (node_addr < ADDRESS_MIN_NODE || node_addr > ADDRESS_MAX_NODE) {
        printf("ERROR Invalid node address\n");
        return;
    }

    // Get queue size
    size_t count = router_.getPendingUpdateCount(node_addr);

    // Send header
    printf("QUEUE %u %zu\n", node_addr, count);

    // TODO: Implement queue iteration to print individual updates
    // For now, just return count
}

void SerialInterface::handleSetSchedule(const char* args) {
    // Parse: <addr> <index> <hour>:<min> <duration> <days> <valve>
    uint16_t node_addr, duration;
    uint8_t index, hour, minute, days, valve;

    if (!parseScheduleArgs(args, node_addr, index, hour, minute,
                          duration, days, valve)) {
        printf("ERROR Invalid SET_SCHEDULE syntax\n");
        return;
    }

    // Create schedule entry
    PMU::ScheduleEntry entry;
    entry.hour = hour;
    entry.minute = minute;
    entry.duration = duration;
    entry.daysMask = static_cast<PMU::DayOfWeek>(days);
    entry.valveId = valve;
    entry.enabled = true;

    // Queue update
    if (router_.queueScheduleUpdate(node_addr, index, entry)) {
        size_t position = router_.getPendingUpdateCount(node_addr);
        printf("QUEUED SET_SCHEDULE %u %zu\n", node_addr, position);
    } else {
        printf("ERROR Failed to queue update\n");
    }
}

void SerialInterface::handleRemoveSchedule(const char* args) {
    // Parse: <addr> <index>
    uint16_t node_addr;
    uint8_t index;

    if (sscanf(args, "%hu %hhu", &node_addr, &index) != 2) {
        printf("ERROR Invalid REMOVE_SCHEDULE syntax\n");
        return;
    }

    // Queue removal
    if (router_.queueRemoveSchedule(node_addr, index)) {
        size_t position = router_.getPendingUpdateCount(node_addr);
        printf("QUEUED REMOVE_SCHEDULE %u %zu\n", node_addr, position);
    } else {
        printf("ERROR Failed to queue removal\n");
    }
}

void SerialInterface::handleSetWakeInterval(const char* args) {
    // Parse: <addr> <seconds>
    uint16_t node_addr, interval;

    if (sscanf(args, "%hu %hu", &node_addr, &interval) != 2) {
        printf("ERROR Invalid SET_WAKE_INTERVAL syntax\n");
        return;
    }

    // Queue update
    if (router_.queueWakeIntervalUpdate(node_addr, interval)) {
        size_t position = router_.getPendingUpdateCount(node_addr);
        printf("QUEUED SET_WAKE_INTERVAL %u %zu\n", node_addr, position);
    } else {
        printf("ERROR Failed to queue update\n");
    }
}

void SerialInterface::handleDateTimeResponse(const char* args) {
    // Parse: YYYY-MM-DD HH:MM:SS <weekday>
    // Example: 2025-01-15 14:30:00 3

    int year, month, day, hour, minute, second, weekday;
    if (sscanf(args, "%d-%d-%d %d:%d:%d %d",
               &year, &month, &day, &hour, &minute, &second, &weekday) != 7) {
        printf("ERROR Invalid DATETIME response format\n");
        return;
    }

    printf("Received datetime: %04d-%02d-%02d %02d:%02d:%02d (day %d)\n",
           year, month, day, hour, minute, second, weekday);

    // TODO: Store for scheduling nodes
    // For now, just acknowledge receipt
}

bool SerialInterface::parseScheduleArgs(const char* args, uint16_t& node_addr,
                                       uint8_t& index, uint8_t& hour,
                                       uint8_t& minute, uint16_t& duration,
                                       uint8_t& days, uint8_t& valve) {
    // Parse: <addr> <index> <hour>:<min> <duration> <days> <valve>
    // Example: 42 0 14:30 900 127 0

    int addr, idx, h, m, dur, d, v;
    if (sscanf(args, "%d %d %d:%d %d %d %d",
               &addr, &idx, &h, &m, &dur, &d, &v) != 7) {
        return false;
    }

    node_addr = addr;
    index = idx;
    hour = h;
    minute = m;
    duration = dur;
    days = d;
    valve = v;

    return true;
}

void SerialInterface::sendResponse(const char* response) {
    printf("%s\n", response);
}

void SerialInterface::sendEvent(const char* event) {
    printf("%s\n", event);
}
```

**Testing**:
- Manual serial commands via USB terminal
- Verify responses match API expectations
- Test queue operations end-to-end

---

### Phase 4: HubMode Integration
**Estimated**: 1-2 hours

**File**: `src/modes/hub_mode.h`

Add member (around line 8):
```cpp
private:
    std::unique_ptr<SerialInterface> serial_interface_;
```

**File**: `src/modes/hub_mode.cpp`

Include header (top of file):
```cpp
#include "hub/serial_interface.h"
```

Initialize in `onStart()` (around line 70):
```cpp
// Initialize serial command interface
serial_interface_ = std::make_unique<SerialInterface>(*hub_router_, *address_manager_);
printf("Serial command interface initialized\n");

// Query datetime from API on startup
serial_interface_->queryDateTime();
```

Add task for serial processing (in `onStart()` after other tasks):
```cpp
// Add serial processing task (high frequency)
task_manager_.addTask(
    [this](uint32_t time) {
        serial_interface_->processInput();
    },
    100,  // Check every 100ms
    "Serial Processing"
);

// Add periodic datetime query
task_manager_.addTask(
    [this](uint32_t time) {
        serial_interface_->queryDateTime();
    },
    3600000,  // Every hour
    "DateTime Query"
);
```

**Testing**:
- Hub responds to LIST_NODES
- Hub responds to SET_SCHEDULE
- Queue operations work via serial

---

### Phase 5: Application Mode Message Routing
**Estimated**: 1 hour

**File**: `src/modes/application_mode.h`

Add virtual method (around line 30):
```cpp
protected:
    // Virtual handlers for derived classes
    virtual void onSensorData(const SensorPayload* payload) {}
    virtual void onActuatorCommand(const ActuatorPayload* payload) {}
    virtual void onHeartbeat(const HeartbeatPayload* payload) {}
    virtual void onUpdateAvailable(const UpdateAvailablePayload* payload) {}  // NEW
```

**File**: `src/modes/application_mode.cpp`

Add handlers in `processIncomingMessage()` (around line 100):
```cpp
case MSG_TYPE_CHECK_UPDATES:
    if (IS_HUB && hub_router_) {
        const CheckUpdatesPayload* payload =
            reinterpret_cast<const CheckUpdatesPayload*>(message.payload.raw);
        hub_router_->handleCheckUpdates(header->src_addr,
                                       payload->node_sequence);
    }
    break;

case MSG_TYPE_UPDATE_AVAILABLE:
    if (!IS_HUB) {
        const UpdateAvailablePayload* payload =
            reinterpret_cast<const UpdateAvailablePayload*>(message.payload.raw);
        onUpdateAvailable(payload);  // Virtual method
    }
    break;

case MSG_TYPE_UPDATE_ACK:
    if (IS_HUB && hub_router_) {
        const UpdateAckPayload* payload =
            reinterpret_cast<const UpdateAckPayload*>(message.payload.raw);
        hub_router_->handleUpdateAck(header->src_addr,
                                     payload->sequence,
                                     payload->success != 0,
                                     payload->error_code);
    }
    break;
```

**Testing**: Message routing to correct handlers.

---

### Phase 6: Irrigation Node Pull Logic
**Estimated**: 4-5 hours

**File**: `src/modes/irrigation_mode.h`

Add members:
```cpp
private:
    // Update handling state
    bool awaiting_updates_;
    uint32_t update_check_time_;
    uint32_t last_keep_awake_ms_;
    uint8_t update_sequence_;

    // Methods
    void sendCheckUpdates();
    void applyScheduleUpdate(const uint8_t* data);
    void applyRemoveSchedule(const uint8_t* data);
    void applyDateTimeUpdate(const uint8_t* data);
    void applyWakeIntervalUpdate(const uint8_t* data);
    void sendUpdateAck(uint8_t sequence, bool success, uint8_t error_code);

protected:
    void onUpdateAvailable(const UpdateAvailablePayload* payload) override;
```

**File**: `src/modes/irrigation_mode.cpp`

Initialize in constructor:
```cpp
IrrigationMode::IrrigationMode(...)
    : ApplicationMode(...)
    , awaiting_updates_(false)
    , update_check_time_(0)
    , last_keep_awake_ms_(0)
    , update_sequence_(0) {
}
```

Modify `handlePmuWake()` for periodic wake:
```cpp
case PMU::WakeReason::Periodic:
    pmu_logger.info("Periodic wake - checking for updates");

    // Keep awake while processing
    pmu_client_->keepAwake();
    last_keep_awake_ms_ = to_ms_since_boot(get_absolute_time());

    // Send heartbeat first
    // ... existing heartbeat code ...

    // Check for updates
    sendCheckUpdates();
    awaiting_updates_ = true;
    update_check_time_ = to_ms_since_boot(get_absolute_time());
    break;
```

Implement update handlers:
```cpp
void IrrigationMode::sendCheckUpdates() {
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length = MessageHandler::createCheckUpdatesMessage(
        messenger_.getAddress(),
        HUB_ADDRESS,
        messenger_.getNextSequenceNumber(),
        update_sequence_,
        buffer
    );

    messenger_.send(buffer, length, RELIABLE);
    pmu_logger.debug("Sent CHECK_UPDATES");
}

void IrrigationMode::onUpdateAvailable(const UpdateAvailablePayload* payload) {
    if (!payload->has_update) {
        pmu_logger.debug("No updates available");
        awaiting_updates_ = false;
        return;
    }

    pmu_logger.info("Update: type=%d, seq=%d",
                   payload->update_type, payload->sequence);

    update_sequence_ = payload->sequence;

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
    }
}

void IrrigationMode::applyScheduleUpdate(const uint8_t* data) {
    // Parse schedule from payload
    PMU::ScheduleEntry entry;
    uint8_t index = data[0];
    entry.hour = data[1];
    entry.minute = data[2];
    entry.duration = (data[4] << 8) | data[3];
    entry.daysMask = static_cast<PMU::DayOfWeek>(data[5]);
    entry.valveId = data[6];
    entry.enabled = (data[7] != 0);

    pmu_logger.info("Applying schedule %d: %02d:%02d dur=%d valve=%d",
                   index, entry.hour, entry.minute, entry.duration, entry.valveId);

    auto& protocol = pmu_client_->getProtocol();
    protocol.setSchedule(entry, [this](bool success, PMU::ErrorCode error) {
        sendUpdateAck(update_sequence_, success, static_cast<uint8_t>(error));

        if (success) {
            pmu_logger.info("Schedule applied");
        } else {
            pmu_logger.error("Schedule failed: %d", static_cast<int>(error));
        }

        // Check for more updates
        sendCheckUpdates();
    });
}

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

Update `onLoop()` for timeout:
```cpp
void IrrigationMode::onLoop() {
    // Process PMU messages
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }

    // Handle update check timeout
    if (awaiting_updates_) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Keep awake
        if (now - last_keep_awake_ms_ > 1000) {
            pmu_client_->keepAwake();
            last_keep_awake_ms_ = now;
        }

        // Timeout after 5 seconds
        if (now - update_check_time_ > 5000) {
            awaiting_updates_ = false;
            pmu_logger.info("Update check timeout");
        }
    }
}
```

**Remove hardcoded test schedule** from `onStart()` (lines 45-106).

**Testing**:
- Node wakes, queries hub
- Hub sends update
- Node applies to PMU
- Node sends ACK
- Hub removes from queue

---

## Testing Plan

### Unit Tests
1. **Queue operations**: Add, peek, remove, count
2. **Message parsing**: Parse schedule args, datetime, etc.
3. **Serial protocol**: Send commands, verify responses

### Integration Tests
1. **Serial interface**:
   - Send `LIST_NODES` via USB, verify response
   - Send `SET_SCHEDULE`, verify queued
   - Send `GET_QUEUE`, verify shows update

2. **LoRa update flow**:
   - Queue schedule while node asleep
   - Wait for node wake (~30s)
   - Verify CHECK_UPDATES sent
   - Verify UPDATE_AVAILABLE received
   - Verify schedule applied to PMU
   - Verify UPDATE_ACK sent
   - Verify update removed from queue

3. **End-to-end**:
   - REST API `POST /api/nodes/42/schedules`
   - Serial command sent to hub
   - Update queued
   - Node wakes, receives update
   - Schedule executes at configured time
   - Valve opens/closes

### Edge Cases
1. **Queue overflow**: Queue 11 updates (limit 10), verify oldest dropped
2. **Node offline**: Queue updates, node doesn't wake, verify timeout
3. **PMU rejection**: Send invalid schedule, verify NACK
4. **Multiple updates**: Queue 3 schedules, verify batch processing
5. **Hub reboot**: Queue lost (expected - no persistence yet)

## File Checklist

### New Files (2)
- [ ] `src/hub/serial_interface.h`
- [ ] `src/hub/serial_interface.cpp`

### Modified Files (9)
- [ ] `src/lora/message.h` - Add message types, UpdateType, payloads
- [ ] `src/lora/message.cpp` - Add MessageHandler helpers
- [ ] `src/lora/hub_router.h` - Add update queue structures
- [ ] `src/lora/hub_router.cpp` - Implement queue management
- [ ] `src/modes/hub_mode.h` - Add serial_interface_ member
- [ ] `src/modes/hub_mode.cpp` - Initialize and process serial
- [ ] `src/modes/irrigation_mode.h` - Add update state and methods
- [ ] `src/modes/irrigation_mode.cpp` - Implement pull logic
- [ ] `src/modes/application_mode.h` - Add onUpdateAvailable() virtual
- [ ] `src/modes/application_mode.cpp` - Route update messages

### CMakeLists.txt
- [ ] Add `src/hub/serial_interface.cpp` to build

## Success Criteria

- ✅ Hub responds to all serial commands correctly
- ✅ Update queue works (add, retrieve, remove)
- ✅ Node pull model works (wake → query → apply → ACK)
- ✅ Schedules created via REST API execute on nodes
- ✅ Multiple updates processed in batch before sleep
- ✅ Timeout allows node to sleep if no updates
- ✅ PMU integration works (setSchedule, setDateTime, etc.)

## Estimated Timeline

- Phase 1: Message Protocol - 2-3 hours
- Phase 2: Hub Update Queue - 3-4 hours
- Phase 3: Serial Interface - 4-5 hours
- Phase 4: HubMode Integration - 1-2 hours
- Phase 5: Application Mode Routing - 1 hour
- Phase 6: Irrigation Node Pull Logic - 4-5 hours
- Testing & Debug - 3-4 hours

**Total: 18-24 hours (~3 days)**

## Migration to Redis (Future)

When ready to add persistence:

1. Add `queue_service.py` module to API
2. Use Redis `LPUSH`/`RPOP` for queue
3. Hub serial protocol unchanged
4. API stores in Redis + forwards to hub
5. Hub still caches updates locally
6. On hub reboot, re-sync from Redis

## Notes

- Queue size limited to 10 updates/node (RP2040 RAM constraint)
- Updates expire after 1 hour (configurable)
- No persistence on hub (queue lost on reboot)
- DateTime sync from API to hub to nodes
- All serial communication via USB stdio (115200 baud)
