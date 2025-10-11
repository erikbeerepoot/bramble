#ifndef PMU_PROTOCOL_H
#define PMU_PROTOCOL_H

#include <cstdint>
#include <array>

namespace PMU {

// Command codes (RP2040 → STM32)
enum class Command : uint8_t {
    SetWakeInterval = 0x10,
    GetWakeInterval = 0x11,
    SetSchedule = 0x12,
    GetSchedule = 0x13,
    ClearSchedule = 0x14,
    KeepAwake = 0x15
};

// Response codes (STM32 → RP2040)
enum class Response : uint8_t {
    Ack = 0x80,
    Nack = 0x81,
    WakeInterval = 0x82,
    ScheduleEntry = 0x83,
    WakeReason = 0x84,
    Status = 0x85,
    ScheduleComplete = 0x86  // Scheduled watering complete, power down imminent
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
inline DayOfWeek operator|(DayOfWeek a, DayOfWeek b) {
    return static_cast<DayOfWeek>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline DayOfWeek operator&(DayOfWeek a, DayOfWeek b) {
    return static_cast<DayOfWeek>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool operator!(DayOfWeek a) {
    return static_cast<uint8_t>(a) == 0;
}

// Protocol constants
constexpr uint8_t START_BYTE = 0xAA;
constexpr uint8_t END_BYTE = 0x55;
constexpr uint8_t MAX_SCHEDULE_ENTRIES = 8;
constexpr uint8_t MAX_MESSAGE_SIZE = 64;
constexpr uint8_t SCHEDULE_ENTRY_SIZE = 7;

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
    bool overlapsWith(const ScheduleEntry& other) const;

    // Calculate minutes until this entry triggers
    // Returns 0xFFFFFFFF if entry doesn't match the given day
    uint32_t minutesUntil(uint8_t currentDay, uint8_t currentHour, uint8_t currentMinute) const;

    // Check if entry matches a specific day
    bool matchesDay(uint8_t dayOfWeek) const;

private:
    // Helper: check if time ranges overlap
    bool timeRangesOverlap(uint8_t h1, uint8_t m1, uint16_t d1,
                          uint8_t h2, uint8_t m2, uint16_t d2) const;
};

// Watering schedule manager
class WateringSchedule {
public:
    WateringSchedule();

    // Add or update a schedule entry at the first available slot
    ErrorCode addEntry(const ScheduleEntry& entry);

    // Update entry at specific index
    ErrorCode updateEntry(uint8_t index, const ScheduleEntry& entry);

    // Remove entry at index
    ErrorCode removeEntry(uint8_t index);

    // Clear all entries
    void clear();

    // Get entry at index (returns nullptr if invalid)
    const ScheduleEntry* getEntry(uint8_t index) const;

    // Find the next schedule entry that should trigger
    const ScheduleEntry* findNextEntry(uint8_t currentDay, uint8_t currentHour,
                                      uint8_t currentMinute) const;

    // Get count of active entries
    uint8_t getCount() const;

private:
    std::array<ScheduleEntry, MAX_SCHEDULE_ENTRIES> entries_;
    uint8_t count_;

    // Check if entry would overlap with existing entries
    bool hasOverlap(const ScheduleEntry& entry, uint8_t excludeIndex = 0xFF) const;
};

// Message parser (state machine for robust parsing)
class MessageParser {
public:
    MessageParser();

    // Process incoming byte, returns true when complete message received
    bool processByte(uint8_t byte);

    // Check if a complete valid message is ready
    bool isComplete() const;

    // Get the command from completed message
    Command getCommand() const;

    // Get pointer to data payload
    const uint8_t* getData() const;

    // Get length of data payload
    uint8_t getDataLength() const;

    // Reset parser to initial state
    void reset();

private:
    enum class State {
        WaitStart,
        ReadLength,
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
    bool complete_;

    uint8_t calculateChecksum() const;
};

// Message builder
class MessageBuilder {
public:
    MessageBuilder();

    // Start building a new message
    void startMessage(uint8_t command);

    // Add data to message
    void addByte(uint8_t data);
    void addUint16(uint16_t data);
    void addUint32(uint32_t data);
    void addScheduleEntry(const ScheduleEntry& entry);

    // Finalize message (adds checksum and end byte)
    // Returns pointer to complete message buffer
    const uint8_t* finalize();

    // Get total message length (including framing)
    uint8_t getLength() const;

private:
    uint8_t buffer_[MAX_MESSAGE_SIZE];
    uint8_t dataLength_;  // Length of command + data (for LENGTH field)
    uint8_t totalLength_; // Total message length including framing

    uint8_t calculateChecksum() const;
};

// Main protocol handler
class Protocol {
public:
    // Callback types
    using UartSendCallback = void(*)(const uint8_t* data, uint8_t length);
    using SetWakeCallback = void(*)(uint32_t seconds);
    using KeepAwakeCallback = void(*)(uint16_t seconds);

    Protocol(UartSendCallback uartSend, SetWakeCallback setWake, KeepAwakeCallback keepAwake);

    // Process received byte from UART
    void processReceivedByte(uint8_t byte);

    // Send wake notification to RP2040
    void sendWakeNotification(WakeReason reason);

    // Send schedule complete notification (RP2040 should be ready for power down)
    void sendScheduleComplete();

    uint32_t getWakeInternal() const { return wakeInterval_; }

    // Get the next scheduled entry (for RTC wakeup checking)
    const ScheduleEntry* getNextScheduledEntry(uint8_t currentDay, uint8_t currentHour,
                                               uint8_t currentMinute) const;

private:
    MessageParser parser_;
    MessageBuilder builder_;
    WateringSchedule schedule_;
    uint32_t wakeInterval_;

    UartSendCallback uartSend_;
    SetWakeCallback setWake_;
    KeepAwakeCallback keepAwake_;

    // Command handlers
    void handleSetWakeInterval(const uint8_t* data, uint8_t length);
    void handleGetWakeInterval();
    void handleSetSchedule(const uint8_t* data, uint8_t length);
    void handleGetSchedule(const uint8_t* data, uint8_t length);
    void handleClearSchedule(const uint8_t* data, uint8_t length);
    void handleKeepAwake(const uint8_t* data, uint8_t length);

    // Response senders
    void sendAck();
    void sendNack(ErrorCode error);
    void sendWakeInterval();
    void sendScheduleEntry(uint8_t index);

    // Helper to send built message
    void sendMessage();
};

}  // namespace PMU

#endif // PMU_PROTOCOL_H
