#include "pmu_protocol.h"

#include <cstring>

#include "main.h"  // For RTC_HandleTypeDef and HAL functions
#include "persistent_storage.h"

// External reference to RTC handle from main.cpp
extern RTC_HandleTypeDef hrtc;

namespace PMU {

// ============================================================================
// ScheduleEntry Implementation
// ============================================================================

ScheduleEntry::ScheduleEntry()
    : hour(0), minute(0), duration(0), daysMask(static_cast<DayOfWeek>(0)), valveId(0),
      enabled(false), periodMinutes(0), windowMinutes(0)
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
    if (duration == 0)
        return false;
    if (static_cast<uint8_t>(daysMask) == 0)
        return false;
    if (periodMinutes > 0) {
        // Interval schedule: need a non-empty active window, and each run must
        // finish before the next firing so occurrences can't overlap.
        if (windowMinutes == 0)
            return false;
        if (duration > static_cast<uint32_t>(periodMinutes) * 60)
            return false;
    }
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

    // An interval entry occupies its whole active window; a one-shot entry only
    // occupies a single run. Compare the [start, start+span) ranges in minutes.
    uint32_t start1 = hour * 60 + minute;
    uint32_t end1 = start1 + occupiedMinutes();
    uint32_t start2 = other.hour * 60 + other.minute;
    uint32_t end2 = start2 + other.occupiedMinutes();

    // They overlap if: start1 < end2 AND start2 < end1
    return (start1 < end2) && (start2 < end1);
}

uint32_t ScheduleEntry::occupiedMinutes() const
{
    if (periodMinutes > 0) {
        return windowMinutes;  // interval: the whole active window
    }
    return (duration + 59) / 60;  // one-shot: run length in minutes (ceiling)
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

    if (periodMinutes == 0) {
        // Legacy one-shot: fires once at the scheduled time.
        if (scheduleMinutes >= currentMinutes) {
            return scheduleMinutes - currentMinutes;
        }
        return 0xFFFFFFFF;  // Time has passed today
    }

    // Interval schedule: firings at start + k*periodMinutes within the window.
    if (currentMinutes < scheduleMinutes) {
        return scheduleMinutes - currentMinutes;  // window hasn't started yet
    }
    uint32_t windowEnd = scheduleMinutes + windowMinutes;
    if (currentMinutes > windowEnd) {
        return 0xFFFFFFFF;  // window is over for today
    }
    uint32_t offset = currentMinutes - scheduleMinutes;
    uint32_t sinceBoundary = offset % periodMinutes;
    if (sinceBoundary == 0) {
        return 0;  // a firing is due right now
    }
    uint32_t untilNext = periodMinutes - sinceBoundary;
    if (scheduleMinutes + offset + untilNext <= windowEnd) {
        return untilNext;
    }
    return 0xFFFFFFFF;  // no more firings within the window today
}

bool ScheduleEntry::isWithinWindow(uint8_t currentDay, uint8_t currentHour, uint8_t currentMinute,
                                   uint32_t toleranceMinutes) const
{
    if (!enabled)
        return false;
    if (!matchesDay(currentDay))
        return false;

    uint32_t currentMinutes = currentHour * 60 + currentMinute;
    uint32_t scheduleMinutes = hour * 60 + minute;

    if (periodMinutes == 0) {
        // Legacy one-shot: active within tolerance of the single scheduled time,
        // whether we woke slightly early or slightly late.
        uint32_t diff = (currentMinutes >= scheduleMinutes) ? (currentMinutes - scheduleMinutes)
                                                            : (scheduleMinutes - currentMinutes);
        return diff <= toleranceMinutes;
    }

    // Interval schedule: active if we are within tolerance of any firing boundary
    // that itself lies inside the active window.
    if (currentMinutes + toleranceMinutes < scheduleMinutes) {
        return false;  // window hasn't started (beyond an early-wake tolerance)
    }
    uint32_t windowEnd = scheduleMinutes + windowMinutes;
    if (currentMinutes > windowEnd + toleranceMinutes) {
        return false;  // window is over (beyond a late-wake tolerance)
    }
    // Distance to the nearest firing boundary (boundaries at start + k*period).
    uint32_t offset =
        (currentMinutes >= scheduleMinutes) ? (currentMinutes - scheduleMinutes) : 0;
    uint32_t sinceBoundary = offset % periodMinutes;
    uint32_t distance = sinceBoundary < (periodMinutes - sinceBoundary)
                            ? sinceBoundary
                            : (periodMinutes - sinceBoundary);
    return distance <= toleranceMinutes;
}

bool ScheduleEntry::matchesDay(uint8_t dayOfWeek) const
{
    if (dayOfWeek > 6)
        return false;  // Invalid day
    uint8_t dayBit = 1 << dayOfWeek;
    return (static_cast<uint8_t>(daysMask) & dayBit) != 0;
}

// ============================================================================
// WateringSchedule Implementation (FRAM-backed)
// ============================================================================

WateringSchedule::WateringSchedule() : storage_(nullptr) {}

ErrorCode WateringSchedule::setEntry(uint8_t index, const ScheduleEntry &entry)
{
    if (!storage_)
        return ErrorCode::InvalidParam;

    if (index >= MAX_SCHEDULE_ENTRIES) {
        return ErrorCode::InvalidIndex;
    }

    if (!entry.isValid()) {
        return ErrorCode::InvalidParam;
    }

    // Overlap check skips the destination slot itself — replacing a slot
    // with a non-overlapping entry should succeed.
    if (hasOverlap(entry, index)) {
        return ErrorCode::Overlap;
    }

    // Count is a watermark (highest-used-slot + 1) so the existing
    // `for i = 0; i < count` iteration covers all live slots. Disabled
    // slots inside the watermark are skipped via the entry's enabled flag.
    uint8_t count = getCount();

    // When expanding the watermark, explicitly disable any gap slots so
    // pre-existing FRAM data (left over from the old shift-on-remove
    // model, or from an upgrade from earlier firmware) cannot show up as
    // a phantom schedule.
    if (index > count) {
        ScheduleEntry empty;
        empty.enabled = false;
        for (uint8_t i = count; i < index; i++) {
            if (!storage_->saveScheduleEntry(i, empty)) {
                return ErrorCode::InvalidParam;
            }
        }
    }

    if (!storage_->saveScheduleEntry(index, entry)) {
        return ErrorCode::InvalidParam;
    }

    if (index + 1 > count) {
        if (!storage_->setScheduleCount(index + 1)) {
            return ErrorCode::InvalidParam;
        }
    }

    return ErrorCode::NoError;
}

ErrorCode WateringSchedule::removeEntry(uint8_t index)
{
    if (!storage_)
        return ErrorCode::InvalidParam;

    if (index >= MAX_SCHEDULE_ENTRIES) {
        return ErrorCode::InvalidIndex;
    }

    uint8_t count = getCount();
    if (index >= count) {
        return ErrorCode::InvalidIndex;
    }

    // Clear the slot's enabled flag in place — do NOT shift subsequent
    // slots, so slot indices stay stable across removes.
    ScheduleEntry entry;
    if (!storage_->loadScheduleEntry(index, entry)) {
        return ErrorCode::InvalidParam;
    }
    entry.enabled = false;
    if (!storage_->saveScheduleEntry(index, entry)) {
        return ErrorCode::InvalidParam;
    }

    // Trim trailing disabled slots from the watermark so iteration stays
    // efficient when entries are removed from the end.
    uint8_t newCount = count;
    while (newCount > 0) {
        ScheduleEntry trailing;
        if (!storage_->loadScheduleEntry(newCount - 1, trailing))
            break;
        if (trailing.enabled)
            break;
        newCount--;
    }
    if (newCount != count) {
        if (!storage_->setScheduleCount(newCount)) {
            return ErrorCode::InvalidParam;
        }
    }
    return ErrorCode::NoError;
}

void WateringSchedule::clear()
{
    if (storage_) {
        storage_->setScheduleCount(0);
    }
}

bool WateringSchedule::getEntry(uint8_t index, ScheduleEntry &out) const
{
    if (!storage_)
        return false;

    uint8_t count = getCount();
    if (index >= count)
        return false;

    return storage_->loadScheduleEntry(index, out);
}

bool WateringSchedule::findNextEntry(uint8_t currentDay, uint8_t currentHour, uint8_t currentMinute,
                                     ScheduleEntry &out) const
{
    if (!storage_)
        return false;

    uint8_t count = getCount();
    bool found = false;
    uint32_t minMinutes = 0xFFFFFFFF;

    for (uint8_t i = 0; i < count; i++) {
        ScheduleEntry entry;
        if (!storage_->loadScheduleEntry(i, entry))
            continue;
        if (!entry.enabled)
            continue;

        uint32_t minutes = entry.minutesUntil(currentDay, currentHour, currentMinute);
        if (minutes < minMinutes) {
            minMinutes = minutes;
            out = entry;
            found = true;
        }
    }

    return found;
}

uint8_t WateringSchedule::getCount() const
{
    if (!storage_)
        return 0;
    return storage_->getScheduleCount();
}

bool WateringSchedule::hasOverlap(const ScheduleEntry &entry, uint8_t excludeIndex) const
{
    if (!storage_)
        return false;

    uint8_t count = getCount();
    for (uint8_t i = 0; i < count; i++) {
        if (i == excludeIndex)
            continue;

        ScheduleEntry existing;
        if (!storage_->loadScheduleEntry(i, existing))
            continue;

        if (entry.overlapsWith(existing)) {
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
    if (totalLength_ >= MAX_MESSAGE_SIZE - 2) {
        return;  // Reserve space for checksum + END_BYTE
    }
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
    addUint16(entry.periodMinutes);
    addUint16(entry.windowMinutes);
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
    : wakeInterval_(300), nextSeqNum_(SEQ_STM32_MIN), currentSeqNum_(0), uartSend_(uartSend),
      setWake_(setWake), keepAwake_(keepAwake), readyForSleep_(readyForSleep), getTick_(getTick),
      seenIndex_(0), nodeStateValid_(false), valveResetPending_(false), clearToSendReceived_(false),
      pendingMessageReady_(false), pendingSeqNum_(0), pendingCommand_(static_cast<Command>(0)),
      pendingDataLen_(0), pendingMessageDropCount_(0), storage_(nullptr)
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
    // Called from UART RX ISR — keep work minimal.
    // Parse bytes; on complete frame, stash into pending slot for main-loop dispatch.
    // Do NOT call handlers or sendAck here — they would block UART TX and cause
    // OVERRUN on subsequent RX bytes during the long blocking transmit.
    if (parser_.processByte(byte)) {
        if (pendingMessageReady_) {
            // Previous message hasn't been dispatched yet. Drop the new one;
            // the RP2040 reliability layer will retry.
            pendingMessageDropCount_++;
        } else {
            pendingSeqNum_ = parser_.getSequenceNumber();
            pendingCommand_ = parser_.getCommand();
            pendingDataLen_ = parser_.getDataLength();
            const uint8_t *src = parser_.getData();
            if (src && pendingDataLen_ > 0 && pendingDataLen_ <= sizeof(pendingData_)) {
                for (uint8_t i = 0; i < pendingDataLen_; i++) {
                    pendingData_[i] = src[i];
                }
            } else {
                pendingDataLen_ = 0;
            }
            pendingMessageReady_ = true;
        }
        parser_.reset();
    }
}

void Protocol::processPendingMessage()
{
    if (!pendingMessageReady_) {
        return;
    }

    uint8_t seqNum = pendingSeqNum_;
    Command cmd = pendingCommand_;
    const uint8_t *data = pendingData_;
    uint8_t dataLen = pendingDataLen_;

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
                    handleReadyForSleep(data, dataLen);
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
            case Command::FactoryReset:
                // Wipe FRAM persistent storage, then reset
                // Always send ACK first so RP2040 knows the command was received
                sendAck();
                if (!isDuplicate) {
                    // Brief delay to allow UART to flush the ACK
                    for (volatile uint32_t i = 0; i < 10000; i++) {}
                    if (storage_) {
                        storage_->factoryReset();
                    }
                    NVIC_SystemReset();
                }
                break;
            case Command::SetValveTimer:
                if (!isDuplicate) {
                    handleSetValveTimer(data, dataLen);
                } else {
                    sendAck();
                }
                break;
            case Command::SaveBlob:
                if (!isDuplicate) {
                    handleSaveBlob(data, dataLen);
                } else {
                    sendAck();
                }
                break;
            case Command::LoadBlob:
                if (!isDuplicate) {
                    handleLoadBlob(data, dataLen);
                } else {
                    sendAck();
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

    // Release the pending slot so the next received frame can be queued
    pendingMessageReady_ = false;
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
    // Valve-reset flag follows the state blob (force valves closed on power-on / NRST)
    builder_.addByte(valveResetPending_ ? 0x01 : 0x00);
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
    // Valve-reset flag follows the state blob, before the schedule entry
    builder_.addByte(valveResetPending_ ? 0x01 : 0x00);
    // Add schedule entry after valve-reset flag
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

bool Protocol::getNextScheduledEntry(uint8_t currentDay, uint8_t currentHour, uint8_t currentMinute,
                                     ScheduleEntry &out) const
{
    return schedule_.findNextEntry(currentDay, currentHour, currentMinute, out);
}

void Protocol::setStorage(PersistentStorage *storage)
{
    storage_ = storage;
    schedule_.setStorage(storage);
}

void Protocol::loadFromStorage()
{
    if (!storage_ || !storage_->isAvailable())
        return;

    // Load wake interval
    uint32_t interval = 0;
    if (storage_->loadWakeInterval(interval) && interval > 0 && interval <= 86400) {
        wakeInterval_ = interval;
    }

    // Schedules are now read on demand from FRAM — nothing to load.

    // Load node state
    if (storage_->isNodeStateValid()) {
        if (storage_->loadNodeState(nodeState_, NODE_STATE_SIZE)) {
            nodeStateValid_ = true;
        }
    }
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

    // Persist to FRAM
    if (storage_) {
        storage_->saveWakeInterval(seconds);
    }

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
    // Payload is index byte followed by an 11-byte ScheduleEntry.
    if (length != SCHEDULE_ENTRY_SIZE + 1) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    uint8_t index = data[0];

    ScheduleEntry entry;
    entry.hour = data[1];
    entry.minute = data[2];
    entry.duration = data[3] | (data[4] << 8);
    entry.daysMask = static_cast<DayOfWeek>(data[5]);
    entry.valveId = data[6];
    entry.enabled = (data[7] != 0);
    entry.periodMinutes = data[8] | (data[9] << 8);
    entry.windowMinutes = data[10] | (data[11] << 8);

    ErrorCode result = schedule_.setEntry(index, entry);

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

void Protocol::handleReadyForSleep(const uint8_t *data, uint8_t length)
{
    if (data && length >= NODE_STATE_SIZE) {
        for (uint8_t i = 0; i < NODE_STATE_SIZE; i++) {
            nodeState_[i] = data[i];
        }
        nodeStateValid_ = true;

        if (storage_) {
            storage_->saveNodeState(nodeState_, NODE_STATE_SIZE);
        }
    }
    // If no state blob, keep previous state (backward compatibility)

    // The node has received the wake notification and completed a full cycle, so a
    // pending valve-reset has been delivered and acted on — clear the one-shot.
    valveResetPending_ = false;

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

void Protocol::handleSetValveTimer(const uint8_t *data, uint8_t length)
{
    // Payload: [duration_lo, duration_hi, valve_id] = 3 bytes
    if (length != 3) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    uint16_t durationSeconds = data[0] | (data[1] << 8);
    uint8_t valveId = data[2];

    if (durationSeconds == 0 || durationSeconds > 7200) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    sendAck();

    if (setValveTimer_) {
        setValveTimer_(durationSeconds, valveId);
    }
}

void Protocol::handleSaveBlob(const uint8_t *data, uint8_t length)
{
    // Payload: {slot:1, total_length:2, offset:2, chunk_length:1, data[0-36]}
    if (length < 6) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    uint8_t slot = data[0];
    uint16_t totalLength = data[1] | (data[2] << 8);
    uint16_t offset = data[3] | (data[4] << 8);
    uint8_t chunkLength = data[5];

    if (chunkLength > length - 6) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    if (!storage_) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    // Clear operation
    if (totalLength == 0) {
        if (storage_->clearBlob(slot)) {
            sendAck();
        } else {
            sendNack(ErrorCode::InvalidParam);
        }
        return;
    }

    // Set used_length on first chunk
    if (offset == 0) {
        if (!storage_->setBlobLength(slot, totalLength)) {
            sendNack(ErrorCode::InvalidParam);
            return;
        }
    }

    // Write chunk data
    if (chunkLength > 0) {
        if (!storage_->saveBlobChunk(slot, offset, data + 6, chunkLength)) {
            sendNack(ErrorCode::InvalidParam);
            return;
        }
    }

    sendAck();
}

void Protocol::handleLoadBlob(const uint8_t *data, uint8_t length)
{
    // Payload: {slot:1}
    if (length < 1) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    uint8_t slot = data[0];

    if (!storage_) {
        sendNack(ErrorCode::InvalidParam);
        return;
    }

    // ACK the command first
    sendAck();

    uint16_t totalLength = storage_->getBlobLength(slot);

    // Send chunked BlobData responses
    uint16_t offset = 0;
    do {
        uint8_t chunkSize = MAX_BLOB_CHUNK_SIZE;
        if (offset + chunkSize > totalLength) {
            chunkSize = static_cast<uint8_t>(totalLength - offset);
        }

        // Read chunk from FRAM
        uint8_t chunkData[MAX_BLOB_CHUNK_SIZE];
        uint8_t bytesRead = 0;
        if (chunkSize > 0) {
            bytesRead = storage_->loadBlobChunk(slot, offset, chunkData, chunkSize);
        }

        // Build BlobData response: {slot:1, total_length:2, offset:2, chunk_length:1, data[]}
        builder_.startMessage(getNextSeqNum(), static_cast<uint8_t>(Response::BlobData));
        builder_.addByte(slot);
        builder_.addUint16(totalLength);
        builder_.addUint16(offset);
        builder_.addByte(bytesRead);
        for (uint8_t i = 0; i < bytesRead; i++) {
            builder_.addByte(chunkData[i]);
        }
        sendMessage();

        offset += bytesRead;
    } while (offset < totalLength);

    // If totalLength was 0, we still need to send one empty BlobData response
    if (totalLength == 0) {
        builder_.startMessage(getNextSeqNum(), static_cast<uint8_t>(Response::BlobData));
        builder_.addByte(slot);
        builder_.addUint16(0);
        builder_.addUint16(0);
        builder_.addByte(0);
        sendMessage();
    }
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
    ScheduleEntry entry;
    if (!schedule_.getEntry(index, entry)) {
        sendNack(ErrorCode::InvalidIndex);
        return;
    }

    // Echo the sequence number from the received command
    builder_.startMessage(currentSeqNum_, static_cast<uint8_t>(Response::ScheduleEntry));
    builder_.addScheduleEntry(entry);
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
