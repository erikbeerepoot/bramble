#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <queue>

#include "pico/stdlib.h"

#include "pmu_client.h"
#include "pmu_protocol.h"

namespace PMU {

// Reliability configuration
namespace Reliability {
constexpr uint32_t BASE_TIMEOUT_MS = 500;
constexpr uint32_t MAX_TIMEOUT_MS = 5000;
constexpr float BACKOFF_MULTIPLIER = 2.0f;
constexpr size_t MAX_QUEUE_DEPTH = 8;
constexpr size_t DEDUP_BUFFER_SIZE = 8;
constexpr uint32_t DEDUP_WINDOW_MS = 5000;
}  // namespace Reliability

/**
 * @brief Callback for command completion
 * @param success true if ACK received, false if NACK
 * @param error Error code if NACK received
 */
using CommandCallback = std::function<void(bool success, ErrorCode error)>;

/**
 * @brief Callback for wake notifications from PMU
 * @param reason Why the device was woken
 * @param entry Schedule entry if wake was scheduled (may be nullptr)
 * @param state_valid true if state blob is valid (false on cold start)
 * @param state 32-byte opaque state blob stored in PMU RAM across sleep cycles
 */
using WakeCallback = std::function<void(WakeReason reason, const ScheduleEntry *entry,
                                        bool state_valid, const uint8_t *state)>;

/**
 * @brief Reliable PMU client with automatic retry
 *
 * Wraps PmuClient to provide:
 * - Automatic retry with exponential backoff
 * - Sequence number tracking
 * - Deduplication of incoming messages
 * - Command queue management
 */
class ReliablePmuClient {
public:
    /**
     * @brief Construct a reliable PMU client
     * @param client Pointer to underlying PmuClient (must remain valid)
     */
    explicit ReliablePmuClient(PmuClient *client);

    /**
     * @brief Initialize the reliable client
     * @return true if initialization successful
     */
    bool init();

    /**
     * @brief Process pending operations (call from main loop)
     *
     * Handles:
     * - Timeout detection and retry
     * - Received byte processing
     * - Queue management
     */
    void update();

    // ========================================================================
    // Reliable commands (all retry until ACK received)
    // ========================================================================

    /**
     * @brief Set the periodic wake interval
     * @param seconds Wake interval in seconds
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool setWakeInterval(uint32_t seconds, CommandCallback callback = nullptr);

    /**
     * @brief Set a schedule entry
     * @param entry Schedule entry to set
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool setSchedule(const ScheduleEntry &entry, CommandCallback callback = nullptr);

    /**
     * @brief Set the RTC date/time
     * @param dateTime Date and time to set
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool setDateTime(const DateTime &dateTime, CommandCallback callback = nullptr);

    /**
     * @brief Clear a schedule entry (or all entries)
     * @param index Entry index to clear (0xFF = clear all)
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool clearSchedule(uint8_t index = 0xFF, CommandCallback callback = nullptr);

    /**
     * @brief Keep the system awake for a period
     * @param seconds Seconds to stay awake
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool keepAwake(uint16_t seconds, CommandCallback callback = nullptr);

    /**
     * @brief Signal ready for sleep/power down
     * @param state 32-byte state blob to persist in PMU RAM (nullptr for no state)
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool readyForSleep(const uint8_t *state = nullptr, CommandCallback callback = nullptr);

    /**
     * @brief Get the current wake interval
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool getWakeInterval(CommandCallback callback = nullptr);

    /**
     * @brief Get a schedule entry
     * @param index Entry index to retrieve
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     */
    bool getSchedule(uint8_t index, CommandCallback callback = nullptr);

    /**
     * @brief Get the current date/time from PMU's RTC
     * @param callback Called when datetime response is received
     * @return true if command queued successfully
     *
     * The command is sent through the reliable queue - STM32 sends ACK
     * followed by DateTimeResponse.
     */
    bool getDateTime(DateTimeCallback callback);

    /**
     * @brief Signal clear to send - RP2040 is ready to receive wake info
     * @param callback Called when command completes (optional)
     * @return true if command queued successfully
     *
     * This command tells the PMU that the RP2040 has initialized and is
     * ready to receive the wake notification with persisted state.
     */
    bool clearToSend(CommandCallback callback = nullptr);

    /**
     * @brief Request full system reset via PMU
     * @param callback Called when command is ACK'd (optional)
     * @return true if command queued successfully
     *
     * The PMU will ACK this command, then perform NVIC_SystemReset(),
     * which resets both the STM32 and kills power to the RP2040.
     * The callback's ACK triggers a watchdog_reboot() as a fallback
     * in case the PMU reset doesn't kill RP2040 power immediately.
     */
    bool systemReset(CommandCallback callback = nullptr);

    /**
     * @brief Get access to the underlying PmuClient
     * Useful for accessing the protocol directly for operations
     * not covered by the reliable client.
     */
    PmuClient *getClient() { return client_; }

    // ========================================================================
    // Event callbacks
    // ========================================================================

    /**
     * @brief Set callback for wake notifications
     * @param callback Called when PMU sends wake notification
     */
    void onWake(WakeCallback callback);

    /**
     * @brief Set callback for schedule complete notifications
     * @param callback Called when scheduled operation completes
     */
    void onScheduleComplete(ScheduleCompleteCallback callback);

    /**
     * @brief Set callback for wake interval response
     * @param callback Called when wake interval is received
     */
    void onWakeInterval(WakeIntervalCallback callback);

    /**
     * @brief Set callback for schedule entry response
     * @param callback Called when schedule entry is received
     */
    void onScheduleEntry(ScheduleEntryCallback callback);

    // ========================================================================
    // Status
    // ========================================================================

    /**
     * @brief Get number of pending commands in queue
     */
    size_t getPendingCount() const;

    /**
     * @brief Check if any commands are pending
     */
    bool hasPendingCommands() const;

    /**
     * @brief Check if underlying client is initialized
     */
    bool isInitialized() const;

private:
    // Pending command structure
    struct PendingCommand {
        uint8_t seqNum;
        Command command;
        std::unique_ptr<uint8_t[]> data;
        uint8_t dataLength;
        uint32_t sendTime;
        uint8_t attempts;
        CommandCallback callback;
    };

    // Deduplication entry
    struct SeenMessage {
        uint8_t seqNum;
        uint32_t timestamp;
    };

    PmuClient *client_;
    uint8_t nextSeqNum_;

    // Command queue (one in-flight at a time)
    std::queue<PendingCommand> commandQueue_;
    std::optional<PendingCommand> inFlight_;

    // Deduplication for STM32→RP2040 messages
    SeenMessage seenBuffer_[Reliability::DEDUP_BUFFER_SIZE];
    size_t seenIndex_;

    // Event callbacks
    WakeCallback wakeCallback_;
    ScheduleCompleteCallback scheduleCompleteCallback_;
    WakeIntervalCallback wakeIntervalCallback_;
    ScheduleEntryCallback scheduleEntryCallback_;
    DateTimeCallback pendingDateTimeCallback_;

    // Internal helpers
    uint8_t getNextSeqNum();
    uint32_t getTimeout(uint8_t attempts) const;
    void handleAck(uint8_t seqNum, bool success, ErrorCode error);
    void retryCommand();
    void sendNextQueued();
    bool wasRecentlySeen(uint8_t seqNum) const;
    void markAsSeen(uint8_t seqNum);
    bool queueCommand(Command cmd, const uint8_t *data, uint8_t dataLen, CommandCallback callback);
    void sendCommand(PendingCommand &cmd);
};

}  // namespace PMU
