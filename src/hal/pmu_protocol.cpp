#include "pmu_protocol.h"
#include "hal/logger.h"
#include <cstring>

static Logger log("PMU");

namespace PMU {

// ============================================================================
// MessageParser Implementation
// ============================================================================

MessageParser::MessageParser()
    : state_(State::WaitStart), bytesRead_(0), expectedLength_(0),
      calculatedChecksum_(0), sequenceNumber_(0), complete_(false) {
}

bool MessageParser::processByte(uint8_t byte) {
    switch (state_) {
        case State::WaitStart:
            if (byte == START_BYTE) {
                bytesRead_ = 0;
                complete_ = false;
                sequenceNumber_ = 0;
                state_ = State::ReadLength;
            }
            // Silently ignore non-start bytes (normal during idle)
            break;

        case State::ReadLength:
            expectedLength_ = byte;
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ = byte;
            state_ = State::ReadSequence;
            break;

        case State::ReadSequence:
            sequenceNumber_ = byte;
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ ^= byte;
            state_ = State::ReadResponse;
            break;

        case State::ReadResponse:
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ ^= byte;
            // Length field includes: seq + response + data
            // We've read: length(1) + seq(1) + response(1) = 3 bytes
            // If expectedLength_ == 2 (seq + response only), no data
            if (expectedLength_ == 2) {
                state_ = State::ReadChecksum;
            } else {
                state_ = State::ReadData;
            }
            break;

        case State::ReadData:
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ ^= byte;
            // Check if we've read all data bytes
            // bytesRead_ includes: length(1) + seq(1) + response(1) + data(n)
            // expectedLength_ = seq(1) + response(1) + data(n) = 2 + n
            if (bytesRead_ >= expectedLength_ + 1) {
                state_ = State::ReadChecksum;
            }
            break;

        case State::ReadChecksum:
            if (byte == calculatedChecksum_) {
                state_ = State::ReadEnd;
            } else {
                // Checksum mismatch, reset
                state_ = State::WaitStart;
            }
            break;

        case State::ReadEnd:
            if (byte == END_BYTE) {
                complete_ = true;
                state_ = State::WaitStart;
                return true;
            }
            state_ = State::WaitStart;
            break;
    }

    return false;
}

bool MessageParser::isComplete() const {
    return complete_;
}

uint8_t MessageParser::getSequenceNumber() const {
    return sequenceNumber_;
}

Response MessageParser::getResponse() const {
    // buffer_[0] = length, buffer_[1] = seq, buffer_[2] = response
    if (bytesRead_ < 3) return static_cast<Response>(0);
    return static_cast<Response>(buffer_[2]);
}

const uint8_t* MessageParser::getData() const {
    // Data starts after length(1) + seq(1) + response(1)
    if (bytesRead_ < 3) return nullptr;
    return &buffer_[3];
}

uint8_t MessageParser::getDataLength() const {
    if (bytesRead_ < 3) return 0;
    // expectedLength_ = seq(1) + response(1) + data(n)
    // dataLength = expectedLength_ - 2
    return (expectedLength_ > 2) ? (expectedLength_ - 2) : 0;
}

void MessageParser::reset() {
    state_ = State::WaitStart;
    bytesRead_ = 0;
    expectedLength_ = 0;
    calculatedChecksum_ = 0;
    sequenceNumber_ = 0;
    complete_ = false;
}

uint8_t MessageParser::calculateChecksum() const {
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < bytesRead_; i++) {
        checksum ^= buffer_[i];
    }
    return checksum;
}

// ============================================================================
// MessageBuilder Implementation
// ============================================================================

MessageBuilder::MessageBuilder() : dataLength_(0), totalLength_(0), sequenceNumber_(0) {
}

void MessageBuilder::startMessage(uint8_t sequenceNumber, Command command) {
    buffer_[0] = START_BYTE;
    // buffer_[1] = length (set in finalize)
    buffer_[2] = sequenceNumber;
    buffer_[3] = static_cast<uint8_t>(command);
    sequenceNumber_ = sequenceNumber;
    dataLength_ = 2;  // seq + command
    totalLength_ = 4; // START + LENGTH + SEQ + COMMAND
}

void MessageBuilder::addByte(uint8_t data) {
    buffer_[totalLength_++] = data;
    dataLength_++;
}

void MessageBuilder::addUint16(uint16_t data) {
    // Little-endian
    addByte(data & 0xFF);
    addByte((data >> 8) & 0xFF);
}

void MessageBuilder::addUint32(uint32_t data) {
    // Little-endian
    addByte(data & 0xFF);
    addByte((data >> 8) & 0xFF);
    addByte((data >> 16) & 0xFF);
    addByte((data >> 24) & 0xFF);
}

void MessageBuilder::addScheduleEntry(const ScheduleEntry& entry) {
    addByte(entry.hour);
    addByte(entry.minute);
    addUint16(entry.duration);
    addByte(static_cast<uint8_t>(entry.daysMask));
    addByte(entry.valveId);
    addByte(entry.enabled ? 1 : 0);
}

const uint8_t* MessageBuilder::finalize() {
    // Set length field
    buffer_[1] = dataLength_;

    // Calculate and add checksum
    buffer_[totalLength_++] = calculateChecksum();

    // Add end byte
    buffer_[totalLength_++] = END_BYTE;

    return buffer_;
}

uint8_t MessageBuilder::getLength() const {
    return totalLength_;
}

uint8_t MessageBuilder::calculateChecksum() const {
    uint8_t checksum = 0;
    // XOR length + command + data
    for (uint8_t i = 1; i < totalLength_; i++) {
        checksum ^= buffer_[i];
    }
    return checksum;
}

// ============================================================================
// Protocol Implementation
// ============================================================================

Protocol::Protocol(UartSendCallback uartSend)
    : uartSend_(uartSend),
      nextSeqNum_(SEQ_RP2040_MIN),
      wakeNotificationCallback_(nullptr),
      scheduleCompleteCallback_(nullptr),
      wakeIntervalCallback_(nullptr),
      scheduleEntryCallback_(nullptr),
      pendingCommandCallback_(nullptr),
      ackCallback_(nullptr),
      pendingDateTimeCallback_(nullptr) {
}

void Protocol::processReceivedByte(uint8_t byte) {
    static bool processing = false;

    if (parser_.processByte(byte)) {
        // Prevent re-entrant processing
        if (processing) {
            return;
        }

        processing = true;

        // Complete message received
        uint8_t seqNum = parser_.getSequenceNumber();
        Response resp = parser_.getResponse();
        const uint8_t* data = parser_.getData();
        uint8_t dataLen = parser_.getDataLength();

        switch (resp) {
            case Response::Ack:
                handleAck(seqNum);
                break;
            case Response::Nack:
                handleNack(seqNum, data, dataLen);
                break;
            case Response::WakeInterval:
                handleWakeInterval(data, dataLen);
                break;
            case Response::ScheduleEntry:
                handleScheduleEntry(data, dataLen);
                break;
            case Response::WakeReason:
                handleWakeNotification(data, dataLen);
                break;
            case Response::ScheduleComplete:
                handleScheduleComplete();
                break;
            case Response::DateTimeResponse:
                handleDateTimeResponse(data, dataLen);
                break;
            default:
                // Unknown response, ignore
                break;
        }

        parser_.reset();
        processing = false;
    }
}

// Command senders

uint8_t Protocol::getNextSequenceNumber() {
    uint8_t seq = nextSeqNum_++;
    if (nextSeqNum_ > SEQ_RP2040_MAX) {
        nextSeqNum_ = SEQ_RP2040_MIN;
    }
    return seq;
}

void Protocol::setAckCallback(AckCallback callback) {
    ackCallback_ = callback;
}

void Protocol::sendCommand(uint8_t seqNum, Command command, const uint8_t* data, uint8_t dataLength) {
    builder_.startMessage(seqNum, command);
    for (uint8_t i = 0; i < dataLength; i++) {
        builder_.addByte(data[i]);
    }
    sendMessage();
}

void Protocol::setWakeInterval(uint32_t seconds, CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::SetWakeInterval);
    builder_.addUint32(seconds);
    sendMessage();
}

void Protocol::getWakeInterval(CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::GetWakeInterval);
    sendMessage();
}

void Protocol::setSchedule(const ScheduleEntry& entry, CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::SetSchedule);
    builder_.addScheduleEntry(entry);
    sendMessage();
}

void Protocol::getSchedule(uint8_t index, CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::GetSchedule);
    builder_.addByte(index);
    sendMessage();
}

void Protocol::clearSchedule(uint8_t index, CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::ClearSchedule);
    builder_.addByte(index);
    sendMessage();
}

void Protocol::keepAwake(uint16_t seconds, CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::KeepAwake);
    builder_.addUint16(seconds);
    sendMessage();
}

void Protocol::setDateTime(const DateTime& dateTime, CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::SetDateTime);
    builder_.addByte(dateTime.year);
    builder_.addByte(dateTime.month);
    builder_.addByte(dateTime.day);
    builder_.addByte(dateTime.weekday);
    builder_.addByte(dateTime.hour);
    builder_.addByte(dateTime.minute);
    builder_.addByte(dateTime.second);
    sendMessage();
}

void Protocol::readyForSleep(CommandResultCallback callback) {
    pendingCommandCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::ReadyForSleep);
    sendMessage();
}

void Protocol::getDateTime(DateTimeCallback callback) {
    pendingDateTimeCallback_ = callback;
    builder_.startMessage(getNextSequenceNumber(), Command::GetDateTime);
    sendMessage();
}

// Callback setters for unsolicited messages

void Protocol::onWakeNotification(WakeNotificationCallback callback) {
    wakeNotificationCallback_ = callback;
}

void Protocol::onScheduleComplete(ScheduleCompleteCallback callback) {
    scheduleCompleteCallback_ = callback;
}

void Protocol::onWakeInterval(WakeIntervalCallback callback) {
    wakeIntervalCallback_ = callback;
}

void Protocol::onScheduleEntry(ScheduleEntryCallback callback) {
    scheduleEntryCallback_ = callback;
}

void Protocol::onDateTime(DateTimeCallback callback) {
    pendingDateTimeCallback_ = callback;
}

// Response handlers

void Protocol::handleAck(uint8_t seqNum) {
    // Call the sequence-aware callback first (for ReliablePmuClient)
    if (ackCallback_) {
        ackCallback_(seqNum, true, ErrorCode::NoError);
    }

    // Also call legacy callback for backward compatibility
    if (pendingCommandCallback_) {
        auto callback = pendingCommandCallback_;
        pendingCommandCallback_ = nullptr;  // Clear before calling to allow new commands
        callback(true, ErrorCode::NoError);
    }
}

void Protocol::handleNack(uint8_t seqNum, const uint8_t* data, uint8_t length) {
    ErrorCode error = ErrorCode::NoError;
    if (length >= 1) {
        error = static_cast<ErrorCode>(data[0]);
    }

    // Call the sequence-aware callback first (for ReliablePmuClient)
    if (ackCallback_) {
        ackCallback_(seqNum, false, error);
    }

    // Also call legacy callback for backward compatibility
    if (pendingCommandCallback_) {
        auto callback = pendingCommandCallback_;
        pendingCommandCallback_ = nullptr;  // Clear before calling to allow new commands
        callback(false, error);
    }
}

void Protocol::handleWakeInterval(const uint8_t* data, uint8_t length) {
    if (length >= 4 && wakeIntervalCallback_) {
        // Read uint32_t little-endian
        uint32_t seconds = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        wakeIntervalCallback_(seconds);
    }
}

void Protocol::handleScheduleEntry(const uint8_t* data, uint8_t length) {
    if (length >= SCHEDULE_ENTRY_SIZE && scheduleEntryCallback_) {
        ScheduleEntry entry;
        entry.hour = data[0];
        entry.minute = data[1];
        entry.duration = data[2] | (data[3] << 8);
        entry.daysMask = static_cast<DayOfWeek>(data[4]);
        entry.valveId = data[5];
        entry.enabled = (data[6] != 0);

        scheduleEntryCallback_(entry);
    }
}

void Protocol::handleWakeNotification(const uint8_t* data, uint8_t length) {
    if (!wakeNotificationCallback_) return;
    if (length < 1) return;

    WakeReason reason = static_cast<WakeReason>(data[0]);
    log.debug("WakeReason=%d", static_cast<int>(reason));

    // Parse schedule entry data (sent with scheduled wake events)
    static ScheduleEntry entry;
    if (length >= 1 + SCHEDULE_ENTRY_SIZE) {
        // Has schedule data - parse it
        entry.hour = data[1];
        entry.minute = data[2];
        entry.duration = data[3] | (data[4] << 8);
        entry.daysMask = static_cast<DayOfWeek>(data[5]);
        entry.valveId = data[6];
        entry.enabled = (data[7] != 0);
        log.debug("ValveId=%d", entry.valveId);
        wakeNotificationCallback_(reason, &entry);
        log.debug("Callback done");
    } else {
        // Periodic or external wake (no schedule data)
        wakeNotificationCallback_(reason, nullptr);
    }
}

void Protocol::handleScheduleComplete() {
    if (scheduleCompleteCallback_) {
        scheduleCompleteCallback_();
    }
}

void Protocol::handleDateTimeResponse(const uint8_t* data, uint8_t length) {
    // Expected format: [valid] [year] [month] [day] [weekday] [hour] [min] [sec]
    if (length < 8) {
        log.error("DateTimeResponse too short: %d bytes", length);
        if (pendingDateTimeCallback_) {
            DateTime empty;
            auto callback = pendingDateTimeCallback_;
            pendingDateTimeCallback_ = nullptr;
            callback(false, empty);
        }
        return;
    }

    bool valid = (data[0] != 0);
    DateTime datetime(
        data[1],  // year (offset from 2000)
        data[2],  // month
        data[3],  // day
        data[4],  // weekday
        data[5],  // hour
        data[6],  // minute
        data[7]   // second
    );

    log.debug("DateTimeResponse: valid=%d, 20%02d-%02d-%02d %02d:%02d:%02d",
              valid, datetime.year, datetime.month, datetime.day,
              datetime.hour, datetime.minute, datetime.second);

    if (pendingDateTimeCallback_) {
        auto callback = pendingDateTimeCallback_;
        pendingDateTimeCallback_ = nullptr;
        callback(valid, datetime);
    }
}

void Protocol::sendMessage() {
    const uint8_t* msg = builder_.finalize();
    uint8_t len = builder_.getLength();

    if (uartSend_) {
        uartSend_(msg, len);
    }
}

}  // namespace PMU
