#include "sensor_mode.h"

#include <cstring>

#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "hardware/i2c.h"
#include "hardware/watchdog.h"

#include "../../main.h"
#include "../hal/cht832x.h"
#include "../hal/logger.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/network_stats.h"
#include "../lora/reliable_messenger.h"
#include "../util/time.h"
#include "../version.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;

static Logger logger("SENSOR");
static Logger pmu_logger("PMU");

SensorMode::~SensorMode() = default;

void SensorMode::onStart()
{
    logger.debug("LED: orange=init, yellow=sync, green=ok, orange=degraded, red=error");

    // Initialize external flash for sensor data storage
    external_flash_ = std::make_unique<ExternalFlash>();
    if (external_flash_->init()) {
        flash_buffer_ = std::make_unique<SensorFlashBuffer>(*external_flash_);
        if (flash_buffer_->init()) {
            SensorFlashMetadata stats;
            flash_buffer_->getStatistics(stats);
            logger.info("Flash buffer initialized: %lu records (%lu untransmitted)",
                        stats.total_records, flash_buffer_->getUntransmittedCount());
        } else {
            logger.error("Failed to initialize flash buffer!");
        }
    } else {
        logger.error("Failed to initialize external flash!");
    }

    // Create CHT832X sensor object (lazy init on first read)
    sensor_ = std::make_unique<CHT832X>(i2c1, PIN_I2C_SDA, PIN_I2C_SCL);
    logger.debug("CHT832X sensor created (lazy init on first read)");

    // Initial LED pattern: orange blink while initializing/awaiting time
    // Patterns are switched dynamically in onStateChange based on state:
    //   SYNCING_TIME: yellow fast short blink (50ms on, 250ms off)
    //   TIME_SYNCED/READING/CHECKING/TRANSMITTING/READY: green short blink
    //   DEGRADED_NO_SENSOR: orange short blink
    //   ERROR: red short blink
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);

    // Set up state change handler - centralizes all reactions to state changes
    sensor_state_.setCallback([this](SensorState state) { onStateChange(state); });

    // Set up registration success callback - transitions REGISTERING → AWAITING_TIME
    messenger_.setRegistrationSuccessCallback([this](uint16_t assigned_addr) {
        pmu_logger.info("Registration success callback: assigned address 0x%04X", assigned_addr);
        task_queue_.cancel(registration_timeout_id_);
        registration_timeout_id_ = 0;
        sensor_state_.reportRegistrationComplete();
    });

    // Initialize batch transmitter
    transmitter_ = std::make_unique<BatchTransmitter>(
        messenger_,
        BatchTransmitter::Config{.max_batches_per_cycle = 20, .hub_address = HUB_ADDRESS});

    // Initialize heartbeat client
    heartbeat_client_ = std::make_unique<HeartbeatClient>(messenger_);
    heartbeat_client_->setStatusProvider([this]() { return collectHeartbeatStatus(); });
    heartbeat_client_->setResponseCallback([this](const HeartbeatResponsePayload &response) {
        // SensorMode owns all side effects:
        // - Sync time to PMU if available
        // - Report to state machine
        if (pmu_manager_ && pmu_manager_->isAvailable()) {
            PMU::DateTime datetime(response.year % 100, response.month, response.day, response.dotw,
                                   response.hour, response.min, response.sec);
            pmu_manager_->syncTime(datetime, [this](bool success) {
                (void)success;
                sensor_state_.reportRtcSynced();
            });
        } else {
            sensor_state_.reportRtcSynced();
        }
    });
    heartbeat_client_->setDeliveryCallback([this](bool success) {
        if (!success && rtc_running()) {
            sensor_state_.reportRtcSynced();
        }
    });

    // Initialize PMU manager
    pmu_manager_ = std::make_unique<SensorPmuManager>(messenger_, flash_buffer_.get(), task_queue_);

    bool pmu_ok = pmu_manager_->initialize([this](bool state_restored, PMU::WakeReason reason) {
        // Ignore duplicate wake notifications if already active
        // This can happen if PMU sends multiple notifications (e.g., during LISTENING window)
        // We must check this BEFORE driving state machine, as transitions may reset state
        if (sensor_state_.state() != SensorState::INITIALIZING &&
            sensor_state_.state() != SensorState::READY_FOR_SLEEP) {
            pmu_logger.debug("Ignoring wake notification - already active (state: %s)",
                             SensorStateMachine::stateName(sensor_state_.state()));
            return;
        }

        // Complete initialization now that state is restored
        // This transitions INITIALIZING → REGISTERING, where the state callback
        // will check if registration is actually needed and proceed accordingly
        if (sensor_state_.state() == SensorState::INITIALIZING) {
            pmu_logger.debug("Marking initialized after state restoration");
            sensor_state_.markInitialized();
            // markInitialized triggers REGISTERING state, callback handles registration check
            return;
        }

        // Re-wake from READY_FOR_SLEEP (periodic/scheduled/external wake)
        switch (reason) {
            case PMU::WakeReason::Periodic:
                pmu_logger.info("Periodic wake.");
                // Report wake to state machine - it handles the appropriate transition
                if (!sensor_state_.reportWakeFromSleep()) {
                    // Need time sync first
                    pmu_logger.warn("RTC not synced on periodic wake - requesting time");
                    heartbeat_client_->send();
                    sensor_state_.expectResponse();
                    sensor_state_.reportHeartbeatSent();
                }
                break;

            case PMU::WakeReason::Scheduled:
                pmu_logger.warn("Unexpected scheduled wake in sensor mode");
                break;

            case PMU::WakeReason::External:
                pmu_logger.info("External wake trigger");
                break;
        }
    });

    if (!pmu_ok) {
        logger.warn("PMU client not available - sending heartbeat for time sync");
        heartbeat_client_->send();
        sensor_state_.expectResponse();
        sensor_state_.reportHeartbeatSent();
        // No PMU, so mark initialized immediately (no state to restore)
        sensor_state_.markInitialized();
    }
}

void SensorMode::onStateChange(SensorState state)
{
    // Centralized task scheduler - reacts to state changes by posting work
    switch (state) {
        case SensorState::AWAITING_TIME:
            // Request time sync (PMU first, then hub fallback)
            // Called after state is restored (markInitialized triggers this state)
            // Use task queue to defer - avoids reentrancy issues when called from PMU callback
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    self->requestTimeSync();
                    return true;
                },
                this, TaskPriority::High);
            break;

        case SensorState::SYNCING_TIME:
            // Waiting for hub response - yellow fast short blink
            led_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 255, 255, 0, 50, 250);
            break;

        case SensorState::TIME_SYNCED:
            // RTC valid - green short blink, initialize timestamps
            led_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 0, 255, 0);
            initializeFlashTimestamps();

            // If we just synced from hub (first boot), resend heartbeat with correct uptime
            // The initial heartbeat had uptime=0 because RTC wasn't set yet
            if (sensor_state_.previousState() == SensorState::SYNCING_TIME) {
                heartbeat_client_->send();
                sensor_state_.expectResponse();
            }

            // Try to initialize sensor
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    self->tryInitSensor();
                    // State machine handles transition based on sensor init result
                    return true;
                },
                this, TaskPriority::High);
            break;

        case SensorState::READING_SENSOR:
            // Take sensor reading
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    self->readAndStoreSensorData(to_ms_since_boot(get_absolute_time()));
                    self->sensor_state_.reportReadComplete();
                    return true;
                },
                this, TaskPriority::High);
            break;

        case SensorState::CHECKING_BACKLOG:
            // Check if transmission needed
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    bool needsTx = self->checkNeedsTransmission();
                    self->sensor_state_.reportCheckComplete(needsTx);
                    return true;
                },
                this, TaskPriority::High);
            break;

        case SensorState::TRANSMITTING:
            // Reset batch counter for this wake cycle
            transmitter_->resetCycleCounter();

            // Branch based on flash availability and health
            if (flash_buffer_ && flash_buffer_->isHealthy()) {
                // Normal path: transmit from flash backlog
                task_queue_.postOnce(
                    [](void *ctx, uint32_t time) -> bool {
                        (void)time;
                        SensorMode *self = static_cast<SensorMode *>(ctx);
                        self->transmitBacklog();
                        // Note: reportTransmitComplete called in transmitter callback
                        return true;
                    },
                    this, TaskPriority::High);
            } else {
                // Fallback path: direct transmit current reading (flash unavailable/unhealthy)
                task_queue_.postOnce(
                    [](void *ctx, uint32_t time) -> bool {
                        (void)time;
                        SensorMode *self = static_cast<SensorMode *>(ctx);
                        self->transmitCurrentReading();
                        return true;
                    },
                    this, TaskPriority::High);
            }
            break;

        case SensorState::LISTENING: {
            // Receive window open - stay awake briefly for hub responses
            uint32_t now = to_ms_since_boot(get_absolute_time());
            logger.info("Receive window open (%lu ms)", LISTEN_WINDOW_MS);
            task_queue_.postDelayed(
                [](void *ctx, uint32_t) -> bool {
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    logger.info("Receive window closed");
                    self->sensor_state_.reportListenComplete();
                    return true;
                },
                this, now, LISTEN_WINDOW_MS, TaskPriority::High);
            break;
        }

        case SensorState::READY_FOR_SLEEP:
            // All work done - signal PMU
            if (pmu_manager_) {
                pmu_manager_->signalReadyForSleep();
            }
            break;

        case SensorState::DEGRADED_NO_SENSOR:
            // Sensor failed - orange short blink, skip to backlog check
            led_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 255, 165, 0);
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    bool needsTx = self->checkNeedsTransmission();
                    self->sensor_state_.reportCheckComplete(needsTx);
                    return true;
                },
                this, TaskPriority::High);
            break;

        case SensorState::ERROR:
            // Red short blink for error
            led_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 255, 0, 0);
            logger.error("Entered ERROR state");
            break;

        case SensorState::INITIALIZING:
            // Initial state - nothing to do yet
            break;

        case SensorState::REGISTERING:
            // Yellow fast blink while checking/waiting for registration
            led_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 255, 255, 0, 50, 250);

            // Check if we actually need to register
            if (messenger_.getNodeAddress() != ADDRESS_UNREGISTERED) {
                // Already registered - skip to AWAITING_TIME
                logger.info("Already registered (addr=0x%04X) - skipping registration",
                            messenger_.getNodeAddress());
                sensor_state_.reportRegistrationComplete();
            } else {
                // Need to register - send message via deferred task to avoid reentrancy
                task_queue_.postOnce(
                    [](void *ctx, uint32_t time) -> bool {
                        (void)time;
                        SensorMode *self = static_cast<SensorMode *>(ctx);
                        self->attemptRegistration();
                        return true;
                    },
                    this, TaskPriority::High);
            }
            break;
    }
}

void SensorMode::onLoop()
{
    if (pmu_manager_ && pmu_manager_->isSleepPending()) {
        tight_loop_contents();
        return;
    }

    if (pmu_manager_) {
        pmu_manager_->update();
    }

    task_queue_.process(to_ms_since_boot(get_absolute_time()));
}

void SensorMode::readAndStoreSensorData(uint32_t current_time)
{
    if (!sensor_) {
        logger.error("Sensor object not created");
        return;
    }

    // Check if RTC has been synced - don't store readings with invalid timestamps
    if (!sensor_state_.isTimeSynced()) {
        logger.warn("RTC not synced yet (state=%s), skipping sensor storage",
                    SensorStateMachine::stateName(sensor_state_.state()));
        return;
    }

    // Lazy init: try to initialize sensor if not yet working
    if (!sensor_state_.hasSensor()) {
        if (!tryInitSensor()) {
            logger.warn("Sensor not ready (state=%s), will retry next cycle",
                        SensorStateMachine::stateName(sensor_state_.state()));
            return;
        }
    }

    CHT832X::Reading reading = sensor_->read();

    // Track sensor read validity for error reporting
    last_sensor_read_valid_ = reading.valid;

    if (!reading.valid) {
        logger.error("Failed to read sensor");
        return;
    }

    // Get Unix timestamp from RTC
    uint32_t unix_timestamp = getUnixTimestamp();
    if (unix_timestamp == 0) {
        logger.error("Failed to get Unix timestamp from RTC");
        return;
    }

    logger.info("Sensor reading: %.2fC, %.2f%%RH (ts=%lu)", reading.temperature, reading.humidity,
                unix_timestamp);

    // Convert to fixed-point format
    // Temperature: int16_t in 0.01C units (e.g., 2350 = 23.50C)
    // Humidity: uint16_t in 0.01% units (e.g., 6500 = 65.00%)
    int16_t temp_fixed = static_cast<int16_t>(reading.temperature * 100.0f);
    uint16_t hum_fixed = static_cast<uint16_t>(reading.humidity * 100.0f);

    // Store reading in memory (also used as direct transmit fallback when flash unavailable)
    current_reading_ = {.timestamp = unix_timestamp,
                        .temperature = temp_fixed,
                        .humidity = hum_fixed,
                        .flags = 0,
                        .reserved = 0,
                        .crc16 = 0};

    // Write to flash (no immediate TX - batch transmission handles delivery)
    // CRC will be calculated by writeRecord()
    if (flash_buffer_) {
        if (!flash_buffer_->writeRecord(current_reading_)) {
            logger.error("Failed to write record to flash!");
        } else {
            logger.debug("Stored sensor data to flash (temp=%d, hum=%d)", temp_fixed, hum_fixed);
        }
    }
}

HeartbeatStatus SensorMode::collectHeartbeatStatus()
{
    // Calculate uptime from initial boot timestamp (persisted across sleep cycles)
    uint32_t uptime = bramble::util::time::uptimeSeconds(
        flash_buffer_ ? flash_buffer_->getInitialBootTimestamp() : 0, getUnixTimestamp());

    uint8_t battery_level = getBatteryLevel();
    // RSSI is negative dBm (e.g., -70 dBm); convert to absolute value for payload
    int rssi = lora_.getRssi();
    uint8_t signal_strength = (rssi < 0 && rssi >= -120) ? static_cast<uint8_t>(-rssi) : 0;
    uint8_t active_sensors = CAP_TEMPERATURE | CAP_HUMIDITY;
    uint16_t error_flags = collectErrorFlags();

    // Get count of untransmitted records in flash backlog
    uint16_t pending_records = 0;
    if (flash_buffer_) {
        uint32_t count = flash_buffer_->getUntransmittedCount();
        // Clamp to uint16_t max (65535)
        pending_records = (count > 0xFFFF) ? 0xFFFF : static_cast<uint16_t>(count);
    }

    return {uptime, battery_level, signal_strength, active_sensors, error_flags, pending_records};
}

uint16_t SensorMode::collectErrorFlags()
{
    uint16_t flags = ERR_FLAG_NONE;

    // Check sensor health via state machine
    if (sensor_state_.isDegraded()) {
        flags |= ERR_FLAG_SENSOR_FAILURE;
    } else if (sensor_state_.hasSensor() && !last_sensor_read_valid_) {
        // Sensor initialized but last read failed
        flags |= ERR_FLAG_SENSOR_FAILURE;
    }

    // Check flash status - include health check for write failures
    if (!external_flash_ || !flash_buffer_ || !flash_buffer_->isHealthy()) {
        flags |= ERR_FLAG_FLASH_FAILURE;
    } else {
        SensorFlashMetadata stats;
        if (flash_buffer_->getStatistics(stats)) {
            uint32_t capacity = SensorFlashBuffer::MAX_RECORDS;
            uint32_t usage = stats.total_records;
            if (capacity > 0 && (usage * 100 / capacity) >= 90) {
                flags |= ERR_FLAG_FLASH_FULL;
            }
        }
    }

    // Check PMU availability
    if (!pmu_manager_ || !pmu_manager_->isAvailable()) {
        flags |= ERR_FLAG_PMU_FAILURE;
    }

    // Only flag RTC error if sync failed, not during normal boot sequence
    // (AWAITING_TIME/SYNCING_TIME are expected states, not errors)
    if (!sensor_state_.isTimeSynced() && !sensor_state_.isAwaitingTime()) {
        flags |= ERR_FLAG_RTC_NOT_SYNCED;
    }

    // Check battery level for warnings
    uint8_t battery = getBatteryLevel();
    if (battery != 255) {  // 255 = external power
        if (battery < 10) {
            flags |= ERR_FLAG_BATTERY_CRITICAL;
        } else if (battery < 20) {
            flags |= ERR_FLAG_BATTERY_LOW;
        }
    }

    // Check consecutive transmission failures
    if (transmitter_ && transmitter_->consecutiveFailures() >= TX_FAILURE_THRESHOLD) {
        flags |= ERR_FLAG_TX_FAILURES;
    }

    // Check network timeout rate from statistics
    if (network_stats_) {
        const auto &global = network_stats_->getGlobalStats();
        uint32_t total_reliable =
            global.criticality_totals[RELIABLE].sent + global.criticality_totals[CRITICAL].sent;
        uint32_t total_timeouts = global.criticality_totals[RELIABLE].timeouts +
                                  global.criticality_totals[CRITICAL].timeouts;

        // Flag if >25% of reliable/critical messages are timing out (with minimum sample size)
        if (total_reliable >= 10 && total_timeouts * 4 > total_reliable) {
            flags |= ERR_FLAG_HIGH_TIMEOUTS;
        }
    }

    return flags;
}

uint8_t SensorMode::getBatteryLevel()
{
    // TODO: Query PMU for actual battery level when battery monitoring is implemented
    // For now, return 255 (external power) since current hardware doesn't have battery
    return 255;
}

bool SensorMode::isTimeToTransmit(uint32_t current_timestamp) const
{
    if (!flash_buffer_) {
        return false;
    }

    // Use provided timestamp or fall back to RTC
    uint32_t now = (current_timestamp != 0) ? current_timestamp : getUnixTimestamp();

    // No valid timestamp available - need hub sync
    if (now == 0) {
        return true;
    }

    SensorFlashMetadata stats;
    uint32_t last_sync = 0;
    if (flash_buffer_->getStatistics(stats)) {
        last_sync = stats.last_sync_timestamp;
    }

    // First boot (no last_sync) - need hub sync for initial time
    if (last_sync == 0) {
        return true;
    }

    uint32_t elapsed = now - last_sync;
    return elapsed >= TRANSMIT_INTERVAL_S;
}

bool SensorMode::checkNeedsTransmission()
{
    // Direct transmit fallback: if flash unavailable or unhealthy but we have a valid reading
    bool flash_available = flash_buffer_ && flash_buffer_->isHealthy();
    if (!flash_available) {
        if (current_reading_.timestamp > 0 && last_sensor_read_valid_) {
            logger.info("Flash unavailable/unhealthy - will transmit current reading directly");
            return true;
        }
        logger.error("Flash unavailable/unhealthy and no valid reading - nothing to transmit");
        return false;
    }

    // Compute elapsed time since last sync (used for logging in both branches)
    uint32_t now = getUnixTimestamp();
    SensorFlashMetadata stats;
    uint32_t last_sync = 0;
    if (flash_buffer_->getStatistics(stats)) {
        last_sync = stats.last_sync_timestamp;
    }
    uint32_t elapsed = (last_sync > 0) ? (now - last_sync) : 0;

    // Check if it's time to transmit (based on TRANSMIT_INTERVAL_S)
    // This ensures ALL transmission paths respect the 10-minute interval
    if (!isTimeToTransmit()) {
        logger.info("Not time to transmit yet (%lu s / %lu s)", elapsed, TRANSMIT_INTERVAL_S);
        return false;
    }

    logger.info("Transmit interval reached (%lu s) - checking backlog", elapsed);

    uint32_t untransmitted_count = flash_buffer_->getUntransmittedCount();
    if (untransmitted_count == 0) {
        logger.debug("No backlog to transmit");
        // Update last sync timestamp - nothing to send counts as success
        flash_buffer_->updateLastSync(getUnixTimestamp());
        return false;
    }

    logger.info("Backlog check: %lu untransmitted records - transmission needed",
                untransmitted_count);
    return true;
}

void SensorMode::transmitCurrentReading()
{
    // Validate we have a reading to transmit
    if (current_reading_.timestamp == 0 || !last_sensor_read_valid_) {
        logger.error("No valid current reading to transmit");
        sensor_state_.reportTransmitComplete();
        return;
    }

    logger.info("Direct transmit: %.2fC (ts=%lu)", current_reading_.temperature / 100.0f,
                current_reading_.timestamp);

    if (!transmitter_->transmit(&current_reading_, 1, [this](bool success) {
            if (success) {
                current_reading_.timestamp = 0;  // Clear to avoid retransmit
            }
            sensor_state_.reportTransmitComplete();
        })) {
        logger.error("Failed to send direct transmit message");
        sensor_state_.reportTransmitComplete();
    }
}

void SensorMode::transmitBacklog()
{
    if (!flash_buffer_) {
        logger.error("No flash buffer - cannot transmit");
        sensor_state_.reportTransmitComplete();
        return;
    }

    // Read up to BATCH_SIZE records
    SensorDataRecord records[SensorFlashBuffer::BATCH_SIZE];
    size_t actual_count = 0;
    size_t records_scanned = 0;

    if (!flash_buffer_->readUntransmittedRecords(records, SensorFlashBuffer::BATCH_SIZE,
                                                 actual_count, records_scanned)) {
        logger.error("Failed to read untransmitted records");
        sensor_state_.reportTransmitComplete();
        return;
    }

    uint32_t untransmitted_count = flash_buffer_->getUntransmittedCount();
    if (actual_count == 0) {
        if (records_scanned > 0) {
            // All scanned records had CRC errors - skip only those we verified as corrupt
            logger.warn("No valid records found - skipping %zu corrupt records (of %lu pending)",
                        records_scanned, untransmitted_count);
            flash_buffer_->advanceReadIndex(static_cast<uint32_t>(records_scanned));
        } else if (untransmitted_count > 0) {
            // Couldn't scan any records (read failure) - don't skip, try again later
            logger.warn("Flash read issues - %lu records pending, will retry later",
                        untransmitted_count);
        }
        sensor_state_.reportTransmitComplete();
        return;
    }

    // Get the start index for tracking which records are in this batch
    SensorFlashMetadata stats;
    flash_buffer_->getStatistics(stats);
    uint32_t start_index = stats.read_index;

    logger.info("Transmitting batch of %zu records", actual_count);

    if (!transmitter_->transmit(
            records, actual_count,
            [this, actual_count](bool success) {
                if (success && flash_buffer_) {
                    flash_buffer_->advanceReadIndex(static_cast<uint32_t>(actual_count));
                    flash_buffer_->updateLastSync(getUnixTimestamp());

                    // Check if we should send another batch
                    uint32_t remaining = flash_buffer_->getUntransmittedCount();
                    if (remaining > 0 && transmitter_->canSendMore()) {
                        logger.info("More backlog (%lu records) - sending next batch", remaining);
                        transmitBacklog();
                        return;  // Don't report complete yet
                    } else if (remaining == 0) {
                        logger.info("All backlog transmitted");
                    } else {
                        logger.info("Batch limit reached, %lu records remaining", remaining);
                    }
                }
                sensor_state_.reportTransmitComplete();
            },
            start_index)) {
        logger.warn("Batch transmission failed to initiate");
        sensor_state_.reportTransmitComplete();
    }
}

void SensorMode::onHeartbeatResponse(const HeartbeatResponsePayload *payload)
{
    // Call base class implementation to update RP2040 RTC
    ApplicationMode::onHeartbeatResponse(payload);

    // Log pending flags (sensor mode doesn't handle updates yet)
    if (payload && payload->pending_update_flags != PENDING_FLAG_NONE) {
        logger.info("Pending update flags: 0x%02X (not handled in sensor mode)",
                    payload->pending_update_flags);
    }

    // Delegate to heartbeat client — ResponseCallback handles PMU sync + state machine
    heartbeat_client_->handleResponse(payload);
}

void SensorMode::onRebootRequested()
{
    if (pmu_manager_) {
        pmu_manager_->requestSystemReset();
    } else {
        logger.warn("PMU manager not available - performing RP2040-only watchdog reboot");
        watchdog_reboot(0, 0, 0);
    }
}

void SensorMode::initializeFlashTimestamps()
{
    // Initialize timestamps on first boot after power loss
    if (!flash_buffer_) {
        return;
    }

    uint32_t now = getUnixTimestamp();

    // Reset boot timestamp on STM32 power cycle (PMU lost its clock, had to sync from hub).
    // This makes uptime reflect "time since last full power loss" rather than "time since
    // first deployment." The SYNCING_TIME previous state means PMU had no valid time.
    if (sensor_state_.previousState() == SensorState::SYNCING_TIME) {
        flash_buffer_->setInitialBootTimestamp(now);
        logger.info("Power cycle detected - reset initial_boot_timestamp to %lu", now);
    } else if (flash_buffer_->getInitialBootTimestamp() == 0) {
        flash_buffer_->setInitialBootTimestamp(now);
        logger.info("Set initial_boot_timestamp to %lu", now);
    }

    // Initialize last_sync_timestamp if this is first boot
    // This gives a valid baseline for the transmission interval check
    SensorFlashMetadata stats;
    if (flash_buffer_->getStatistics(stats) && stats.last_sync_timestamp == 0) {
        flash_buffer_->updateLastSync(now);
        logger.info("Initialized last_sync_timestamp to %lu", now);
    }
}

bool SensorMode::tryInitSensor()
{
    // Already working - no need to re-init
    if (sensor_state_.hasSensor()) {
        return true;
    }

    if (!sensor_) {
        return false;
    }

    logger.debug("Waiting for sensor power-on stabilization...");
    sleep_ms(2);  // Datasheet specifies 1ms max power-up time

    if (sensor_->init()) {
        // Report success to state machine (transitions to OPERATIONAL)
        sensor_state_.reportSensorInitSuccess();
        logger.debug("CHT832X sensor initialized on I2C1 (SDA=%d, SCL=%d)", PIN_I2C_SDA,
                     PIN_I2C_SCL);

        return true;
    }

    // Report failure to state machine (transitions to DEGRADED_NO_SENSOR)
    sensor_state_.reportSensorInitFailure();
    logger.error("Failed to initialize CHT832X sensor!");
    logger.error("Check wiring: Red=3.3V, Black=GND, Green=GPIO%d, Yellow=GPIO%d", PIN_I2C_SCL,
                 PIN_I2C_SDA);
    return false;
}

void SensorMode::requestTimeSync()
{
    if (pmu_manager_ && pmu_manager_->isAvailable()) {
        // Try PMU's battery-backed RTC first
        pmu_manager_->requestTime([this](bool valid, const PMU::DateTime &datetime) {
            if (valid) {
                pmu_logger.debug("PMU has valid time: 20%02d-%02d-%02d %02d:%02d:%02d",
                                 datetime.year, datetime.month, datetime.day, datetime.hour,
                                 datetime.minute, datetime.second);

                // Check if it's time to transmit BEFORE setting RTC
                bool time_to_transmit =
                    isTimeToTransmit(bramble::util::time::toUnixTimestamp(datetime));

                // Set RP2040 RTC from PMU time
                datetime_t dt = bramble::util::time::toDatetimeT(datetime);

                if (rtc_set_datetime(&dt)) {
                    // The RTC on the RP2040 runs on a separate clock domain
                    // (typically derived from a 32kHz source). When you call
                    // rtc_set_datetime(), the write to the RTC registers needs
                    // to cross clock domains and propagate. Sync our subsecond
                    // counter for logging purposes here to align the start of a second
                    sleep_us(64);
                    Logger::syncSubsecondCounter();

                    Logger("SensorSM")
                        .info("RTC set from PMU: %04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month,
                              dt.day, dt.hour, dt.min, dt.sec);

                    // Report RTC sync - state callback handles LED pattern change
                    sensor_state_.reportRtcSynced();
                    if (flash_buffer_ && flash_buffer_->getInitialBootTimestamp() == 0) {
                        uint32_t now = getUnixTimestamp();
                        flash_buffer_->setInitialBootTimestamp(now);
                        logger.info("Set initial_boot_timestamp to %lu (from PMU sync)", now);
                    }

                    // Only sync from hub if it's time to transmit
                    if (time_to_transmit) {
                        pmu_logger.info("Syncing time by sending heartbeat.");
                        heartbeat_client_->send();
                        sensor_state_.expectResponse();
                    } else {
                        pmu_logger.info("Skip hub sync: not time to transmit yet.");
                    }
                } else {
                    logger.error("Failed to set RTC from PMU time");
                }
            } else {
                // PMU doesn't have valid time - need to sync from hub
                pmu_logger.info("PMU time not valid - sending heartbeat for hub sync");
                heartbeat_client_->send();
                sensor_state_.expectResponse();
                sensor_state_.reportHeartbeatSent();
            }
        });
    } else {
        // No PMU - send heartbeat to sync from hub
        logger.info("No PMU - sending heartbeat for time sync");
        heartbeat_client_->send();
        sensor_state_.expectResponse();
        sensor_state_.reportHeartbeatSent();
    }
}

void SensorMode::attemptRegistration()
{
    uint64_t device_id = getDeviceId();

    pmu_logger.info("Sending registration (device_id=0x%016llX)", device_id);

    uint8_t seq = messenger_.sendRegistrationRequest(ADDRESS_HUB, device_id, NODE_TYPE_SENSOR,
                                                     CAP_TEMPERATURE | CAP_HUMIDITY,
                                                     BRAMBLE_FIRMWARE_VERSION, "Sensor Node");

    if (seq != 0) {
        pmu_logger.info("Registration request sent (seq=%d)", seq);
        // Start registration timeout — fires if hub never responds
        uint32_t now = to_ms_since_boot(get_absolute_time());
        registration_timeout_id_ = task_queue_.postDelayed(
            [](void *ctx, uint32_t) -> bool {
                SensorMode *self = static_cast<SensorMode *>(ctx);
                pmu_logger.warn("Registration timeout - will retry on next wake");
                self->registration_timeout_id_ = 0;
                self->sensor_state_.reportRegistrationTimeout();
                return true;
            },
            this, now, REGISTRATION_TIMEOUT_MS, TaskPriority::High);
        sensor_state_.reportRegistrationSent();
        // Expect registration response - ensures we enter LISTENING before sleep
        sensor_state_.expectResponse();
    } else {
        pmu_logger.error("Failed to send registration request");
        // Failed to send - go to sleep and retry next cycle
        sensor_state_.reportRegistrationTimeout();
    }
}
