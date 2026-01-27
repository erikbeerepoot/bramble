#include "sensor_mode.h"

#include <ctime>

#include "pico/stdlib.h"

#include "hardware/i2c.h"

#include "../hal/cht832x.h"
#include "../hal/logger.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/network_stats.h"
#include "../lora/reliable_messenger.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;

// PMU UART configuration - adjust pins based on your hardware
#define PMU_UART_ID uart0
#define PMU_UART_TX_PIN 0
#define PMU_UART_RX_PIN 1

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

            // Restore persisted LoRa sequence number across sleep/wake cycles
            uint8_t saved_seq = flash_buffer_->getNextSeqNum();
            if (saved_seq >= 128) {
                messenger_.setNextSeqNum(saved_seq);
                logger.debug("Restored LoRa seq num: %u", saved_seq);
            }
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
    sensor_state_.setCallback([this](SensorState state) {
        onStateChange(state);
    });

    // Initialize PMU client at 9600 baud to match STM32 LPUART configuration
    pmu_client_ = new PmuClient(PMU_UART_ID, PMU_UART_TX_PIN, PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    // Create reliable PMU client wrapper for automatic retry
    if (pmu_available_) {
        reliable_pmu_ = new PMU::ReliablePmuClient(pmu_client_);
        reliable_pmu_->init();
    }

    // Note: Task queue is used to coordinate RTC sync -> backlog -> sleep flow
    // Tasks are posted dynamically as work becomes available

    if (pmu_available_ && reliable_pmu_) {
        pmu_logger.info("PMU client initialized successfully");

        // Set up PMU callback handler
        reliable_pmu_->onWake([this](PMU::WakeReason reason, const PMU::ScheduleEntry *entry) {
            this->handlePmuWake(reason, entry);
        });

        // Try to get time from PMU's battery-backed RTC (faster than waiting for hub sync)
        pmu_logger.debug("Requesting datetime from PMU...");
        reliable_pmu_->getDateTime([this](bool valid, const PMU::DateTime &datetime) {
            if (valid) {
                pmu_logger.debug("PMU has valid time: 20%02d-%02d-%02d %02d:%02d:%02d",
                                 datetime.year, datetime.month, datetime.day, datetime.hour,
                                 datetime.minute, datetime.second);

                // Convert PMU datetime to Unix timestamp using mktime
                uint16_t year = 2000 + datetime.year;
                std::tm tm = {};
                tm.tm_year = year - 1900;        // years since 1900
                tm.tm_mon = datetime.month - 1;  // 0-based month
                tm.tm_mday = datetime.day;
                tm.tm_hour = datetime.hour;
                tm.tm_min = datetime.minute;
                tm.tm_sec = datetime.second;
                uint32_t pmu_timestamp = static_cast<uint32_t>(std::mktime(&tm));

                // Check if it's time to transmit BEFORE setting RTC
                bool time_to_transmit = isTimeToTransmit(pmu_timestamp);

                // Set RP2040 RTC from PMU time
                datetime_t dt;
                dt.year = year;
                dt.month = datetime.month;
                dt.day = datetime.day;
                dt.dotw = datetime.weekday;
                dt.hour = datetime.hour;
                dt.min = datetime.minute;
                dt.sec = datetime.second;

                if (rtc_set_datetime(&dt)) {
                    sleep_us(64);  // Wait for RTC to propagate
                    Logger("SensorSM").info("RTC set from PMU: %04d-%02d-%02d %02d:%02d:%02d", dt.year,
                                            dt.month, dt.day, dt.hour, dt.min, dt.sec);

                    // Report RTC sync - state callback handles LED pattern change
                    sensor_state_.reportRtcSynced();
                    if (flash_buffer_ && flash_buffer_->getInitialBootTimestamp() == 0) {
                        uint32_t now = getUnixTimestamp();
                        flash_buffer_->setInitialBootTimestamp(now);
                        logger.info("Set initial_boot_timestamp to %lu (from PMU sync)", now);
                    }

                    // Only sync from hub if it's time to transmit
                    // This avoids unnecessary LoRa traffic between transmission intervals
                    if (time_to_transmit) {
                        pmu_logger.info("Time to transmit - sending heartbeat for hub sync");
                        heartbeat_request_time_ = to_ms_since_boot(get_absolute_time());
                        sendHeartbeat(0);
                        // Note: state is already TIME_SYNCED from reportRtcSynced() above,
                        // so reportHeartbeatSent() would be rejected (wrong source state)
                    } else {
                        // PMU time is good enough, proceed directly
                        pmu_logger.info("Not time to transmit - using PMU time, skipping hub sync");
                        // reportRtcSynced will trigger TIME_SYNCED -> onStateChange handles the rest
                    }
                } else {
                    logger.error("Failed to set RTC from PMU time");
                }
            } else {
                // PMU doesn't have valid time - need to sync from hub
                pmu_logger.info(
                    "PMU time not valid (first boot?) - sending heartbeat for hub sync");
                heartbeat_request_time_ = to_ms_since_boot(get_absolute_time());
                sendHeartbeat(0);
                sensor_state_.reportHeartbeatSent();
            }
        });
    } else {
        logger.warn("PMU client not available - sending heartbeat for time sync");
        heartbeat_request_time_ = to_ms_since_boot(get_absolute_time());
        sendHeartbeat(0);
        sensor_state_.reportHeartbeatSent();
    }

    // Note: Periodic tasks are no longer used - state machine drives all work.
    // The wake cycle flow (TIME_SYNCED -> READING_SENSOR -> CHECKING_BACKLOG ->
    // TRANSMITTING -> READY_FOR_SLEEP) handles sensor reads and transmissions.
    // Heartbeats are sent on each wake cycle as part of time sync.

    // Mark sensor state machine as initialized (transitions to AWAITING_TIME)
    sensor_state_.markInitialized();
}

void SensorMode::onStateChange(SensorState state)
{
    // Centralized task scheduler - reacts to state changes by posting work
    switch (state) {
        case SensorState::AWAITING_TIME:
            // Schedule heartbeat for time sync (if PMU doesn't have time)
            // Note: PMU time check happens in onStart, this is the fallback path
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
                sendHeartbeat(to_ms_since_boot(get_absolute_time()));
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
                    bool needsTx = self->checkBacklog();
                    self->sensor_state_.reportCheckComplete(needsTx);
                    return true;
                },
                this, TaskPriority::High);
            break;

        case SensorState::TRANSMITTING:
            // Transmit backlog
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    self->transmitBacklog();
                    // Note: reportTransmitComplete called in transmitBatch callback
                    return true;
                },
                this, TaskPriority::High);
            break;

        case SensorState::LISTENING:
            // Receive window open - stay awake briefly for hub responses
            listen_window_start_time_ = to_ms_since_boot(get_absolute_time());
            logger.info("Receive window open (%lu ms)", LISTEN_WINDOW_MS);
            break;

        case SensorState::READY_FOR_SLEEP:
            // All work done - signal PMU
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    self->signalReadyForSleep();
                    return true;
                },
                this, TaskPriority::Low);
            break;

        case SensorState::DEGRADED_NO_SENSOR:
            // Sensor failed - orange short blink, skip to backlog check
            led_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 255, 165, 0);
            task_queue_.postOnce(
                [](void *ctx, uint32_t time) -> bool {
                    (void)time;
                    SensorMode *self = static_cast<SensorMode *>(ctx);
                    bool needsTx = self->checkBacklog();
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
    }
}


void SensorMode::onLoop()
{
    // Process any pending PMU messages and handle retries
    if (pmu_available_ && reliable_pmu_) {
        reliable_pmu_->update();
    }

    // Hub sync timeout - check if PMU already set RTC and we can proceed
    if (!sensor_state_.isTimeSynced() && heartbeat_request_time_ > 0) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - heartbeat_request_time_;
        if (elapsed >= HEARTBEAT_TIMEOUT_MS) {
            logger.warn("Hub sync timeout (%lu ms) - checking if RTC is running", elapsed);
            heartbeat_request_time_ = 0;
            // Check hardware directly - if RTC is running, PMU must have set it
            if (rtc_running()) {
                // reportRtcSynced triggers TIME_SYNCED -> onStateChange handles the rest
                sensor_state_.reportRtcSynced();
            }
        }
    }

    // Listen window timeout - close receive window and proceed to sleep
    if (sensor_state_.isListening() && listen_window_start_time_ > 0) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - listen_window_start_time_;
        if (elapsed >= LISTEN_WINDOW_MS) {
            logger.info("Receive window closed after %lu ms", elapsed);
            listen_window_start_time_ = 0;
            sensor_state_.reportListenComplete();
        }
    }

    // Process any pending tasks (deferred sleep signals, etc.)
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    task_queue_.process(current_time);
}

void SensorMode::readAndStoreSensorData(uint32_t current_time)
{
    (void)current_time;  // No longer used - we use RTC Unix timestamp

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

    auto reading = sensor_->read();

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

    // Write to flash only (no immediate TX - batch transmission handles delivery)
    if (flash_buffer_) {
        SensorDataRecord record = {
            .timestamp = unix_timestamp,
            .temperature = temp_fixed,
            .humidity = hum_fixed,
            .flags = 0,  // Not transmitted yet
            .reserved = 0,
            .crc16 = 0  // Will be calculated by writeRecord()
        };

        if (!flash_buffer_->writeRecord(record)) {
            logger.error("Failed to write record to flash!");
        } else {
            logger.debug("Stored sensor data to flash (temp=%d, hum=%d)", temp_fixed, hum_fixed);
        }
    }
}

void SensorMode::sendHeartbeat(uint32_t /* current_time */)
{
    // Calculate uptime from initial boot timestamp (persisted across sleep cycles)
    uint32_t uptime = 0;
    uint32_t initial_boot = flash_buffer_ ? flash_buffer_->getInitialBootTimestamp() : 0;
    uint32_t current_time_rtc = getUnixTimestamp();
    if (initial_boot > 0 && current_time_rtc > initial_boot) {
        uptime = current_time_rtc - initial_boot;
    }
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

    logger.debug("Sending heartbeat (uptime=%lu s, battery=%u, errors=0x%04X, pending=%u)", uptime,
                 battery_level, error_flags, pending_records);

    messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength, active_sensors,
                             error_flags, pending_records);

    // Heartbeat expects a HEARTBEAT_RESPONSE from the hub
    sensor_state_.expectResponse();
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

    // Check flash status
    if (!external_flash_ || !flash_buffer_) {
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
    if (!pmu_available_) {
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
    if (consecutive_tx_failures_ >= TX_FAILURE_THRESHOLD) {
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

bool SensorMode::checkBacklog()
{
    if (!flash_buffer_) {
        logger.debug("No flash buffer - no backlog to check");
        return false;
    }

    // Check if it's time to transmit (based on TRANSMIT_INTERVAL_S)
    // This ensures ALL transmission paths respect the 10-minute interval
    if (!isTimeToTransmit()) {
        uint32_t now = getUnixTimestamp();
        SensorFlashMetadata stats;
        uint32_t last_sync = 0;
        if (flash_buffer_->getStatistics(stats)) {
            last_sync = stats.last_sync_timestamp;
        }
        uint32_t elapsed = now - last_sync;
        logger.info("Not time to transmit yet (%lu s / %lu s)", elapsed, TRANSMIT_INTERVAL_S);
        return false;
    }

    uint32_t now = getUnixTimestamp();
    SensorFlashMetadata stats;
    uint32_t last_sync = 0;
    if (flash_buffer_->getStatistics(stats)) {
        last_sync = stats.last_sync_timestamp;
    }
    uint32_t elapsed = now - last_sync;
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

    if (!flash_buffer_->readUntransmittedRecords(records, SensorFlashBuffer::BATCH_SIZE,
                                                 actual_count)) {
        logger.error("Failed to read untransmitted records");
        sensor_state_.reportTransmitComplete();
        return;
    }

    uint32_t untransmitted_count = flash_buffer_->getUntransmittedCount();
    if (actual_count == 0) {
        logger.warn("No valid records to transmit - skipping %lu corrupt records",
                    untransmitted_count);
        // All remaining records are corrupt - advance past them
        flash_buffer_->advanceReadIndex(untransmitted_count);
        sensor_state_.reportTransmitComplete();
        return;
    }

    logger.info("Transmitting batch of %zu records", actual_count);

    // Transmit the batch - reportTransmitComplete called in callback
    if (transmitBatch(records, actual_count)) {
        logger.info("Batch transmission initiated");
        // Note: updateLastSync moved to ACK callback in transmitBatch to avoid
        // resetting the 10-minute timer before delivery is confirmed
    } else {
        logger.warn("Batch transmission failed to initiate");
        sensor_state_.reportTransmitComplete();
    }
}

bool SensorMode::transmitBatch(const SensorDataRecord *records, size_t count)
{
    if (!records || count == 0 || !flash_buffer_) {
        return false;
    }

    // Get the start index for tracking which records are in this batch
    SensorFlashMetadata stats;
    flash_buffer_->getStatistics(stats);
    uint32_t start_index = stats.read_index;

    // Convert SensorDataRecord to BatchSensorRecord format
    BatchSensorRecord batch_records[MAX_BATCH_RECORDS];
    for (size_t i = 0; i < count && i < MAX_BATCH_RECORDS; i++) {
        batch_records[i].timestamp = records[i].timestamp;
        batch_records[i].temperature = records[i].temperature;
        batch_records[i].humidity = records[i].humidity;
        batch_records[i].flags = records[i].flags;
        batch_records[i].reserved = records[i].reserved;
        batch_records[i].crc16 = records[i].crc16;
    }

    // Send batch with callback to advance read index on ACK
    // Note: We don't mark individual records as transmitted - timestamps allow
    // deduplication during USB recovery if needed. This avoids flash wear.
    uint8_t seq = messenger_.sendSensorDataBatchWithCallback(
        HUB_ADDRESS, start_index, batch_records, static_cast<uint8_t>(count), RELIABLE,
        [this, count](uint8_t seq_num, uint8_t ack_status, uint64_t context) {
            if (ack_status == 0 && flash_buffer_) {
                // ACK received - advance read index past transmitted records
                if (flash_buffer_->advanceReadIndex(static_cast<uint32_t>(count))) {
                    logger.info("Batch ACK received (seq=%d): %zu records transmitted", seq_num,
                                count);
                    consecutive_tx_failures_ = 0;  // Reset failure counter on success
                    flash_buffer_->updateLastSync(getUnixTimestamp());

                    // Check if we've cleared all backlog
                    if (flash_buffer_->getUntransmittedCount() == 0) {
                        logger.info("All backlog transmitted");
                    }
                }
            } else {
                logger.warn("Batch transmission failed (seq=%d, status=%d), will retry", seq_num,
                            ack_status);
                if (consecutive_tx_failures_ < 255) {
                    consecutive_tx_failures_++;
                }
            }
            // Report completion to state machine (triggers READY_FOR_SLEEP)
            sensor_state_.reportTransmitComplete();
        },
        start_index  // Pass start index as user_context
    );

    if (seq == 0) {
        logger.error("Failed to send batch message");
        return false;
    }

    logger.debug("Sent batch of %zu records (start_index=%lu, seq=%d)", count, start_index, seq);
    return true;
}

void SensorMode::signalReadyForSleep()
{
    if (!pmu_available_ || !reliable_pmu_) {
        logger.debug("PMU not available, skipping ready for sleep signal");
        return;
    }

    // Save LoRa sequence number and flush flash buffer metadata before power down
    // Without this, write_index isn't persisted and records get corrupted on next boot
    if (flash_buffer_) {
        flash_buffer_->saveNextSeqNum(messenger_.getNextSeqNum());
        flash_buffer_->flush();
    }

    // Post deferred task to send sleep signal in main loop
    // This avoids calling PMU protocol directly from callback chains (stack safety)
    // Using postOnce() to deduplicate - multiple code paths call this but we only
    // need one ReadyForSleep command per wake cycle
    pmu_logger.debug("Requesting sleep (posting deferred task)");

    // Pass this pointer as context so we can access reliable_pmu_
    task_queue_.postOnce(
        [](void *ctx, uint32_t time) -> bool {
            (void)time;
            SensorMode *self = static_cast<SensorMode *>(ctx);

            pmu_logger.info("Sending ReadyForSleep via reliable client");
            self->reliable_pmu_->readyForSleep([](bool success, PMU::ErrorCode error) {
                if (success) {
                    pmu_logger.info("Ready for sleep acknowledged");
                } else {
                    pmu_logger.error("Ready for sleep failed: error %d", static_cast<int>(error));
                }
            });
            return true;  // Task complete
        },
        this, TaskPriority::Low);
}

void SensorMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry)
{
    (void)entry;

    switch (reason) {
        case PMU::WakeReason::Periodic:
            pmu_logger.info("Periodic wake - restarting sensor cycle");
            // Report wake to state machine - it handles the appropriate transition
            if (!sensor_state_.reportWakeFromSleep()) {
                // Need time sync first
                pmu_logger.warn("RTC not synced on periodic wake - requesting time");
                sendHeartbeat(0);
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
}

void SensorMode::onRtcSynced()
{
    sensor_state_.reportRtcSynced();
}

void SensorMode::onHeartbeatResponse(const HeartbeatResponsePayload *payload)
{
    // Base class sets RP2040 RTC and calls onRtcSynced() → sensor_state_.reportRtcSynced()
    ApplicationMode::onHeartbeatResponse(payload);

    // Also sync time to PMU for persistence across power cycles
    if (pmu_available_ && reliable_pmu_ && payload) {
        PMU::DateTime datetime(payload->year % 100,  // PMU uses 2-digit year (e.g., 26 for 2026)
                               payload->month, payload->day, payload->dotw, payload->hour,
                               payload->min, payload->sec);

        pmu_logger.info("Syncing time to PMU: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

        reliable_pmu_->setDateTime(datetime, [](bool success, PMU::ErrorCode error) {
            if (success) {
                pmu_logger.info("PMU time sync successful");
            } else {
                pmu_logger.error("PMU time sync failed: error %d", static_cast<int>(error));
            }
        });
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

    logger.debug("Waiting 1s for sensor power-on stabilization...");
    sleep_ms(1000);

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