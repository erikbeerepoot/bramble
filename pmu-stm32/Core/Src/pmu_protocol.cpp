#include "pmu_protocol.h"

#include <cstring>

#include "main.h"  // For RTC_HandleTypeDef and HAL functions

// External reference to RTC handle from main.cpp
extern RTC_HandleTypeDef hrtc;

namespace PMU {

// ============================================================================
// ScheduleEntry Implementation
// ============================================================================

ScheduleEntry::ScheduleEntry()
    : hour(0), minute(0), duration(0), daysMask(static_cast<DayOfWeek>(0)), valveId(0),
      enabled(false)
{
}

bool ScheduleEntry::isValid() const
{
    if (!enabled)
        return true;  // Disabled entries are always valid
    if (hour > 23)
        return false;
    if (minute > 59)
        return false;
    if (duration == 0 || duration > 65535)
        return false;
    if (static_cast<uint8_t>(daysMask) == 0)
        return false;
    return true;
}

bool ScheduleEntry::overlapsWith(const ScheduleEntry &other) const
{
    if (!enabled || !other.enabled)
        return false;

    // Check if they share any common days
    DayOfWeek commonDays = daysMask & other.daysMask;
    if (!commonDays)
        return false;  // No common days, no overlap

    // Check if time ranges overlap
    return timeRangesOverlap(hour, minute, duration, other.hour, other.minute, other.duration);
}

uint32_t ScheduleEntry::minutesUntil(uint8_t currentDay, uint8_t currentHour,
                                     uint8_t currentMinute) const
{
    if (!enabled)
        return 0xFFFFFFFF;
    if (!matchesDay(currentDay))
        return 0xFFFFFFFF;

    uint32_t currentMinutes = currentHour * 60 + currentMinute;
    uint32_t scheduleMinutes = hour * 60 + minute;

    if (scheduleMinutes >= currentMinutes) {
        return scheduleMinutes - currentMinutes;
    }

    return 0xFFFFFFFF;  // Time has passed today
}

bool ScheduleEntry::isWithinWindow(uint8_t currentDay, uint8_t currentHour, uint8_t currentMinute,
                                   uint32_t windowMinutes) const
{
    if (!enabled)
        return false;
    if (!matchesDay(currentDay))
        return false;

    uint32_t currentMinutes = currentHour * 60 + currentMinute;
    uint32_t scheduleMinutes = hour * 60 + minute;

    // Check if we're at or past the scheduled time, but within the window
    if (currentMinutes >= scheduleMinutes) {
        uint32_t minutesPast = currentMinutes - scheduleMinutes;
        return minutesPast <= windowMinutes;
    }

    // Also check if we're slightly before (within the window)
    // This handles the case where we wake slightly early
    if (scheduleMinutes > currentMinutes) {
        uint32_t minutesUntil = scheduleMinutes - currentMinutes;
        return minutesUntil <= windowMinutes;
    }

    return false;
}

bool ScheduleEntry::matchesDay(uint8_t dayOfWeek) const
{
    if (dayOfWeek > 6)
        return false;  // Invalid day
    uint8_t dayBit = 1 << dayOfWeek;
    return (static_cast<uint8_t>(daysMask) & dayBit) != 0;
}

bool ScheduleEntry::timeRangesOverlap(uint8_t h1, uint8_t m1, uint16_t d1, uint8_t h2, uint8_t m2,
                                      uint16_t d2) const
{
    // Convert to minutes since midnight
    uint32_t start1 = h1 * 60 + m1;
    uint32_t end1 = start1 + (d1 / 60);  // duration in seconds -> minutes

    uint32_t start2 = h2 * 60 + m2;
    uint32_t end2 = start2 + (d2 / 60);

    // Check if ranges overlap
    // They overlap if: start1 < end2 AND start2 < end1
    return (start1 < end2) && (start2 < end1);
}

// ============================================================================
// WateringSchedule Implementation
// ============================================================================

WateringSchedule::WateringSchedule() : count_(0)
{
    // Initialize all entries as disabled
    for (auto &entry : entries_) {
        entry.enabled = false;
    }
}

ErrorCode WateringSchedule::addEntry(const ScheduleEntry &entry)
{
    if (!entry.isValid()) {
        return ErrorCode::InvalidParam;
    }

    if (count_ >= MAX_SCHEDULE_ENTRIES) {
        return ErrorCode::ScheduleFull;
    }

    if (hasOverlap(entry)) {
        return ErrorCode::Overlap;
    }

    // Append to end of list
    entries_[count_] = entry;
    count_++;

    return ErrorCode::NoError;
}

ErrorCode WateringSchedule::updateEntry(uint8_t index, const ScheduleEntry &entry)
{
    if (index >= count_) {
        return ErrorCode::InvalidIndex;
    }

    if (!entry.isValid()) {
        return ErrorCode::InvalidParam;
    }

    if (hasOverlap(entry, index)) {
        return ErrorCode::Overlap;
    }

    entries_[index] = entry;
    return ErrorCode::NoError;
}

ErrorCode WateringSchedule::removeEntry(uint8_t index)
{
    if (index >= count_) {
        return ErrorCode::InvalidIndex;
    }

    // Shift all entries after this one down
    for (uint8_t i = index; i < count_ - 1; i++) {
        entries_[i] = entries_[i + 1];
    }

    count_--;
    return ErrorCode::NoError;
}

void WateringSchedule::clear()
{
    count_ = 0;
}

const ScheduleEntry *WateringSchedule::getEntry(uint8_t index) const
{
    if (index >= count_) {
        return nullptr;
    }
    return &entries_[index];
}

const ScheduleEntry *WateringSchedule::findNextEntry(uint8_t currentDay, uint8_t currentHour,
                                                     uint8_t currentMinute) const
{
    const ScheduleEntry *nextEntry = nullptr;
    uint32_t minMinutes = 0xFFFFFFFF;

    for (uint8_t i = 0; i < count_; i++) {
        const auto &entry = entries_[i];
        if (!entry.enabled)
            continue;

        uint32_t minutesUntil = entry.minutesUntil(currentDay, currentHour, currentMinute);
        if (minutesUntil < minMinutes) {
            minMinutes = minutesUntil;
            nextEntry = &entry;
        }
    }

    return nextEntry;
}

uint8_t WateringSchedule::getCount() const
{
    return count_;
}

bool WateringSchedule::hasOverlap(const ScheduleEntry &entry, uint8_t excludeIndex) const
{
    for (uint8_t i = 0; i < count_; i++) {
        if (i == excludeIndex)
            continue;

        if (entry.overlapsWith(entries_[i])) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// MessageParser Implementation
// ============================================================================

MessageParser::MessageParser()
    : state_(State::WaitStart), bytesRead_(0), expectedLength_(0), calculatedChecksum_(0),
      sequenceNumber_(0), complete_(false)
{
}

bool MessageParser::processByte(uint8_t byte)
{
    switch (state_) {
        case State::WaitStart:
            if (byte == START_BYTE) {
                bytesRead_ = 0;
                complete_ = false;
                sequenceNumber_ = 0;
                state_ = State::ReadLength;
            }
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
            state_ = State::ReadCommand;
            break;

        case State::ReadCommand:
            buffer_[bytesRead_++] = byte;
            calculatedChecksum_ ^= byte;
            // Length field includes: seq + command + data
            // We've read: length(1) + seq(1) + command(1) = 3 bytes
            // If expectedLength_ == 2 (seq + command only), no data
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
            // bytesRead_ includes: length(1) + seq(1) + command(1) + data(n)
            // expectedLength_ = seq(1) + command(1) + data(n) = 2 + n
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

bool MessageParser::isComplete() const
{
    return complete_;
}

uint8_t MessageParser::getSequenceNumber() const
{
    return sequenceNumber_;
}

Command MessageParser::getCommand() const
{
    // buffer_[0] = length, buffer_[1] = seq, buffer_[2] = command
    if (bytesRead_ < 3)
        return static_cast<Command>(0);
    return static_cast<Command>(buffer_[2]);
}

const uint8_t *MessageParser::getData() const
{
    // Data starts after length(1) + seq(1) + command(1)
    if (bytesRead_ < 3)
        return nullptr;
    return &buffer_[3];
}

uint8_t MessageParser::getDataLength() const
{
    if (bytesRead_ < 3)
        return 0;
    // expectedLength_ = seq(1) + command(1) + data(n)
    // dataLength = expectedLength_ - 2
    return (expectedLength_ > 2) ? (expectedLength_ - 2) : 0;
}

void MessageParser::reset()
{
    state_ = State::WaitStart;
    bytesRead_ = 0;
    expectedLength_ = 0;
    calculatedChecksum_ = 0;
    sequenceNumber_ = 0;
    complete_ = false;
}

uint8_t MessageParser::calculateChecksum() const
{
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < bytesRead_; i++) {
        checksum ^= buffer_[i];
    }
    return checksum;
}

// ============================================================================
// MessageBuilder Implementation
// ============================================================================

MessageBuilder::MessageBuilder() : dataLength_(0), totalLength_(0) {}

void MessageBuilder::startMessage(uint8_t sequenceNumber, uint8_t response)
{
    buffer_[0] = START_BYTE;
    // buffer_[1] = length (set in finalize)
    buffer_[2] = sequenceNumber;
    buffer_[3] = response;
    dataLength_ = 2;   // seq + response
    totalLength_ = 4;  // START + LENGTH + SEQ + RESPONSE
}

void MessageBuilder::addByte(uint8_t data)
{
    buffer_[totalLength_++] = data;
    dataLength_++;
}

void MessageBuilder::addUint16(uint16_t data)
{
    // Little-endian
    addByte(data & 0xFF);
    addByte((data >> 8) & 0xFF);
}

void MessageBuilder::addUint32(uint32_t data)
{
    // Little-endian
    addByte(data & 0xFF);
    addByte((data >> 8) & 0xFF);
    addByte((data >> 16) & 0xFF);
    addByte((data >> 24) & 0xFF);
}

void MessageBuilder::addScheduleEntry(const ScheduleEntry &entry)
{
    addByte(entry.hour);
    addByte(entry.minute);
    addUint16(entry.duration);
    addByte(static_cast<uint8_t>(entry.daysMask));
    addByte(entry.valveId);
    addByte(entry.enabled ? 1 : 0);
}

const uint8_t *MessageBuilder::finalize()
{
    // Set length field
    buffer_[1] = dataLength_;

    // Calculate and add checksum
    buffer_[totalLength_++] = calculateChecksum();

    // Add end byte
    buffer_[totalLength_++] = END_BYTE;

    return buffer_;
}

uint8_t MessageBuilder::getLength() const
{
    return totalLength_;
}

uint8_t MessageBuilder::calculateChecksum() const
{
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

Protocol::Protocol(UartSendCallback uartSend, SetWakeCallback setWake, KeepAwakeCallback keepAwake,
                   ReadyForSleepCallback readyForSleep, GetTickCallback getTick)
    : wakeInterval_(60), nextSeqNum_(SEQ_STM32_MIN), currentSeqNum_(0), uartSend_(uartSend),
      setWake_(setWake), keepAwake_(keepAwake), readyForSleep_(readyForSleep), getTick_(getTick),
      seenIndex_(0), nodeStateValid_(false), clearToSendReceived_(false)
{
    // Initialize deduplication buffer
    for (auto &entry : seenBuffer_) {
        entry.seqNum = 0;
        entry.timestamp = 0;
    }
    // Initialize node state to zeros
    for (uint8_t i = 0; i < NODE_STATE_SIZE; i++) {
        nodeState_[i] = 0;
    }
}

bool Protocol::wasRecentlySeen(uint8_t seqNum)
{
    if (!getTick_)
        return false;  // No tick function, skip dedup

    uint32_t now = getTick_();
    for (uint8_t i = 0; i < DEDUP_BUFFER_SIZE; i++) {
        if (seenBuffer_[i].seqNum == seqNum && (now - seenBuffer_[i].timestamp) < DEDUP_WINDOW_MS) {
            return true;
        }
    }
    return false;
}

void Protocol::markAsSeen(uint8_t seqNum)
{
    if (!getTick_)
        return;  // No tick function, skip dedup

    seenBuffer_[seenIndex_].seqNum = seqNum;
    seenBuffer_[seenIndex_].timestamp = getTick_();
    seenIndex_ = (seenIndex_ + 1) % DEDUP_BUFFER_SIZE;
}

void Protocol::processReceivedByte(uint8_t byte)
{
    if (parser_.processByte(byte)) {
        // Complete message received
        uint8_t seqNum = parser_.getSequenceNumber();
        Command cmd = parser_.getCommand();
        const uint8_t *data = parser_.getData();
        uint8_t dataLen = parser_.getDataLength();

        // Store current sequence number for ACK/NACK responses
        currentSeqNum_ = seqNum;

        // Check for duplicate (but always send ACK)
        bool isDuplicate = wasRecentlySeen(seqNum);

        // Always send response (ACK/NACK), but only execute if not duplicate
        switch (cmd) {
            case Command::SetWakeInterval:
                if (!isDuplicate) {
                    handleSetWakeInterval(data, dataLen);
                } else {
                    sendAck();  // Resend ACK for duplicate
                }
                break;
            case Command::GetWakeInterval:
                if (!isDuplicate) {
                    handleGetWakeInterval();
                } else {
                    sendAck();
                }
                break;
            case Command::SetSchedule:
                if (!isDuplicate) {
                    handleSetSchedule(data, dataLen);
                } else {
                    sendAck();
                }
                break;
            case Command::GetSchedule:
                if (!isDuplicate) {
                    handleGetSchedule(data, dataLen);
                } else {
                    sendAck();
                }
                break;
            case Command::ClearSchedule:
                if (!isDuplicate) {
                    handleClearSchedule(data, dataLen);
                } else {
                    sendAck();
                }
                break;
            case Command::KeepAwake:
                if (!isDuplicate) {
                    handleKeepAwake(data, dataLen);
                } else {
                    sendAck();
                }
                break;
            case Command::SetDateTime:
                if (!isDuplicate) {
                    handleSetDateTime(data, dataLen);
                } else {
                    sendAck();
                }
                break;
            case Command::ReadyForSleep:
                if (!isDuplicate) {
                    handleReadyForSleep();
                } else {
                    sendAck();
                }
                break;
            case Command::GetDateTime:
                if (!isDuplicate) {
                    handleGetDateTime();
                } else {
                    sendAck();
                }
                break;
            case Command::ClearToSend:
                // RP2040 is ready to receive wake info
                clearToSendReceived_ = true;
                sendAck();
                break;
            case Command::SystemReset:
                // Full system reset requested by RP2040
                // Always send ACK (even for duplicates, in case first ACK was lost)
                sendAck();
                if (!isDuplicate) {
                    // Brief delay to allow UART to flush the ACK
                    for (volatile uint32_t i = 0; i < 10000; i++) {}
                    NVIC_SystemReset();
                }
                break;
            default:
                sendNack(ErrorCode::InvalidParam);
                break;
        }

        // Mark as seen after processing (only for valid commands)
        if (cmd != static_cast<Command>(0)) {
            markAsSeen(seqNum);
        }

        parser_.reset();
    }
}

uint8_t Protocol::getNextSeqNum()
{
    uint8_t seq = nextSeqNum_++;
    if (nextSeqNum_ > SEQ_STM32_MAX) {
        nextSeqNum_ = SEQ_STM32_MIN;
    }
    return seq;
}

void Protocol::sendWakeNotification(WakeReason reason)
{
    builder_.startMessage(getNextSeqNum(), static_cast<uint8_t>(Response::WakeReason));
    builder_.addByte(static_cast<uint8_t>(reason));
    // Add state valid flag and state blob (new protocol)
    builder_.addByte(nodeStateValid_ ? 0x01 : 0x00);
    for (uint8_t i = 0; i < NODE_STATE_SIZE; i++) {
        builder_.addByte(nodeState_[i]);
    }
    sendMessage();
}

void Protocol::sendWakeNotificationWithSchedule(WakeReason reason, const ScheduleEntry *entry)
{
    builder_.startMessage(getNextSeqNum(), static_cast<uint8_t>(Response::WakeReason));
    builder_.addByte(static_cast<uint8_t>(reason));
    // Add state valid flag and state blob (new protocol)
    builder_.addByte(nodeStateValid_ ? 0x01 : 0x00);
    for (uint8_t i = 0; i < NODE_STATE_SIZE; i++) {
        builder_.addByte(nodeState_[i]);
    }
    // Add schedule entry after state blob
    if (entry) {
        builder_.addScheduleEntry(*entry);
    }
    sendMessage();
}

void Protocol::sendScheduleComplete()
{
    builder_.startMessage(getNextSeqNum(), static_cast<uint8_t>(Response::ScheduleComplete));
    sendMessage();
}

const ScheduleEntry *Protocol::getNextScheduledEntry(uint8_t currentDay, uint8_t currentHour,
                                                     uint8_t currentMinute) const
{
    return schedule_.findNextEntry(currentDay, currentHour, currentMinute);
}

// Command handlers

void Protocol::handleSetWakeInterval(const uint8_t *data, uint8_t length)
{
    if (length != 4) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    // Read uint32_t little-endian
    uint32_t seconds = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    if (seconds == 0 || seconds > 86400) {  // Max 24 hours
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    wakeInterval_ = seconds;

    // Send ACK before reconfiguring RTC to avoid timing issues
    sendAck();

    // Now reconfigure the RTC wakeup timer
    if (setWake_) {
        setWake_(seconds);
    }
}

void Protocol::handleGetWakeInterval()
{
    sendWakeInterval();
}

void Protocol::handleSetSchedule(const uint8_t *data, uint8_t length)
{
    if (length != SCHEDULE_ENTRY_SIZE) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    ScheduleEntry entry;
    entry.hour = data[0];
    entry.minute = data[1];
    entry.duration = data[2] | (data[3] << 8);
    entry.daysMask = static_cast<DayOfWeek>(data[4]);
    entry.valveId = data[5];
    entry.enabled = (data[6] != 0);

    ErrorCode result = schedule_.addEntry(entry);

    if (result == ErrorCode::NoError) {
        sendAck();
    } else {
        sendNack(result);
    }
}

void Protocol::handleGetSchedule(const uint8_t *data, uint8_t length)
{
    if (length != 1) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    uint8_t index = data[0];
    sendScheduleEntry(index);
}

void Protocol::handleClearSchedule(const uint8_t *data, uint8_t length)
{
    if (length != 1) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    uint8_t index = data[0];

    if (index == 0xFF) {
        // Clear all
        schedule_.clear();
        sendAck();
    } else {
        ErrorCode result = schedule_.removeEntry(index);
        if (result == ErrorCode::NoError) {
            sendAck();
        } else {
            sendNack(result);
        }
    }
}

void Protocol::handleKeepAwake(const uint8_t *data, uint8_t length)
{
    if (length != 2) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    uint16_t seconds = data[0] | (data[1] << 8);

    if (keepAwake_) {
        keepAwake_(seconds);
    }

    sendAck();
}

void Protocol::handleSetDateTime(const uint8_t *data, uint8_t length)
{
    // Expected: 7 bytes (year, month, day, weekday, hour, minute, second)
    if (length != 7) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    RTC_TimeTypeDef time = {0};
    RTC_DateTypeDef date = {0};

    // Parse date/time from message
    // Year is offset from 2000
    date.Year = data[0];
    date.Month = data[1];
    date.Date = data[2];
    date.WeekDay = data[3];  // 0=Sunday, 1=Monday, etc.

    time.Hours = data[4];
    time.Minutes = data[5];
    time.Seconds = data[6];
    time.TimeFormat = RTC_HOURFORMAT_24;
    time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    time.StoreOperation = RTC_STOREOPERATION_RESET;

    // Validate ranges
    if (date.Month < 1 || date.Month > 12 || date.Date < 1 || date.Date > 31 || date.WeekDay > 6 ||
        time.Hours > 23 || time.Minutes > 59 || time.Seconds > 59) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    // Set RTC time and date (use global namespace for hrtc)
    if (HAL_RTC_SetTime(&::hrtc, &time, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_SetDate(&::hrtc, &date, RTC_FORMAT_BIN) != HAL_OK) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    // Write magic value to backup register to indicate time has been synced
    // This persists as long as the PMU is powered from main battery
    HAL_RTCEx_BKUPWrite(&::hrtc, RTC_BKP_DR0, TIME_VALID_MAGIC);

    sendAck();
}

void Protocol::handleReadyForSleep()
{
    // Extract state blob from payload if present (new protocol)
    const uint8_t *data = parser_.getData();
    uint8_t dataLen = parser_.getDataLength();

    if (dataLen >= NODE_STATE_SIZE) {
        // New protocol: state blob is included in payload
        for (uint8_t i = 0; i < NODE_STATE_SIZE; i++) {
            nodeState_[i] = data[i];
        }
        nodeStateValid_ = true;
    }
    // If no state blob, keep previous state (backward compatibility)

    // RP2040 signals it's done with work and ready for power down
    // Send ACK first, then call the callback
    sendAck();

    if (readyForSleep_) {
        readyForSleep_();
    }
}

void Protocol::handleGetDateTime()
{
    // Send ACK first (confirms command received, enables retry on RP2040)
    sendAck();

    // Read current time from RTC
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;

    if (HAL_RTC_GetTime(&::hrtc, &time, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_GetDate(&::hrtc, &date, RTC_FORMAT_BIN) != HAL_OK) {
        // RTC read failed - send invalid response
        sendDateTimeResponse(false, 0, 1, 1, 0, 0, 0, 0);
        return;
    }

    // Check if RTC has been synced by reading backup register
    uint32_t magic = HAL_RTCEx_BKUPRead(&::hrtc, RTC_BKP_DR0);
    bool valid = (magic == TIME_VALID_MAGIC);

    // Send response with valid flag and datetime
    sendDateTimeResponse(valid, date.Year, date.Month, date.Date, date.WeekDay, time.Hours,
                         time.Minutes, time.Seconds);
}

// Response senders (echo the command's sequence number)

void Protocol::sendAck()
{
    // Echo the sequence number from the received command
    builder_.startMessage(currentSeqNum_, static_cast<uint8_t>(Response::Ack));
    sendMessage();
}

void Protocol::sendNack(ErrorCode error)
{
    // Echo the sequence number from the received command
    builder_.startMessage(currentSeqNum_, static_cast<uint8_t>(Response::Nack));
    builder_.addByte(static_cast<uint8_t>(error));
    sendMessage();
}

void Protocol::sendWakeInterval()
{
    // Echo the sequence number from the received command
    builder_.startMessage(currentSeqNum_, static_cast<uint8_t>(Response::WakeInterval));
    builder_.addUint32(wakeInterval_);
    sendMessage();
}

void Protocol::sendScheduleEntry(uint8_t index)
{
    const ScheduleEntry *entry = schedule_.getEntry(index);

    if (entry == nullptr) {
        sendNack(ErrorCode::InvalidIndex);
        return;
    }

    // Echo the sequence number from the received command
    builder_.startMessage(currentSeqNum_, static_cast<uint8_t>(Response::ScheduleEntry));
    builder_.addScheduleEntry(*entry);
    sendMessage();
}

void Protocol::sendDateTimeResponse(bool valid, uint8_t year, uint8_t month, uint8_t day,
                                    uint8_t weekday, uint8_t hour, uint8_t minute, uint8_t second)
{
    // Echo the sequence number from the received command
    builder_.startMessage(currentSeqNum_, static_cast<uint8_t>(Response::DateTimeResponse));
    builder_.addByte(valid ? 0x01 : 0x00);
    builder_.addByte(year);
    builder_.addByte(month);
    builder_.addByte(day);
    builder_.addByte(weekday);
    builder_.addByte(hour);
    builder_.addByte(minute);
    builder_.addByte(second);
    sendMessage();
}

void Protocol::sendMessage()
{
    const uint8_t *msg = builder_.finalize();
    uint8_t len = builder_.getLength();

    if (uartSend_) {
        uartSend_(msg, len);
    }
}

}  // namespace PMU
