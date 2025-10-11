#include "pmu_protocol.h"
#include <cstring>

namespace PMU {

// ============================================================================
// MessageParser Implementation
// ============================================================================

MessageParser::MessageParser()
    : state_(State::WaitStart), bytesRead_(0), expectedLength_(0),
      calculatedChecksum_(0), complete_(false) {
}

bool MessageParser::processByte(uint8_t byte) {
    switch (state_) {
        case State::WaitStart:
            if (byte == START_BYTE) {
                bytesRead_ = 0;
                complete_ = false;
                state_ = State::ReadLength;
            }
            break;

        case State::ReadLength:
            expectedLength_ = byte;
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ = byte;
            state_ = State::ReadResponse;
            break;

        case State::ReadResponse:
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ ^= byte;
            if (expectedLength_ == 1) {
                // No data bytes, go straight to checksum
                state_ = State::ReadChecksum;
            } else {
                state_ = State::ReadData;
            }
            break;

        case State::ReadData:
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ ^= byte;
            // Check if we've read all data bytes (length includes response byte)
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

Response MessageParser::getResponse() const {
    if (bytesRead_ < 2) return static_cast<Response>(0);
    return static_cast<Response>(buffer_[1]);
}

const uint8_t* MessageParser::getData() const {
    if (bytesRead_ < 2) return nullptr;
    return &buffer_[2];
}

uint8_t MessageParser::getDataLength() const {
    if (bytesRead_ < 2) return 0;
    return expectedLength_ - 1;  // Subtract response byte
}

void MessageParser::reset() {
    state_ = State::WaitStart;
    bytesRead_ = 0;
    expectedLength_ = 0;
    calculatedChecksum_ = 0;
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

MessageBuilder::MessageBuilder() : dataLength_(0), totalLength_(0) {
}

void MessageBuilder::startMessage(Command command) {
    buffer_[0] = START_BYTE;
    buffer_[2] = static_cast<uint8_t>(command);
    dataLength_ = 1;  // Just command for now
    totalLength_ = 3; // START + LENGTH + COMMAND
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
      wakeNotificationCallback_(nullptr),
      scheduleCompleteCallback_(nullptr),
      ackCallback_(nullptr),
      nackCallback_(nullptr),
      wakeIntervalCallback_(nullptr),
      scheduleEntryCallback_(nullptr) {
}

void Protocol::processReceivedByte(uint8_t byte) {
    if (parser_.processByte(byte)) {
        // Complete message received
        Response resp = parser_.getResponse();
        const uint8_t* data = parser_.getData();
        uint8_t dataLen = parser_.getDataLength();

        switch (resp) {
            case Response::Ack:
                handleAck();
                break;
            case Response::Nack:
                handleNack(data, dataLen);
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
            default:
                // Unknown response, ignore
                break;
        }

        parser_.reset();
    }
}

// Command senders

bool Protocol::setWakeInterval(uint32_t seconds) {
    builder_.startMessage(Command::SetWakeInterval);
    builder_.addUint32(seconds);
    sendMessage();
    return true;
}

bool Protocol::getWakeInterval() {
    builder_.startMessage(Command::GetWakeInterval);
    sendMessage();
    return true;
}

bool Protocol::setSchedule(const ScheduleEntry& entry) {
    builder_.startMessage(Command::SetSchedule);
    builder_.addScheduleEntry(entry);
    sendMessage();
    return true;
}

bool Protocol::getSchedule(uint8_t index) {
    builder_.startMessage(Command::GetSchedule);
    builder_.addByte(index);
    sendMessage();
    return true;
}

bool Protocol::clearSchedule(uint8_t index) {
    builder_.startMessage(Command::ClearSchedule);
    builder_.addByte(index);
    sendMessage();
    return true;
}

bool Protocol::keepAwake(uint16_t seconds) {
    builder_.startMessage(Command::KeepAwake);
    builder_.addUint16(seconds);
    sendMessage();
    return true;
}

// Callback setters

void Protocol::onWakeNotification(WakeNotificationCallback callback) {
    wakeNotificationCallback_ = callback;
}

void Protocol::onScheduleComplete(ScheduleCompleteCallback callback) {
    scheduleCompleteCallback_ = callback;
}

void Protocol::onAck(AckCallback callback) {
    ackCallback_ = callback;
}

void Protocol::onNack(NackCallback callback) {
    nackCallback_ = callback;
}

void Protocol::onWakeInterval(WakeIntervalCallback callback) {
    wakeIntervalCallback_ = callback;
}

void Protocol::onScheduleEntry(ScheduleEntryCallback callback) {
    scheduleEntryCallback_ = callback;
}

// Response handlers

void Protocol::handleAck() {
    if (ackCallback_) {
        ackCallback_();
    }
}

void Protocol::handleNack(const uint8_t* data, uint8_t length) {
    if (length >= 1 && nackCallback_) {
        ErrorCode error = static_cast<ErrorCode>(data[0]);
        nackCallback_(error);
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
    if (wakeNotificationCallback_) {
        if (length < 1) return;  // Need at least wake reason

        WakeReason reason = static_cast<WakeReason>(data[0]);

        // Parse schedule entry data (sent with scheduled wake events)
        ScheduleEntry entry;
        if (length >= 1 + SCHEDULE_ENTRY_SIZE) {
            entry.hour = data[1];
            entry.minute = data[2];
            entry.duration = data[3] | (data[4] << 8);
            entry.daysMask = static_cast<DayOfWeek>(data[5]);
            entry.valveId = data[6];
            entry.enabled = (data[7] != 0);
            wakeNotificationCallback_(reason, &entry);
        } else {
            // Periodic or external wake (no schedule data)
            wakeNotificationCallback_(reason, nullptr);
        }
    }
}

void Protocol::handleScheduleComplete() {
    if (scheduleCompleteCallback_) {
        scheduleCompleteCallback_();
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
