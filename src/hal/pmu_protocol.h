#ifndef PMU_PROTOCOL_H
#define PMU_PROTOCOL_H

#include <cstdint>
#include <functional>

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
    GetDateTime = 0x18     // Get RTC date/time from PMU (returns DateTimeResponse)
};

// Response codes (STM32 → RP2040)
enum class Response : uint8_t {
    Ack = 0x80,
    Nack = 0x81,
    WakeInterval = 0x82,
    ScheduleEntry = 0x83,
    WakeReason = 0x84,
    Status = 0x85,
    ScheduleComplete = 0x86,
    DateTimeResponse = 0x87  // Response to GetDateTime: valid flag + 7 datetime bytes
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
constexpr uint8_t MAX_MESSAGE_SIZE = 64;
constexpr uint8_t SCHEDULE_ENTRY_SIZE = 7;
constexpr uint8_t NODE_STATE_SIZE = 32;  // Opaque state blob stored in PMU RAM

// Sequence number ranges (for deduplication)
constexpr uint8_t SEQ_RP2040_MIN = 1;
constexpr uint8_t SEQ_RP2040_MAX = 127;
constexpr uint8_t SEQ_STM32_MIN = 128;
constexpr uint8_t SEQ_STM32_MAX = 254;

// Date/Time structure for setting RTC
struct DateTime {
    uint8_t year;     // Years since 2000 (0-99)
    uint8_t month;    // 1-12
    uint8_t day;      // 1-31
    uint8_t weekday;  // 0=Sunday, 1=Monday, ..., 6=Saturday
    uint8_t hour;     // 0-23
    uint8_t minute;   // 0-59
    uint8_t second;   // 0-59

    DateTime() : year(0), month(1), day(1), weekday(0), hour(0), minute(0), second(0) {}

    DateTime(uint8_t y, uint8_t mon, uint8_t d, uint8_t wd, uint8_t h, uint8_t min, uint8_t s)
        : year(y), month(mon), day(d), weekday(wd), hour(h), minute(min), second(s)
    {
    }
};

// Schedule entry structure
struct ScheduleEntry {
    uint8_t hour;       // 0-23
    uint8_t minute;     // 0-59
    uint16_t duration;  // Duration in seconds
    DayOfWeek daysMask;
    uint8_t valveId;
    bool enabled;

    ScheduleEntry()
        : hour(0), minute(0), duration(0), daysMask(static_cast<DayOfWeek>(0)), valveId(0),
          enabled(false)
    {
    }
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

    // Get the response type from completed message
    Response getResponse() const;

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
        ReadResponse,
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
    void startMessage(uint8_t sequenceNumber, Command command);

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

    // Get the sequence number used in this message
    uint8_t getSequenceNumber() const { return sequenceNumber_; }

private:
    uint8_t buffer_[MAX_MESSAGE_SIZE];
    uint8_t dataLength_;   // Length of seq + command + data (for LENGTH field)
    uint8_t totalLength_;  // Total message length including framing
    uint8_t sequenceNumber_;

    uint8_t calculateChecksum() const;
};

// Callback types
using UartSendCallback = std::function<void(const uint8_t *data, uint8_t length)>;
// Wake notification includes state blob from PMU (stored in PMU RAM across sleep cycles)
// state_valid: false on cold start (PMU reset), true if state was preserved
// state: 32-byte opaque blob, only meaningful when state_valid is true
using WakeNotificationCallback = std::function<void(WakeReason reason, const ScheduleEntry *entry,
                                                    bool state_valid, const uint8_t *state)>;
using ScheduleCompleteCallback = std::function<void()>;
using CommandResultCallback = std::function<void(bool success, ErrorCode error)>;
using WakeIntervalCallback = std::function<void(uint32_t seconds)>;
using ScheduleEntryCallback = std::function<void(const ScheduleEntry &entry)>;
using DateTimeCallback = std::function<void(bool valid, const DateTime &datetime)>;

// Callback for ACK with sequence number
using AckCallback = std::function<void(uint8_t seqNum, bool success, ErrorCode error)>;

// Main protocol handler
class Protocol {
public:
    Protocol(UartSendCallback uartSend);

    // Process received byte from UART
    void processReceivedByte(uint8_t byte);

    // Low-level send with explicit sequence number (used by ReliablePmuClient)
    void sendCommand(uint8_t seqNum, Command command, const uint8_t *data, uint8_t dataLength);

    // Set callback for ACK/NACK with sequence number
    void setAckCallback(AckCallback callback);

    // Legacy commands (use internal sequence numbering, not recommended)
    void setWakeInterval(uint32_t seconds, CommandResultCallback callback = nullptr);
    void getWakeInterval(CommandResultCallback callback = nullptr);
    void setSchedule(const ScheduleEntry &entry, CommandResultCallback callback = nullptr);
    void getSchedule(uint8_t index, CommandResultCallback callback = nullptr);
    void clearSchedule(uint8_t index,
                       CommandResultCallback callback = nullptr);  // 0xFF to clear all
    void keepAwake(uint16_t seconds, CommandResultCallback callback = nullptr);
    void setDateTime(const DateTime &dateTime, CommandResultCallback callback = nullptr);
    void readyForSleep(
        CommandResultCallback callback = nullptr);  // Signal work complete, ready for power down
    void getDateTime(DateTimeCallback callback = nullptr);  // Get RTC date/time from PMU

    // Set callback handlers for unsolicited responses
    void onWakeNotification(WakeNotificationCallback callback);
    void onScheduleComplete(ScheduleCompleteCallback callback);
    void onWakeInterval(WakeIntervalCallback callback);
    void onScheduleEntry(ScheduleEntryCallback callback);
    void onDateTime(DateTimeCallback callback);

    // Get next sequence number for legacy API
    uint8_t getNextSequenceNumber();

private:
    MessageParser parser_;
    MessageBuilder builder_;
    UartSendCallback uartSend_;
    uint8_t nextSeqNum_;

    // Response callbacks for unsolicited messages
    WakeNotificationCallback wakeNotificationCallback_;
    ScheduleCompleteCallback scheduleCompleteCallback_;
    WakeIntervalCallback wakeIntervalCallback_;
    ScheduleEntryCallback scheduleEntryCallback_;

    // Pending command callback (for ACK/NACK responses - legacy)
    CommandResultCallback pendingCommandCallback_;

    // ACK callback with sequence number (for ReliablePmuClient)
    AckCallback ackCallback_;

    // Pending datetime callback (for GetDateTime response)
    DateTimeCallback pendingDateTimeCallback_;

    // Response handlers
    void handleAck(uint8_t seqNum);
    void handleNack(uint8_t seqNum, const uint8_t *data, uint8_t length);
    void handleWakeInterval(const uint8_t *data, uint8_t length);
    void handleScheduleEntry(const uint8_t *data, uint8_t length);
    void handleWakeNotification(const uint8_t *data, uint8_t length);
    void handleScheduleComplete();
    void handleDateTimeResponse(const uint8_t *data, uint8_t length);

    // Helper to send built message
    void sendMessage();
};

}  // namespace PMU

#endif  // PMU_PROTOCOL_H
