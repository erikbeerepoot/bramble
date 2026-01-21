# PMU Serial Communication Reliability Architecture

## Overview

This document defines the architecture for reliable serial communication between the STM32 PMU (Power Management Unit) and the RP2040 microcontroller.

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

1. **Layer on existing protocol** - Don't change wire format significantly (backward compatible)
2. **Adapt proven patterns** - Follow ReliableMessenger design from LoRa stack
3. **Criticality-based delivery** - Not all commands need the same reliability
4. **Bidirectional reliability** - Both RP2040→STM32 and STM32→RP2040

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
│   - Criticality-based delivery                              │
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
│   - Timeout watchdog                                        │
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
- 0 and 255 reserved (0 = no-seq/legacy, 255 = broadcast)
- Wrap around within each range

**Enhanced ACK Response:**
```
CURRENT ACK:  [START][01][0x80][CHECKSUM][END]  (no context)
PROPOSED ACK: [START][02][0x80][SEQ_ECHO][CHECKSUM][END]
                                ^^^^^^^^
                          Echo the command's sequence number
```

This allows the sender to know which command was acknowledged.

### 2. Criticality Levels

Adapt the LoRa criticality model for PMU operations:

```cpp
enum class PmuCriticality {
    BEST_EFFORT = 0,   // No retry (status queries)
    RELIABLE = 1,      // 3 retries, 500ms/1s/2s backoff (normal commands)
    CRITICAL = 2       // Persistent retry (power-critical operations)
};
```

**Command Criticality Mapping:**

| Command | Criticality | Rationale |
|---------|-------------|-----------|
| `GetWakeInterval` | BEST_EFFORT | Informational, can retry manually |
| `GetSchedule` | BEST_EFFORT | Informational |
| `SetWakeInterval` | RELIABLE | Important but not time-critical |
| `SetSchedule` | RELIABLE | Important configuration |
| `SetDateTime` | RELIABLE | Time sync is important |
| `ClearSchedule` | RELIABLE | Configuration change |
| `KeepAwake` | CRITICAL | Power-critical, must not miss |
| `ReadyForSleep` | CRITICAL | Power-critical, must complete |

### 3. Retry Policy

```cpp
struct PmuRetryPolicy {
    static constexpr uint32_t BEST_EFFORT_TIMEOUT_MS = 0;      // No timeout
    static constexpr uint32_t RELIABLE_BASE_TIMEOUT_MS = 500;  // Fast for UART
    static constexpr uint32_t RELIABLE_MAX_TIMEOUT_MS = 2000;
    static constexpr uint8_t  RELIABLE_MAX_ATTEMPTS = 3;
    static constexpr uint32_t CRITICAL_BASE_TIMEOUT_MS = 500;
    static constexpr uint32_t CRITICAL_MAX_TIMEOUT_MS = 5000;
    static constexpr uint8_t  CRITICAL_MAX_ATTEMPTS = 10;
    static constexpr bool     CRITICAL_INFINITE_RETRY = true;
};
```

### 4. ReliablePmuClient Class

```cpp
class ReliablePmuClient {
public:
    // Callback types
    using CommandCallback = std::function<void(bool success, PMU::ErrorCode error)>;
    using WakeCallback = std::function<void(PMU::WakeReason reason, const PMU::ScheduleEntry* entry)>;

    ReliablePmuClient(PmuClient* client);

    // High-level reliable commands
    bool setWakeInterval(uint32_t seconds,
                         PmuCriticality criticality = PmuCriticality::RELIABLE,
                         CommandCallback callback = nullptr);

    bool setSchedule(const PMU::ScheduleEntry& entry,
                     PmuCriticality criticality = PmuCriticality::RELIABLE,
                     CommandCallback callback = nullptr);

    bool setDateTime(const PMU::DateTime& dt,
                     PmuCriticality criticality = PmuCriticality::RELIABLE,
                     CommandCallback callback = nullptr);

    bool keepAwake(uint16_t seconds,
                   PmuCriticality criticality = PmuCriticality::CRITICAL,
                   CommandCallback callback = nullptr);

    bool readyForSleep(PmuCriticality criticality = PmuCriticality::CRITICAL,
                       CommandCallback callback = nullptr);

    // Query commands (no reliability needed)
    bool getWakeInterval(PMU::WakeIntervalCallback callback);
    bool getSchedule(uint8_t index, PMU::ScheduleEntryCallback callback);

    // Event callbacks
    void onWake(WakeCallback callback);
    void onScheduleComplete(PMU::ScheduleCompleteCallback callback);

    // Must be called regularly from main loop
    void update();

    // Statistics
    size_t getPendingCount() const;
    uint32_t getRetryCount() const;
    uint32_t getFailureCount() const;

private:
    struct PendingCommand {
        uint8_t seq_num;
        PMU::Command command;
        std::unique_ptr<uint8_t[]> data;
        uint8_t data_length;
        uint32_t send_time;
        uint8_t attempts;
        PmuCriticality criticality;
        CommandCallback callback;
    };

    PmuClient* client_;
    uint8_t next_seq_num_;
    std::map<uint8_t, PendingCommand> pending_;

    // Deduplication (for STM32→RP2040 messages)
    struct SeenMessage {
        uint8_t seq_num;
        uint32_t timestamp;
    };
    SeenMessage seen_[8];  // Small ring buffer
    size_t seen_index_;

    // Statistics
    uint32_t retry_count_;
    uint32_t failure_count_;

    uint8_t getNextSeqNum();
    void handleAck(uint8_t seq_num, PMU::ErrorCode error);
    void checkTimeouts();
    void retryCommand(PendingCommand& cmd);
    bool wasRecentlySeen(uint8_t seq_num);
    void markAsSeen(uint8_t seq_num);
};
```

### 5. Command Queue Management

Unlike LoRa (which can have many pending messages in flight), the PMU serial link is simpler:

**Queue Strategy:**
- Only **one command in flight** at a time (simplifies STM32 implementation)
- Queue additional commands, send next when ACK received or timeout
- Maximum queue depth: 8 commands
- Oldest commands dropped if queue full (with error callback)

```cpp
std::queue<PendingCommand> command_queue_;
std::optional<PendingCommand> in_flight_;
```

### 6. Timeout and Watchdog

**Per-Command Timeout:**
- Start timer when command sent
- On timeout: retry (if attempts remaining) or fail (call callback with error)

**Protocol Watchdog:**
- If no valid response within 10 seconds of any transmission, reset parser
- This handles corruption-induced desync

```cpp
void ReliablePmuClient::update() {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Check for in-flight timeout
    if (in_flight_) {
        uint32_t elapsed = now - in_flight_->send_time;
        uint32_t timeout = getTimeout(in_flight_->criticality, in_flight_->attempts);

        if (elapsed > timeout) {
            if (shouldRetry(in_flight_->criticality, in_flight_->attempts)) {
                retryCommand(*in_flight_);
            } else {
                // Max retries exceeded
                if (in_flight_->callback) {
                    in_flight_->callback(false, PMU::ErrorCode::Timeout);
                }
                failure_count_++;
                in_flight_.reset();
                sendNextQueued();
            }
        }
    }

    // Process received bytes (delegates to PmuClient)
    client_->process();
}
```

### 7. Deduplication (STM32 Side)

The STM32 must also implement deduplication to handle RP2040 retries:

```cpp
// On STM32 side (pmu_protocol.cpp in pmu-stm32/)
bool was_recently_seen(uint8_t seq_num) {
    for (int i = 0; i < SEEN_BUFFER_SIZE; i++) {
        if (seen_buffer[i].seq_num == seq_num &&
            (HAL_GetTick() - seen_buffer[i].timestamp) < DEDUP_WINDOW_MS) {
            return true;
        }
    }
    return false;
}

void process_command(uint8_t seq_num, Command cmd, const uint8_t* data, uint8_t len) {
    // Always send ACK (even for duplicates)
    send_ack(seq_num);

    // But only execute if not a duplicate
    if (!was_recently_seen(seq_num)) {
        mark_as_seen(seq_num);
        execute_command(cmd, data, len);
    }
}
```

### 8. Backward Compatibility

To support gradual migration:

1. **Legacy Detection**: If `seq_num == 0`, treat as legacy (no-seq) message
2. **Version Negotiation**: Optional handshake on startup to detect capabilities
3. **Fallback Mode**: If STM32 doesn't echo seq in ACK, disable reliability features

```cpp
bool ReliablePmuClient::detectCapabilities() {
    // Send a probe command with seq_num
    // If ACK echoes seq_num: full reliability supported
    // If ACK doesn't echo: legacy mode, disable dedup
    return probe_result_;
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
    ┌─────▶│   IN_FLIGHT  │◀────────┐
    │      └──────┬───────┘         │
    │             │                 │
    │    ┌────────┴────────┐        │
    │    ▼                 ▼        │
    │  ┌─────┐         ┌───────┐    │ (retry)
    │  │ ACK │         │TIMEOUT│────┘
    │  └──┬──┘         └───┬───┘
    │     │                │ (max retries)
    │     ▼                ▼
    │ ┌────────┐      ┌────────┐
    └─│SUCCESS │      │ FAILED │
      │callback│      │callback│
      └────────┘      └────────┘
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
5. Implement statistics tracking

### Phase 3: STM32 Deduplication

1. Add seen-message ring buffer
2. Implement duplicate detection
3. Always ACK but skip duplicate execution
4. Test retry scenarios

### Phase 4: Integration & Testing

1. Unit tests for reliability layer
2. Integration tests with simulated failures
3. Stress testing with high retry rates
4. Power-cycle recovery testing

## Error Handling

### Recoverable Errors

| Error | Recovery |
|-------|----------|
| Checksum mismatch | Discard frame, wait for retry |
| Timeout | Automatic retry with backoff |
| Queue full | Drop oldest, callback with error |
| UART framing error | Log, continue (PmuClient handles) |

### Non-Recoverable Errors

| Error | Action |
|-------|--------|
| Max retries exceeded | Callback with failure, log warning |
| STM32 NACK with error | Callback with error code, don't retry |
| Protocol desync | Watchdog reset, re-init |

## Configuration Constants

```cpp
namespace PmuReliability {
    // Timeouts
    constexpr uint32_t RELIABLE_BASE_TIMEOUT_MS = 500;
    constexpr uint32_t CRITICAL_BASE_TIMEOUT_MS = 500;
    constexpr uint32_t WATCHDOG_TIMEOUT_MS = 10000;

    // Retries
    constexpr uint8_t RELIABLE_MAX_ATTEMPTS = 3;
    constexpr uint8_t CRITICAL_MAX_ATTEMPTS = 10;

    // Queue
    constexpr size_t MAX_QUEUE_DEPTH = 8;
    constexpr size_t DEDUP_BUFFER_SIZE = 8;
    constexpr uint32_t DEDUP_WINDOW_MS = 5000;

    // Sequence numbers
    constexpr uint8_t SEQ_RP2040_MIN = 1;
    constexpr uint8_t SEQ_RP2040_MAX = 127;
    constexpr uint8_t SEQ_STM32_MIN = 128;
    constexpr uint8_t SEQ_STM32_MAX = 254;
    constexpr uint8_t SEQ_LEGACY = 0;
    constexpr uint8_t SEQ_BROADCAST = 255;
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

1. Commands with RELIABLE criticality achieve 99%+ delivery
2. CRITICAL commands eventually succeed (infinite retry)
3. No duplicate command execution on STM32
4. Graceful degradation in high-error conditions
5. < 50 bytes additional RAM overhead on STM32
6. < 500 bytes additional RAM overhead on RP2040
7. Backward compatible with existing protocol (legacy mode)

## Open Questions

1. **Queue priority**: Should CRITICAL commands jump the queue?
   - Recommendation: Yes, CRITICAL goes to front of queue

2. **Concurrent commands**: Allow multiple in-flight for independent operations?
   - Recommendation: No, keep it simple with one-at-a-time

3. **STM32 resource constraints**: How much RAM/flash available for dedup buffer?
   - Need to verify: STM32L0 has limited resources

4. **Power implications**: Do retries significantly impact sleep timing?
   - Analysis needed: Retry delays vs. wake schedule
