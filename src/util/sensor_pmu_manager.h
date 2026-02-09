#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "../hal/pmu_client.h"
#include "../hal/pmu_reliability.h"
#include "task_queue.h"

class ReliableMessenger;
class SensorFlashBuffer;

/**
 * @brief Persisted state stored in PMU RAM across sleep cycles
 *
 * This struct is packed into a 32-byte opaque blob and sent to the PMU
 * with ReadyForSleep. The PMU stores it in RAM (always powered from battery)
 * and returns it in the WakeReason notification.
 *
 * On cold start (battery disconnect), state_valid=false and we reconstruct
 * from flash scan.
 */
struct __attribute__((packed)) SensorPersistedState {
    uint8_t version;            // Format version (for future compatibility)
    uint8_t next_seq_num;       // LoRa sequence number
    uint16_t assigned_address;  // Node address (survives warm reboot, lost on cold start)
    uint32_t read_index;        // Flash buffer read position
    uint32_t write_index;       // Flash buffer write position
    uint8_t padding[20];        // Reserved for future use
};
static_assert(sizeof(SensorPersistedState) == 32, "SensorPersistedState must be 32 bytes");

// Current state format version - bumped for assigned_address field
constexpr uint8_t STATE_VERSION = 2;

/**
 * @brief Manages PMU hardware and protocol for SensorMode
 *
 * Owns PmuClient and ReliablePmuClient, encapsulates:
 * - ClearToSend handshake and wake notification handling
 * - State persistence across sleep/wake (pack/unpack SensorPersistedState)
 * - RTC sync to/from PMU battery-backed clock
 * - ReadyForSleep signaling with state blob
 * - System reset via PMU
 *
 * SensorMode orchestrates everything via callbacks — this class only does PMU I/O.
 */
class SensorPmuManager {
public:
    /**
     * @brief Callback when PMU initialization completes (wake notification or timeout)
     * @param state_restored true if PMU state was unpacked and applied to messenger/flash
     * @param reason Wake reason from PMU
     */
    using InitCallback = std::function<void(bool state_restored, PMU::WakeReason reason)>;

    /**
     * @brief Callback when PMU RTC datetime is retrieved
     * @param valid true if datetime is valid
     * @param datetime The retrieved datetime
     */
    using TimeCallback = std::function<void(bool valid, const PMU::DateTime &datetime)>;

    /**
     * @brief Callback when a PMU command completes
     * @param success true if command succeeded
     */
    using CommandCallback = std::function<void(bool success)>;

    /**
     * @brief Construct a SensorPmuManager
     * @param messenger Reference to LoRa messenger (for seq num and node address)
     * @param flash_buffer Pointer to flash buffer (nullable, for read/write indices)
     * @param task_queue Reference to task queue (for deferred scheduling)
     */
    SensorPmuManager(ReliableMessenger &messenger, SensorFlashBuffer *flash_buffer,
                     TaskQueue &task_queue);

    ~SensorPmuManager();

    /**
     * @brief Initialize PMU hardware and start ClearToSend handshake
     *
     * Creates PmuClient, ReliablePmuClient, performs ClearToSend handshake.
     * Calls init_callback once (either on wake notification or timeout).
     *
     * @param init_callback Called when initialization completes
     * @return false if PMU hardware unavailable
     */
    bool initialize(InitCallback init_callback);

    /**
     * @brief Process PMU reliable client (call from main loop)
     */
    void update();

    /**
     * @brief Request datetime from PMU's battery-backed RTC
     * @param callback Called with datetime result
     */
    void requestTime(TimeCallback callback);

    /**
     * @brief Set PMU RTC to given datetime
     * @param datetime Datetime to set
     * @param callback Called when command completes (optional)
     */
    void syncTime(const PMU::DateTime &datetime, CommandCallback callback = nullptr);

    /**
     * @brief Pack state from messenger/flash and send ReadyForSleep to PMU
     *
     * Packs current LoRa seq num, node address, and flash indices into a
     * 32-byte blob. Sends via deferred task to avoid callback-chain stack issues.
     */
    void signalReadyForSleep();

    /**
     * @brief Request full system reset via PMU
     *
     * Sends SystemReset command. On ACK (or failure), performs watchdog reboot
     * as fallback in case PMU doesn't kill power immediately.
     */
    void requestSystemReset();

    /**
     * @brief Check if PMU hardware is available
     */
    bool isAvailable() const { return pmu_available_; }

    /**
     * @brief Check if sleep has been signaled and acknowledged
     *
     * When true, the main loop should halt to prevent UART activity from
     * keeping the STM32 awake after dcdc.disable().
     */
    bool isSleepPending() const { return sleep_pending_; }

private:
    // PMU UART configuration
    static constexpr uint PMU_UART_TX_PIN = 0;
    static constexpr uint PMU_UART_RX_PIN = 1;
    static constexpr uint PMU_UART_BAUDRATE = 9600;

    // Timeout for wake notification after ClearToSend
    static constexpr uint32_t WAKE_NOTIFICATION_TIMEOUT_MS = 1000;

    ReliableMessenger &messenger_;
    SensorFlashBuffer *flash_buffer_;
    TaskQueue &task_queue_;

    PmuClient *pmu_client_ = nullptr;
    PMU::ReliablePmuClient *reliable_pmu_ = nullptr;
    bool pmu_available_ = false;
    bool sleep_pending_ = false;
    uint16_t wake_timeout_id_ = 0;

    InitCallback init_callback_;

    /**
     * @brief Handle PMU wake notification (called from ReliablePmuClient callback)
     * @param reason Wake reason from PMU
     * @param entry Schedule entry (if scheduled wake)
     * @param state_valid true if state blob is valid (false on cold start)
     * @param state 32-byte state blob from PMU RAM (or null)
     */
    void handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry, bool state_valid,
                       const uint8_t *state);

    /**
     * @brief Pack current state into SensorPersistedState
     * @param out State struct to fill
     */
    void packState(SensorPersistedState &out) const;

    /**
     * @brief Restore state from SensorPersistedState blob
     * @param persisted State to restore
     * @return true if state was restored successfully
     */
    bool unpackState(const SensorPersistedState *persisted);
};
