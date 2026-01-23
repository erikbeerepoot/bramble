#include "pmu_reliability.h"

#include <algorithm>
#include <cstring>

#include "hal/logger.h"

static Logger log("PMU_REL");

namespace PMU {

ReliablePmuClient::ReliablePmuClient(PmuClient *client)
    : client_(client), nextSeqNum_(SEQ_RP2040_MIN), seenIndex_(0), wakeCallback_(nullptr),
      scheduleCompleteCallback_(nullptr), wakeIntervalCallback_(nullptr),
      scheduleEntryCallback_(nullptr), pendingDateTimeCallback_(nullptr)
{
    // Initialize deduplication buffer
    for (auto &entry : seenBuffer_) {
        entry.seqNum = 0;
        entry.timestamp = 0;
    }
}

bool ReliablePmuClient::init()
{
    if (!client_) {
        return false;
    }

    // Initialize underlying client if not already
    if (!client_->isInitialized()) {
        if (!client_->init()) {
            return false;
        }
    }

    // Set up ACK callback to handle responses
    client_->getProtocol().setAckCallback([this](uint8_t seqNum, bool success, ErrorCode error) {
        this->handleAck(seqNum, success, error);
    });

    // Forward event callbacks to protocol
    client_->getProtocol().onWakeNotification(
        [this](WakeReason reason, const ScheduleEntry *entry) {
            if (wakeCallback_) {
                wakeCallback_(reason, entry);
            }
        });

    client_->getProtocol().onScheduleComplete([this]() {
        if (scheduleCompleteCallback_) {
            scheduleCompleteCallback_();
        }
    });

    client_->getProtocol().onWakeInterval([this](uint32_t seconds) {
        if (wakeIntervalCallback_) {
            wakeIntervalCallback_(seconds);
        }
    });

    client_->getProtocol().onScheduleEntry([this](const ScheduleEntry &entry) {
        if (scheduleEntryCallback_) {
            scheduleEntryCallback_(entry);
        }
    });

    return true;
}

void ReliablePmuClient::update()
{
    if (!client_)
        return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Check for in-flight timeout
    if (inFlight_) {
        uint32_t elapsed = now - inFlight_->sendTime;
        uint32_t timeout = getTimeout(inFlight_->attempts);

        if (elapsed > timeout) {
            log.info("Timeout on seq %d, attempt %d (elapsed=%lu, timeout=%lu)", inFlight_->seqNum,
                     inFlight_->attempts, elapsed, timeout);
            retryCommand();
        }
    }

    // Process received bytes
    client_->process();
}

uint8_t ReliablePmuClient::getNextSeqNum()
{
    uint8_t seq = nextSeqNum_++;
    if (nextSeqNum_ > SEQ_RP2040_MAX) {
        nextSeqNum_ = SEQ_RP2040_MIN;
    }
    return seq;
}

uint32_t ReliablePmuClient::getTimeout(uint8_t attempts) const
{
    uint32_t timeout = Reliability::BASE_TIMEOUT_MS;
    for (uint8_t i = 1; i < attempts; i++) {
        timeout = static_cast<uint32_t>(timeout * Reliability::BACKOFF_MULTIPLIER);
        if (timeout >= Reliability::MAX_TIMEOUT_MS) {
            return Reliability::MAX_TIMEOUT_MS;
        }
    }
    return timeout;
}

void ReliablePmuClient::handleAck(uint8_t seqNum, bool success, ErrorCode error)
{
    // Check if this ACK matches our in-flight command
    if (!inFlight_ || inFlight_->seqNum != seqNum) {
        log.info("ACK for unknown seq %d (expected %d)", seqNum, inFlight_ ? inFlight_->seqNum : 0);
        return;
    }

    log.info("ACK received for seq %d, success=%d", seqNum, success);

    // Call the callback if set
    if (inFlight_->callback) {
        inFlight_->callback(success, error);
    }

    // Clear in-flight and send next queued command
    inFlight_.reset();
    sendNextQueued();
}

void ReliablePmuClient::retryCommand()
{
    if (!inFlight_)
        return;

    inFlight_->attempts++;
    inFlight_->sendTime = to_ms_since_boot(get_absolute_time());

    log.debug("Retry seq %d, attempt %d", inFlight_->seqNum, inFlight_->attempts);

    // Re-send the command
    sendCommand(*inFlight_);
}

void ReliablePmuClient::sendNextQueued()
{
    if (inFlight_ || commandQueue_.empty()) {
        return;
    }

    // Move front of queue to in-flight
    inFlight_ = std::move(commandQueue_.front());
    commandQueue_.pop();

    // Send the command
    sendCommand(*inFlight_);
}

void ReliablePmuClient::sendCommand(PendingCommand &cmd)
{
    cmd.sendTime = to_ms_since_boot(get_absolute_time());
    client_->getProtocol().sendCommand(cmd.seqNum, cmd.command, cmd.data.get(), cmd.dataLength);
}

bool ReliablePmuClient::wasRecentlySeen(uint8_t seqNum) const
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (size_t i = 0; i < Reliability::DEDUP_BUFFER_SIZE; i++) {
        if (seenBuffer_[i].seqNum == seqNum &&
            (now - seenBuffer_[i].timestamp) < Reliability::DEDUP_WINDOW_MS) {
            return true;
        }
    }
    return false;
}

void ReliablePmuClient::markAsSeen(uint8_t seqNum)
{
    seenBuffer_[seenIndex_].seqNum = seqNum;
    seenBuffer_[seenIndex_].timestamp = to_ms_since_boot(get_absolute_time());
    seenIndex_ = (seenIndex_ + 1) % Reliability::DEDUP_BUFFER_SIZE;
}

bool ReliablePmuClient::queueCommand(Command cmd, const uint8_t *data, uint8_t dataLen,
                                     CommandCallback callback)
{
    // Check queue depth
    if (commandQueue_.size() >= Reliability::MAX_QUEUE_DEPTH) {
        log.warn("Command queue full, rejecting command");
        return false;
    }

    // Create pending command
    PendingCommand pending;
    pending.seqNum = getNextSeqNum();
    pending.command = cmd;
    pending.dataLength = dataLen;
    pending.sendTime = 0;
    pending.attempts = 1;  // First attempt
    pending.callback = callback;

    // Copy data if present
    if (dataLen > 0 && data != nullptr) {
        pending.data = std::make_unique<uint8_t[]>(dataLen);
        std::memcpy(pending.data.get(), data, dataLen);
    }

    log.debug("Queuing command 0x%02X with seq %d", static_cast<uint8_t>(cmd), pending.seqNum);

    // If nothing in-flight, send immediately
    if (!inFlight_) {
        inFlight_ = std::move(pending);
        sendCommand(*inFlight_);
    } else {
        commandQueue_.push(std::move(pending));
    }

    return true;
}

// ============================================================================
// Reliable commands
// ============================================================================

bool ReliablePmuClient::setWakeInterval(uint32_t seconds, CommandCallback callback)
{
    uint8_t data[4];
    data[0] = seconds & 0xFF;
    data[1] = (seconds >> 8) & 0xFF;
    data[2] = (seconds >> 16) & 0xFF;
    data[3] = (seconds >> 24) & 0xFF;
    return queueCommand(Command::SetWakeInterval, data, 4, callback);
}

bool ReliablePmuClient::setSchedule(const ScheduleEntry &entry, CommandCallback callback)
{
    uint8_t data[SCHEDULE_ENTRY_SIZE];
    data[0] = entry.hour;
    data[1] = entry.minute;
    data[2] = entry.duration & 0xFF;
    data[3] = (entry.duration >> 8) & 0xFF;
    data[4] = static_cast<uint8_t>(entry.daysMask);
    data[5] = entry.valveId;
    data[6] = entry.enabled ? 1 : 0;
    return queueCommand(Command::SetSchedule, data, SCHEDULE_ENTRY_SIZE, callback);
}

bool ReliablePmuClient::setDateTime(const DateTime &dateTime, CommandCallback callback)
{
    uint8_t data[7];
    data[0] = dateTime.year;
    data[1] = dateTime.month;
    data[2] = dateTime.day;
    data[3] = dateTime.weekday;
    data[4] = dateTime.hour;
    data[5] = dateTime.minute;
    data[6] = dateTime.second;
    return queueCommand(Command::SetDateTime, data, 7, callback);
}

bool ReliablePmuClient::clearSchedule(uint8_t index, CommandCallback callback)
{
    return queueCommand(Command::ClearSchedule, &index, 1, callback);
}

bool ReliablePmuClient::keepAwake(uint16_t seconds, CommandCallback callback)
{
    uint8_t data[2];
    data[0] = seconds & 0xFF;
    data[1] = (seconds >> 8) & 0xFF;
    return queueCommand(Command::KeepAwake, data, 2, callback);
}

bool ReliablePmuClient::readyForSleep(CommandCallback callback)
{
    return queueCommand(Command::ReadyForSleep, nullptr, 0, callback);
}

bool ReliablePmuClient::getWakeInterval(CommandCallback callback)
{
    return queueCommand(Command::GetWakeInterval, nullptr, 0, callback);
}

bool ReliablePmuClient::getSchedule(uint8_t index, CommandCallback callback)
{
    return queueCommand(Command::GetSchedule, &index, 1, callback);
}

bool ReliablePmuClient::getDateTime(DateTimeCallback callback)
{
    // Store callback for when DateTimeResponse arrives
    pendingDateTimeCallback_ = callback;

    // Set up the Protocol's callback to forward to our stored callback
    // This must be done each time since handleDateTimeResponse clears it after use
    client_->getProtocol().onDateTime([this](bool valid, const DateTime &datetime) {
        if (pendingDateTimeCallback_) {
            auto cb = pendingDateTimeCallback_;
            pendingDateTimeCallback_ = nullptr;
            cb(valid, datetime);
        }
    });

    // Queue command - ACK confirms delivery, DateTimeResponse handled via onDateTime
    return queueCommand(Command::GetDateTime, nullptr, 0, [](bool success, ErrorCode error) {
        // ACK received - DateTimeResponse will follow
        // If NACK, the DateTimeCallback won't be called (already cleared)
    });
}

// ============================================================================
// Event callbacks
// ============================================================================

void ReliablePmuClient::onWake(WakeCallback callback)
{
    wakeCallback_ = callback;
}

void ReliablePmuClient::onScheduleComplete(ScheduleCompleteCallback callback)
{
    scheduleCompleteCallback_ = callback;
}

void ReliablePmuClient::onWakeInterval(WakeIntervalCallback callback)
{
    wakeIntervalCallback_ = callback;
}

void ReliablePmuClient::onScheduleEntry(ScheduleEntryCallback callback)
{
    scheduleEntryCallback_ = callback;
}

// ============================================================================
// Status
// ============================================================================

size_t ReliablePmuClient::getPendingCount() const
{
    return commandQueue_.size() + (inFlight_ ? 1 : 0);
}

bool ReliablePmuClient::hasPendingCommands() const
{
    return inFlight_.has_value() || !commandQueue_.empty();
}

bool ReliablePmuClient::isInitialized() const
{
    return client_ && client_->isInitialized();
}

}  // namespace PMU
