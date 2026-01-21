# PMU Serial Communication Reliability Architecture

## Overview

This document defines the architecture for reliable serial communication between the STM32 PMU (Power Management Unit) and the RP2040 microcontroller. All commands are treated as critical and will retry until acknowledged.

## Current State Analysis

### Existing Implementation

**Wire Protocol** (`pmu_protocol.h`):
```
[START: 0xAA][LENGTH][COMMAND/RESPONSE][DATA...][CHECKSUM][END: 0x55]
```
- START_BYTE: 0xAA
- END_BYTE: 0x55
- MAX_MESSAGE_SIZE: 64 bytes
- Checksum: XOR of bytes after length field
- Baud: 9600, 8N2 (2 stop bits)

**PMU Client** (`pmu_client.h`):
- IRQ-driven RX with 128-byte ring buffer
- Blocking TX
- State machine parser for robust framing
- Callback-based async command/response

### Current Limitations

1. **No Sequence Numbers** - Cannot detect duplicate messages or track delivery
2. **No Timeout Detection** - Caller must manually implement timeouts
3. **No Automatic Retry** - Failures require manual re-sending
4. **No Deduplication** - STM32 may execute commands multiple times if RP2040 retries
5. **Single Pending Command** - Only one `pendingCommandCallback_` at a time
6. **No Error Recovery** - Protocol can get stuck if framing is corrupted

## Proposed Architecture

### Design Principles

1. **All messages are critical** - Retry until success
2. **Simple and robust** - Minimal complexity
3. **Bidirectional reliability** - Both RP2040→STM32 and STM32→RP2040

### Architecture Layers

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                        │
│           (IrrigationMode, SensorMode, etc.)                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│               ReliablePmuClient (NEW)                       │
│   - Sequence numbering & deduplication                      │
│   - Timeout detection & automatic retry                     │
│   - ACK tracking & callbacks                                │
│   - Command queue management                                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│               PMU Protocol (ENHANCED)                       │
│   - Add sequence number to frame header                     │
│   - Enhanced ACK with sequence echo                         │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    PmuClient (UNCHANGED)                    │
│   - UART TX/RX                                              │
│   - IRQ handling                                            │
│   - Ring buffer                                             │
└─────────────────────────────────────────────────────────────┘
```

## Detailed Design

### 1. Enhanced Wire Protocol

Add a sequence number to the existing frame format:

```
CURRENT:  [START][LENGTH][CMD][DATA...][CHECKSUM][END]
PROPOSED: [START][LENGTH][SEQ][CMD][DATA...][CHECKSUM][END]
                          ^^^
                     New: 1 byte
```

**Sequence Number Rules:**
- RP2040 uses range: 1-127
- STM32 uses range: 128-254
- Wrap around within each range

**Enhanced ACK Response:**
```
CURRENT ACK:  [START][01][0x80][CHECKSUM][END]  (no context)
PROPOSED ACK: [START][02][SEQ_ECHO][0x80][CHECKSUM][END]
                          ^^^^^^^^
                    Echo the command's sequence number
```

This allows the sender to know which command was acknowledged.

### 2. Retry Policy

All commands retry with exponential backoff until acknowledged:

```cpp
namespace PmuReliability {
    constexpr uint32_t BASE_TIMEOUT_MS = 500;
    constexpr uint32_t MAX_TIMEOUT_MS = 5000;
    constexpr float BACKOFF_MULTIPLIER = 2.0f;
}
```

**Timeout Calculation:**
```
Attempt 1: 500ms
Attempt 2: 1000ms
Attempt 3: 2000ms
Attempt 4+: 5000ms (capped)
```

### 3. ReliablePmuClient Class

```cpp
class ReliablePmuClient {
public:
    using CommandCallback = std::function<void(bool success, PMU::ErrorCode error)>;
    using WakeCallback = std::function<void(PMU::WakeReason reason, const PMU::ScheduleEntry* entry)>;

    ReliablePmuClient(PmuClient* client);

    // Reliable commands (all retry until success)
    bool setWakeInterval(uint32_t seconds, CommandCallback callback = nullptr);
    bool setSchedule(const PMU::ScheduleEntry& entry, CommandCallback callback = nullptr);
    bool setDateTime(const PMU::DateTime& dt, CommandCallback callback = nullptr);
    bool clearSchedule(CommandCallback callback = nullptr);
    bool keepAwake(uint16_t seconds, CommandCallback callback = nullptr);
    bool readyForSleep(CommandCallback callback = nullptr);

    // Query commands (also reliable)
    bool getWakeInterval(PMU::WakeIntervalCallback callback);
    bool getSchedule(uint8_t index, PMU::ScheduleEntryCallback callback);

    // Event callbacks
    void onWake(WakeCallback callback);
    void onScheduleComplete(PMU::ScheduleCompleteCallback callback);

    // Must be called regularly from main loop
    void update();

    // Status
    size_t getPendingCount() const;
    bool hasPendingCommands() const;

private:
    struct PendingCommand {
        uint8_t seq_num;
        PMU::Command command;
        std::unique_ptr<uint8_t[]> data;
        uint8_t data_length;
        uint32_t send_time;
        uint8_t attempts;
        CommandCallback callback;
    };

    PmuClient* client_;
    uint8_t next_seq_num_;  // Range: 1-127

    // Command queue (one in-flight at a time)
    std::queue<PendingCommand> command_queue_;
    std::optional<PendingCommand> in_flight_;

    // Deduplication for STM32→RP2040 messages
    struct SeenMessage {
        uint8_t seq_num;
        uint32_t timestamp;
    };
    SeenMessage seen_[8];
    size_t seen_index_;

    uint8_t getNextSeqNum();
    uint32_t getTimeout(uint8_t attempts);
    void handleAck(uint8_t seq_num, PMU::ErrorCode error);
    void retryCommand();
    void sendNextQueued();
    bool wasRecentlySeen(uint8_t seq_num);
    void markAsSeen(uint8_t seq_num);
};
```

### 4. Command Queue Management

**Queue Strategy:**
- Only **one command in flight** at a time
- Queue additional commands, send next when ACK received
- Maximum queue depth: 8 commands
- If queue full: block caller (return false)

```cpp
std::queue<PendingCommand> command_queue_;
std::optional<PendingCommand> in_flight_;
```

### 5. Update Loop

```cpp
void ReliablePmuClient::update() {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Check for in-flight timeout
    if (in_flight_) {
        uint32_t elapsed = now - in_flight_->send_time;
        uint32_t timeout = getTimeout(in_flight_->attempts);

        if (elapsed > timeout) {
            retryCommand();
        }
    }

    // Process received bytes
    client_->process();
}

void ReliablePmuClient::retryCommand() {
    in_flight_->attempts++;
    in_flight_->send_time = to_ms_since_boot(get_absolute_time());

    // Re-send the command
    client_->sendRaw(in_flight_->seq_num, in_flight_->command,
                     in_flight_->data.get(), in_flight_->data_length);
}

uint32_t ReliablePmuClient::getTimeout(uint8_t attempts) {
    uint32_t timeout = BASE_TIMEOUT_MS;
    for (uint8_t i = 1; i < attempts && timeout < MAX_TIMEOUT_MS; i++) {
        timeout *= BACKOFF_MULTIPLIER;
    }
    return std::min(timeout, MAX_TIMEOUT_MS);
}
```

### 6. Deduplication (STM32 Side)

The STM32 must implement deduplication to handle RP2040 retries:

```cpp
// On STM32 side (pmu_protocol.cpp)
static constexpr size_t SEEN_BUFFER_SIZE = 8;
static constexpr uint32_t DEDUP_WINDOW_MS = 5000;

struct SeenMessage {
    uint8_t seq_num;
    uint32_t timestamp;
};
static SeenMessage seen_buffer[SEEN_BUFFER_SIZE];
static size_t seen_index = 0;

bool was_recently_seen(uint8_t seq_num) {
    uint32_t now = HAL_GetTick();
    for (size_t i = 0; i < SEEN_BUFFER_SIZE; i++) {
        if (seen_buffer[i].seq_num == seq_num &&
            (now - seen_buffer[i].timestamp) < DEDUP_WINDOW_MS) {
            return true;
        }
    }
    return false;
}

void mark_as_seen(uint8_t seq_num) {
    seen_buffer[seen_index].seq_num = seq_num;
    seen_buffer[seen_index].timestamp = HAL_GetTick();
    seen_index = (seen_index + 1) % SEEN_BUFFER_SIZE;
}

void process_command(uint8_t seq_num, Command cmd, const uint8_t* data, uint8_t len) {
    // Always send ACK (even for duplicates)
    send_ack(seq_num);

    // Only execute if not a duplicate
    if (!was_recently_seen(seq_num)) {
        mark_as_seen(seq_num);
        execute_command(cmd, data, len);
    }
}
```

## State Diagrams

### Command Lifecycle

```
           ┌──────────────┐
           │    QUEUED    │
           └──────┬───────┘
                  │ (slot available)
                  ▼
           ┌──────────────┐
           │   IN_FLIGHT  │◀───────┐
           └──────┬───────┘        │
                  │                │
         ┌────────┴────────┐       │
         ▼                 ▼       │
       ┌─────┐         ┌───────┐   │ (retry forever)
       │ ACK │         │TIMEOUT│───┘
       └──┬──┘         └───────┘
          │
          ▼
      ┌────────┐
      │SUCCESS │
      │callback│
      └────────┘
```

### Parser State Machine (Enhanced)

```
                    ┌──────────────────────────────────────────┐
                    │                                          │
                    ▼                                          │
              ┌──────────┐                                     │
     ───────▶│WAIT_START│                                     │
              └────┬─────┘                                     │
                   │ (0xAA)                                    │
                   ▼                                           │
              ┌──────────┐                                     │
              │READ_LEN  │─────────(invalid)──────────────────▶│
              └────┬─────┘                                     │
                   │                                           │
                   ▼                                           │
              ┌──────────┐                                     │
              │READ_SEQ  │◀──── NEW STATE                      │
              └────┬─────┘                                     │
                   │                                           │
                   ▼                                           │
              ┌──────────┐                                     │
              │READ_CMD  │                                     │
              └────┬─────┘                                     │
                   │                                           │
                   ▼                                           │
              ┌──────────┐                                     │
              │READ_DATA │                                     │
              └────┬─────┘                                     │
                   │ (all data read)                           │
                   ▼                                           │
              ┌──────────┐                                     │
              │READ_CSUM │─────────(mismatch)─────────────────▶│
              └────┬─────┘                                     │
                   │ (match)                                   │
                   ▼                                           │
              ┌──────────┐                                     │
              │READ_END  │─────────(not 0x55)──────────────────┘
              └────┬─────┘
                   │ (0x55)
                   ▼
              ┌──────────┐
              │ COMPLETE │
              └──────────┘
```

## Implementation Plan

### Phase 1: Protocol Enhancement (Both Sides)

1. Add sequence number field to message format
2. Update MessageBuilder to include seq_num
3. Update MessageParser to extract seq_num
4. Enhance ACK to echo seq_num
5. Update checksum calculation to include seq_num

### Phase 2: ReliablePmuClient (RP2040)

1. Create ReliablePmuClient class
2. Implement command queueing
3. Add timeout detection and retry logic
4. Add deduplication for incoming messages

### Phase 3: STM32 Deduplication

1. Add seen-message ring buffer
2. Implement duplicate detection
3. Always ACK but skip duplicate execution
4. Test retry scenarios

### Phase 4: Integration & Testing

1. Unit tests for reliability layer
2. Integration tests with simulated failures
3. Power-cycle recovery testing

## Error Handling

| Error | Recovery |
|-------|----------|
| Checksum mismatch | Discard frame, wait for retry |
| Timeout | Automatic retry with backoff |
| Queue full | Return false to caller |
| UART framing error | Log, continue (PmuClient handles) |
| STM32 NACK with error | Callback with error code, retry anyway |

## Configuration Constants

```cpp
namespace PmuReliability {
    // Timeouts
    constexpr uint32_t BASE_TIMEOUT_MS = 500;
    constexpr uint32_t MAX_TIMEOUT_MS = 5000;
    constexpr float BACKOFF_MULTIPLIER = 2.0f;

    // Queue
    constexpr size_t MAX_QUEUE_DEPTH = 8;

    // Deduplication
    constexpr size_t DEDUP_BUFFER_SIZE = 8;
    constexpr uint32_t DEDUP_WINDOW_MS = 5000;

    // Sequence numbers
    constexpr uint8_t SEQ_RP2040_MIN = 1;
    constexpr uint8_t SEQ_RP2040_MAX = 127;
    constexpr uint8_t SEQ_STM32_MIN = 128;
    constexpr uint8_t SEQ_STM32_MAX = 254;
}
```

## File Structure

```
bramble/
├── src/
│   ├── hal/
│   │   ├── pmu_client.h/.cpp           # UNCHANGED - UART layer
│   │   ├── pmu_protocol.h/.cpp         # ENHANCED - add seq_num
│   │   ├── pmu_reliability.h           # NEW - ReliablePmuClient
│   │   └── pmu_reliability.cpp         # NEW
│
├── pmu-stm32/
│   └── Core/
│       └── Src/
│           └── pmu_protocol.cpp        # ENHANCED - add dedup
```

## Success Criteria

1. All commands eventually succeed (infinite retry)
2. No duplicate command execution on STM32
3. < 50 bytes additional RAM overhead on STM32
4. < 500 bytes additional RAM overhead on RP2040
