#ifndef PMU_PROTOCOL_H
#define PMU_PROTOCOL_H

#include <array>
#include <cstdint>

namespace PMU {

// Command codes (RP2040 → STM32)
enum class Command : uint8_t {
    SetWakeInterval = 0x10,
    GetWakeInterval = 0x11,
    SetSchedule = 0x12,
    GetSchedule = 0x13,
    ClearSchedule = 0x14,
    KeepAwake = 0x15,
    SetDateTime =
        0x16,  // Set RTC date/time (7 bytes: year, month, day, weekday, hour, minute, second)
    ReadyForSleep = 0x17,  // RP2040 signals work complete, ready for power down
    GetDateTime = 0x18,    // Get RTC date/time from PMU (returns DateTimeResponse)
    ClearToSend = 0x19,    // RP2040 signals ready to receive wake info
    SystemReset = 0x1A     // Request full system reset (PMU resets itself + RP2040)
};

// Response codes (STM32 → RP2040)
enum class Response : uint8_t {
    Ack = 0x80,
    Nack = 0x81,
    WakeInterval = 0x82,
    ScheduleEntry = 0x83,
    WakeReason = 0x84,
    Status = 0x85,
    ScheduleComplete = 0x86,  // Scheduled watering complete, power down imminent
    DateTimeResponse = 0x87   // Response to GetDateTime: valid flag + 7 datetime bytes
};

// Error codes
enum class ErrorCode : uint8_t {
    NoError = 0x00,
    InvalidParam = 0x01,
    ScheduleFull = 0x02,
    InvalidIndex = 0x03,
    Overlap = 0x04,
    ChecksumError = 0x05
};

// Wake reason codes
enum class WakeReason : uint8_t {
    Periodic = 0x00,
    Scheduled = 0x01,
    External = 0x02
};

// Days of week bitmask
enum class DayOfWeek : uint8_t {
    Sunday = 0x01,
    Monday = 0x02,
    Tuesday = 0x04,
    Wednesday = 0x08,
    Thursday = 0x10,
    Friday = 0x20,
    Saturday = 0x40,
    EveryDay = 0x7F
};

// Bitwise operators for DayOfWeek
inline DayOfWeek operator|(DayOfWeek a, DayOfWeek b)
{
    return static_cast<DayOfWeek>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline DayOfWeek operator&(DayOfWeek a, DayOfWeek b)
{
    return static_cast<DayOfWeek>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool operator!(DayOfWeek a)
{
    return static_cast<uint8_t>(a) == 0;
}

// Protocol constants
constexpr uint8_t START_BYTE = 0xAA;
constexpr uint8_t END_BYTE = 0x55;
constexpr uint8_t MAX_SCHEDULE_ENTRIES = 2;  // Reduced to 2 to fit in 2KB RAM
constexpr uint8_t MAX_MESSAGE_SIZE = 48;     // Increased to accommodate state blob (was 32)
constexpr uint8_t SCHEDULE_ENTRY_SIZE = 7;
constexpr uint8_t NODE_STATE_SIZE = 32;  // Opaque state blob stored in PMU RAM

// Sequence number ranges (for deduplication)
constexpr uint8_t SEQ_RP2040_MIN = 1;
constexpr uint8_t SEQ_RP2040_MAX = 127;
constexpr uint8_t SEQ_STM32_MIN = 128;
constexpr uint8_t SEQ_STM32_MAX = 254;

// Deduplication constants
constexpr uint8_t DEDUP_BUFFER_SIZE = 8;
constexpr uint32_t DEDUP_WINDOW_MS = 5000;

// Time validity magic value - written to RTC backup register when RTC is synced
// This allows the PMU to know if its RTC has ever been synchronized
constexpr uint32_t TIME_VALID_MAGIC = 0xBEEF2025;

// Schedule entry structure
class ScheduleEntry {
public:
    uint8_t hour;
    uint8_t minute;
    uint16_t duration;  // Duration in seconds
    DayOfWeek daysMask;
    uint8_t valveId;
    bool enabled;

    ScheduleEntry();

    // Validation
    bool isValid() const;

    // Check if this entry overlaps with another
    bool overlapsWith(const ScheduleEntry &other) const;

    // Calculate minutes until this entry triggers
    // Returns 0xFFFFFFFF if entry doesn't match the given day
    uint32_t minutesUntil(uint8_t currentDay, uint8_t currentHour, uint8_t currentMinute) const;

    // Check if current time is within this schedule entry's time window
    // windowMinutes: how many minutes past the scheduled time to still consider "active"
    bool isWithinWindow(uint8_t currentDay, uint8_t currentHour, uint8_t currentMinute,
                        uint32_t windowMinutes) const;

    // Check if entry matches a specific day
    bool matchesDay(uint8_t dayOfWeek) const;

private:
    // Helper: check if time ranges overlap
    bool timeRangesOverlap(uint8_t h1, uint8_t m1, uint16_t d1, uint8_t h2, uint8_t m2,
                           uint16_t d2) const;
};

// Watering schedule manager
class WateringSchedule {
public:
    WateringSchedule();

    // Add or update a schedule entry at the first available slot
    ErrorCode addEntry(const ScheduleEntry &entry);

    // Update entry at specific index
    ErrorCode updateEntry(uint8_t index, const ScheduleEntry &entry);

    // Remove entry at index
    ErrorCode removeEntry(uint8_t index);

    // Clear all entries
    void clear();

    // Get entry at index (returns nullptr if invalid)
    const ScheduleEntry *getEntry(uint8_t index) const;

    // Find the next schedule entry that should trigger
    const ScheduleEntry *findNextEntry(uint8_t currentDay, uint8_t currentHour,
                                       uint8_t currentMinute) const;

    // Get count of active entries
    uint8_t getCount() const;

private:
    std::array<ScheduleEntry, MAX_SCHEDULE_ENTRIES> entries_;
    uint8_t count_;

    // Check if entry would overlap with existing entries
    bool hasOverlap(const ScheduleEntry &entry, uint8_t excludeIndex = 0xFF) const;
};

// Message parser (state machine for robust parsing)
class MessageParser {
public:
    MessageParser();

    // Process incoming byte, returns true when complete message received
    bool processByte(uint8_t byte);

    // Check if a complete valid message is ready
    bool isComplete() const;

    // Get the sequence number from completed message
    uint8_t getSequenceNumber() const;

    // Get the command from completed message
    Command getCommand() const;

    // Get pointer to data payload
    const uint8_t *getData() const;

    // Get length of data payload
    uint8_t getDataLength() const;

    // Reset parser to initial state
    void reset();

private:
    enum class State {
        WaitStart,
        ReadLength,
        ReadSequence,
        ReadCommand,
        ReadData,
        ReadChecksum,
        ReadEnd
    };

    State state_;
    uint8_t buffer_[MAX_MESSAGE_SIZE];
    uint8_t bytesRead_;
    uint8_t expectedLength_;
    uint8_t calculatedChecksum_;
    uint8_t sequenceNumber_;
    bool complete_;

    uint8_t calculateChecksum() const;
};

// Message builder
class MessageBuilder {
public:
    MessageBuilder();

    // Start building a new message with sequence number
    void startMessage(uint8_t sequenceNumber, uint8_t response);

    // Add data to message
    void addByte(uint8_t data);
    void addUint16(uint16_t data);
    void addUint32(uint32_t data);
    void addScheduleEntry(const ScheduleEntry &entry);

    // Finalize message (adds checksum and end byte)
    // Returns pointer to complete message buffer
    const uint8_t *finalize();

    // Get total message length (including framing)
    uint8_t getLength() const;

private:
    uint8_t buffer_[MAX_MESSAGE_SIZE];
    uint8_t dataLength_;   // Length of seq + response + data (for LENGTH field)
    uint8_t totalLength_;  // Total message length including framing

    uint8_t calculateChecksum() const;
};

// Deduplication entry
struct SeenMessage {
    uint8_t seqNum;
    uint32_t timestamp;
};

// Main protocol handler
class Protocol {
public:
    // Callback types
    using UartSendCallback = void (*)(const uint8_t *data, uint8_t length);
    using SetWakeCallback = void (*)(uint32_t seconds);
    using KeepAwakeCallback = void (*)(uint16_t seconds);
    using ReadyForSleepCallback = void (*)();
    using GetTickCallback = uint32_t (*)();

    Protocol(UartSendCallback uartSend, SetWakeCallback setWake, KeepAwakeCallback keepAwake,
             ReadyForSleepCallback readyForSleep = nullptr, GetTickCallback getTick = nullptr);

    // Process received byte from UART
    void processReceivedByte(uint8_t byte);

    // Send wake notification to RP2040
    void sendWakeNotification(WakeReason reason);

    // Send wake notification with schedule entry (for scheduled watering events)
    void sendWakeNotificationWithSchedule(WakeReason reason, const ScheduleEntry *entry);

    // Send schedule complete notification (RP2040 should be ready for power down)
    void sendScheduleComplete();

    uint32_t getWakeInterval() const { return wakeInterval_; }

    // Clear-to-send state management
    bool isCtsReceived() const { return clearToSendReceived_; }
    void clearCtsReceived() { clearToSendReceived_ = false; }

    // Clear deduplication buffer - call when starting a new RP2040 boot cycle
    // This is needed because HAL tick is suspended during STOP mode, so old
    // sequence numbers may still appear "recent" after waking up
    void clearDedupBuffer()
    {
        for (auto &entry : seenBuffer_) {
            entry.seqNum = 0;
            entry.timestamp = 0;
        }
    }

    // Get the next scheduled entry (for RTC wakeup checking)
    const ScheduleEntry *getNextScheduledEntry(uint8_t currentDay, uint8_t currentHour,
                                               uint8_t currentMinute) const;

private:
    MessageParser parser_;
    MessageBuilder builder_;
    WateringSchedule schedule_;
    uint32_t wakeInterval_;
    uint8_t nextSeqNum_;
    uint8_t currentSeqNum_;  // Sequence number of current command being processed

    UartSendCallback uartSend_;
    SetWakeCallback setWake_;
    KeepAwakeCallback keepAwake_;
    ReadyForSleepCallback readyForSleep_;
    GetTickCallback getTick_;

    // Deduplication buffer
    SeenMessage seenBuffer_[DEDUP_BUFFER_SIZE];
    uint8_t seenIndex_;

    // Node state storage (persists in PMU RAM across RP2040 sleep cycles)
    // state_valid_ is false after power-on reset (cold start detection)
    uint8_t nodeState_[NODE_STATE_SIZE];
    bool nodeStateValid_;

    // Clear-to-send flag - set when RP2040 signals ready to receive wake info
    bool clearToSendReceived_;

    // Deduplication helpers
    bool wasRecentlySeen(uint8_t seqNum);
    void markAsSeen(uint8_t seqNum);

    // Command handlers
    void handleSetWakeInterval(const uint8_t *data, uint8_t length);
    void handleGetWakeInterval();
    void handleSetSchedule(const uint8_t *data, uint8_t length);
    void handleGetSchedule(const uint8_t *data, uint8_t length);
    void handleClearSchedule(const uint8_t *data, uint8_t length);
    void handleKeepAwake(const uint8_t *data, uint8_t length);
    void handleSetDateTime(const uint8_t *data, uint8_t length);
    void handleReadyForSleep();
    void handleGetDateTime();

    // Response senders (with sequence number echo)
    void sendAck();
    void sendNack(ErrorCode error);
    void sendWakeInterval();
    void sendScheduleEntry(uint8_t index);
    void sendDateTimeResponse(bool valid, uint8_t year, uint8_t month, uint8_t day, uint8_t weekday,
                              uint8_t hour, uint8_t minute, uint8_t second);

    // Helper to send built message
    void sendMessage();

    // Get next sequence number for unsolicited messages
    uint8_t getNextSeqNum();
};

}  // namespace PMU

#endif  // PMU_PROTOCOL_H
