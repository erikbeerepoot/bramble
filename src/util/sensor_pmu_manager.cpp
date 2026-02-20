#include "sensor_pmu_manager.h"

#include <cstring>

#include "pico/stdlib.h"

#include "hardware/watchdog.h"

#include "../hal/logger.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../storage/sensor_flash_buffer.h"

// PMU UART instance - must match hardware wiring
#define PMU_UART_ID uart0

static Logger pmu_logger("PMU");

SensorPmuManager::SensorPmuManager(ReliableMessenger &messenger, SensorFlashBuffer *flash_buffer,
                                   TaskQueue &task_queue)
    : messenger_(messenger), flash_buffer_(flash_buffer), task_queue_(task_queue)
{
}

SensorPmuManager::~SensorPmuManager()
{
    delete reliable_pmu_;
    delete pmu_client_;
}

bool SensorPmuManager::initialize(InitCallback init_callback)
{
    init_callback_ = std::move(init_callback);

    // Initialize PMU client at 9600 baud to match STM32 LPUART configuration
    pmu_client_ = new PmuClient(PMU_UART_ID, PMU_UART_TX_PIN, PMU_UART_RX_PIN, PMU_UART_BAUDRATE);
    pmu_available_ = pmu_client_->init();

    // Create reliable PMU client wrapper for automatic retry
    if (pmu_available_) {
        reliable_pmu_ = new PMU::ReliablePmuClient(pmu_client_);
        reliable_pmu_->init();

        // Allow UART to stabilize before first message - prevents parser desync
        sleep_ms(150);
    }

    if (!pmu_available_ || !reliable_pmu_) {
        pmu_logger.warn("PMU client not available");
        return false;
    }

    pmu_logger.info("PMU client initialized successfully");

    // Set up PMU callback handler - this receives state and completes initialization
    reliable_pmu_->onWake(
        [this](PMU::WakeReason reason, const PMU::ScheduleEntry *entry, bool state_valid,
               const uint8_t *state) { this->handlePmuWake(reason, entry, state_valid, state); });

    // Signal ready to receive wake info - PMU will respond with wake notification
    // containing persisted state (write_index, read_index, etc.)
    pmu_logger.debug("Sending ClearToSend to PMU...");
    reliable_pmu_->clearToSend([this](bool success, PMU::ErrorCode error) {
        if (!success) {
            pmu_logger.error("ClearToSend failed: %d", static_cast<int>(error));
            // Fall back to proceeding without waiting for PMU state
            if (init_callback_) {
                init_callback_(false, PMU::WakeReason::Periodic);
            }
        } else {
            pmu_logger.info("ClearToSend ACK received - waiting for WakeNotification");
            // Start timeout — if WakeNotification doesn't arrive, proceed without PMU state
            uint32_t now = to_ms_since_boot(get_absolute_time());
            wake_timeout_id_ = task_queue_.postDelayed(
                [](void *ctx, uint32_t) -> bool {
                    SensorPmuManager *self = static_cast<SensorPmuManager *>(ctx);
                    pmu_logger.warn("WakeNotification timeout - proceeding without PMU state");
                    self->wake_timeout_id_ = 0;
                    if (self->init_callback_) {
                        self->init_callback_(false, PMU::WakeReason::Periodic);
                        self->init_callback_ = nullptr;
                    }
                    return true;
                },
                this, now, WAKE_NOTIFICATION_TIMEOUT_MS, TaskPriority::High);
        }
    });

    return true;
}

void SensorPmuManager::update()
{
    if (pmu_available_ && reliable_pmu_) {
        reliable_pmu_->update();
    }
}

void SensorPmuManager::requestTime(TimeCallback callback)
{
    if (!pmu_available_ || !reliable_pmu_) {
        if (callback) {
            callback(false, PMU::DateTime());
        }
        return;
    }

    pmu_logger.debug("Requesting datetime from PMU...");
    reliable_pmu_->getDateTime([callback](bool valid, const PMU::DateTime &datetime) {
        if (callback) {
            callback(valid, datetime);
        }
    });
}

void SensorPmuManager::syncTime(const PMU::DateTime &datetime, CommandCallback callback)
{
    if (!pmu_available_ || !reliable_pmu_) {
        if (callback) {
            callback(false);
        }
        return;
    }

    pmu_logger.info("Syncing time to PMU: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                    datetime.month, datetime.day, datetime.hour, datetime.minute, datetime.second);

    reliable_pmu_->setDateTime(datetime, [callback](bool success, PMU::ErrorCode error) {
        if (success) {
            pmu_logger.info("PMU time sync successful");
        } else {
            pmu_logger.error("PMU time sync failed: error %d", static_cast<int>(error));
        }
        if (callback) {
            callback(success);
        }
    });
}

void SensorPmuManager::signalReadyForSleep()
{
    if (!pmu_available_ || !reliable_pmu_) {
        pmu_logger.debug("PMU not available, skipping ready for sleep signal");
        return;
    }

    // Post deferred task to send sleep signal in main loop
    // This avoids calling PMU protocol directly from callback chains (stack safety)
    // Using postOnce() to deduplicate - multiple code paths call this but we only
    // need one ReadyForSleep command per wake cycle
    pmu_logger.debug("Requesting sleep (posting deferred task)");

    task_queue_.postOnce(
        [](void *ctx, uint32_t time) -> bool {
            (void)time;
            SensorPmuManager *self = static_cast<SensorPmuManager *>(ctx);

            // Pack state fresh at send time
            SensorPersistedState state;
            self->packState(state);

            pmu_logger.info(
                "Sending ReadyForSleep with state (seq=%u, addr=0x%04X, read=%lu, write=%lu)",
                state.next_seq_num, state.assigned_address, state.read_index, state.write_index);

            self->reliable_pmu_->readyForSleep(
                reinterpret_cast<const uint8_t *>(&state),
                [self](bool success, PMU::ErrorCode error) {
                    if (success) {
                        pmu_logger.info("Ready for sleep acknowledged - halting");
                        // Set flag to halt main loop - prevents UART activity from keeping
                        // STM32 awake when USB power keeps RP2040 running after dcdc.disable()
                        self->sleep_pending_ = true;
                    } else {
                        pmu_logger.error("Ready for sleep failed: error %d",
                                         static_cast<int>(error));
                    }
                });
            return true;  // Task complete
        },
        this, TaskPriority::Low);
}

void SensorPmuManager::requestSystemReset()
{
    if (pmu_available_ && reliable_pmu_) {
        pmu_logger.warn("Requesting full system reset via PMU");
        reliable_pmu_->systemReset([](bool success, PMU::ErrorCode error) {
            (void)error;
            (void)success;
            // PMU will reset itself (killing RP2040 power), but if the ACK
            // arrives before power is cut, do a watchdog reboot as fallback.
            // Also reboot if PMU command failed - we always honor reboot requests.
            watchdog_reboot(0, 0, 0);
        });
    } else {
        pmu_logger.warn("PMU not available - performing RP2040-only watchdog reboot");
        watchdog_reboot(0, 0, 0);
    }
}

void SensorPmuManager::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry,
                                     bool state_valid, const uint8_t *state)
{
    (void)entry;

    // Cancel the wake notification timeout — PMU responded
    task_queue_.cancel(wake_timeout_id_);
    wake_timeout_id_ = 0;

    // Restore state from PMU RAM if valid
    bool restored = false;
    if (state_valid && state != nullptr) {
        const SensorPersistedState *persisted =
            reinterpret_cast<const SensorPersistedState *>(state);
        restored = unpackState(persisted);
    }

    // Validate restored write_index — if flash at that location isn't erased,
    // PMU state is stale (e.g. firmware upgrade reset it) and we need to scan.
    if (restored && flash_buffer_ && !flash_buffer_->isWriteLocationErased()) {
        pmu_logger.warn("PMU write_index=%lu is stale (flash not erased) - falling back to scan",
                        flash_buffer_->getWriteIndex());
        restored = false;
    }

    // Cold start: reconstruct state from flash scan
    if (!restored && flash_buffer_) {
        pmu_logger.info("Cold start detected - scanning flash for state");
        if (flash_buffer_->scanForWriteIndex()) {
            pmu_logger.info("Flash scan complete: write_index=%lu, read_index=%lu",
                            flash_buffer_->getWriteIndex(), flash_buffer_->getReadIndex());
        } else {
            pmu_logger.error("Flash scan failed - starting fresh");
        }
    }

    // Notify SensorMode via callback
    if (init_callback_) {
        init_callback_(restored, reason);
        init_callback_ = nullptr;
    }
}

void SensorPmuManager::packState(SensorPersistedState &out) const
{
    memset(&out, 0, sizeof(out));
    out.version = STATE_VERSION;
    out.next_seq_num = messenger_.getNextSeqNum();
    out.assigned_address = messenger_.getNodeAddress();
    if (flash_buffer_) {
        out.read_index = flash_buffer_->getReadIndex();
        out.write_index = flash_buffer_->getWriteIndex();
    }
}

bool SensorPmuManager::unpackState(const SensorPersistedState *persisted)
{
    if (!persisted) {
        return false;
    }

    // Check version compatibility
    if (persisted->version != STATE_VERSION) {
        pmu_logger.warn("State version mismatch (got %u, expected %u) - cold start",
                        persisted->version, STATE_VERSION);
        return false;
    }

    // Restore LoRa sequence number (no flash dependency)
    messenger_.setNextSeqNum(persisted->next_seq_num);

    // Restore assigned address from PMU RAM (no flash dependency)
    if (persisted->assigned_address != ADDRESS_UNREGISTERED) {
        messenger_.setNodeAddress(persisted->assigned_address);
    }

    // Restore flash buffer indices only if flash is available
    if (flash_buffer_) {
        flash_buffer_->setReadIndex(persisted->read_index);
        flash_buffer_->setWriteIndex(persisted->write_index);
        pmu_logger.info("Restored state: read=%lu, write=%lu, seq=%u, addr=0x%04X",
                        persisted->read_index, persisted->write_index, persisted->next_seq_num,
                        persisted->assigned_address);
    } else {
        pmu_logger.info("Restored state: seq=%u, addr=0x%04X (flash unavailable)",
                        persisted->next_seq_num, persisted->assigned_address);
    }

    return true;
}
