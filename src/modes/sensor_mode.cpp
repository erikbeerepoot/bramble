#include "sensor_mode.h"
#include "../hal/cht832x.h"
#include "../hal/logger.h"
#include "../lora/reliable_messenger.h"
#include "../lora/message.h"
#include "../led_patterns.h"
#include "hardware/i2c.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;

// PMU UART configuration - adjust pins based on your hardware
#define PMU_UART_ID uart0
#define PMU_UART_TX_PIN 0
#define PMU_UART_RX_PIN 1

static Logger logger("SENSOR");
static Logger pmu_logger("PMU");

SensorMode::~SensorMode() = default;

void SensorMode::onStart() {
    logger.info("=== SENSOR MODE ACTIVE ===");
    logger.info("- Temperature/humidity data logger");
    logger.info("- 30 second reading interval");
    logger.info("- Orange LED blink (init) -> Red short blink (operational)");

    // Initialize external flash for sensor data storage
    // Flash shares SPI1 with LoRa (MISO=GPIO8, SCK=GPIO14, MOSI=GPIO15)
    // Flash has its own CS (GPIO6) and RESET (GPIO7)
    external_flash_ = std::make_unique<ExternalFlash>();
    if (external_flash_->init()) {
        logger.info("External flash initialized");

        // Initialize flash buffer
        flash_buffer_ = std::make_unique<SensorFlashBuffer>(*external_flash_);
        if (flash_buffer_->init()) {
            SensorFlashMetadata stats;
            flash_buffer_->getStatistics(stats);
            logger.info("Flash buffer initialized: %lu records (%lu untransmitted)",
                       stats.total_records,
                       flash_buffer_->getUntransmittedCount());
        } else {
            logger.error("Failed to initialize flash buffer!");
        }
    } else {
        logger.error("Failed to initialize external flash!");
    }

    // Initialize CHT832X temperature/humidity sensor
    sensor_ = std::make_unique<CHT832X>(i2c1, PIN_I2C_SDA, PIN_I2C_SCL);

    if (sensor_->init()) {
        logger.info("CHT832X sensor initialized on I2C1 (SDA=%d, SCL=%d)",
                   PIN_I2C_SDA, PIN_I2C_SCL);

        // Take an initial reading to verify sensor is working
        auto reading = sensor_->read();
        if (reading.valid) {
            logger.info("Initial reading: %.2fC, %.2f%%RH",
                       reading.temperature, reading.humidity);
        }
    } else {
        logger.error("Failed to initialize CHT832X sensor!");
        logger.error("Check wiring: Red=3.3V, Black=GND, Green=GPIO%d, Yellow=GPIO%d",
                    PIN_I2C_SCL, PIN_I2C_SDA);
    }

    // Start with orange blinking pattern while waiting for RTC sync
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);
    // Store operational pattern (red short blink) for after RTC sync
    // Red single channel at full brightness for power efficiency
    operational_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 255, 0, 0);

    // Initialize PMU client at 9600 baud to match STM32 LPUART configuration
    pmu_client_ = new PmuClient(PMU_UART_ID, PMU_UART_TX_PIN, PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        pmu_logger.info("PMU client initialized successfully");

        // Set up PMU callback handler
        pmu_client_->getProtocol().onWakeNotification([this](PMU::WakeReason reason, const PMU::ScheduleEntry* entry) {
            this->handlePmuWake(reason, entry);
        });
    } else {
        logger.warn("PMU client not available - running without power management");
    }

    // Send initial heartbeat immediately to sync RTC before first sensor reading
    logger.info("Sending initial heartbeat for time sync...");
    sendHeartbeat(0);

    // Add periodic sensor reading task (stores to flash, no immediate TX)
    task_manager_.addTask(
        [this](uint32_t time) {
            readAndStoreSensorData(time);
        },
        SENSOR_READ_INTERVAL_MS,
        "Sensor Read"
    );

    // Add heartbeat task
    task_manager_.addTask(
        [this](uint32_t time) {
            sendHeartbeat(time);
        },
        HEARTBEAT_INTERVAL_MS,
        "Heartbeat"
    );

    // Add backlog transmission task
    task_manager_.addTask(
        [this](uint32_t time) {
            checkAndTransmitBacklog(time);
        },
        BACKLOG_TX_INTERVAL_MS,
        "Backlog Transmission"
    );
}

void SensorMode::onLoop() {
    // Process any pending PMU messages (fills flags, minimal work)
    if (pmu_available_ && pmu_client_) {
        pmu_client_->process();
    }

    // Handle deferred backlog check (triggered by PMU periodic wake)
    // Wait for RTC sync before processing - we need valid timestamps
    if (backlog_check_requested_ && isRtcSynced()) {
        backlog_check_requested_ = false;

        // Take a sensor reading now that RTC is synced
        pmu_logger.info("RTC synced - taking sensor reading");
        readAndStoreSensorData(to_ms_since_boot(get_absolute_time()));

        // Check if it's time to transmit (every 15 minutes)
        // Use last_sync_timestamp from flash metadata (persists across power cycles)
        uint32_t now = getUnixTimestamp();
        SensorFlashMetadata stats;
        uint32_t last_sync = 0;
        if (flash_buffer_ && flash_buffer_->getStatistics(stats)) {
            last_sync = stats.last_sync_timestamp;
        }
        uint32_t elapsed = now - last_sync;

        if (elapsed >= TRANSMIT_INTERVAL_S || last_sync == 0) {
            pmu_logger.info("Transmit interval reached (%lu s) - checking backlog", elapsed);
            checkAndTransmitBacklog(to_ms_since_boot(get_absolute_time()));
            // Note: last_sync_timestamp is updated in checkAndTransmitBacklog on success
        } else {
            pmu_logger.info("Not time to transmit yet (%lu s / %lu s) - going to sleep",
                          elapsed, TRANSMIT_INTERVAL_S);
            signalReadyForSleep();
        }
    }

    // Handle deferred sleep signal (outside callback chain for stack safety)
    if (sleep_requested_) {
        sleep_requested_ = false;
        if (pmu_available_ && pmu_client_) {
            // Small delay to ensure STM32 UART RX is ready after sending wake notification
            sleep_ms(100);
            pmu_logger.info("Signaling ready for sleep (deferred)");
            pmu_client_->getProtocol().readyForSleep([](bool success, PMU::ErrorCode error) {
                if (success) {
                    pmu_logger.info("Ready for sleep acknowledged");
                } else {
                    pmu_logger.error("Ready for sleep failed: error %d", static_cast<int>(error));
                }
            });
        }
    }
}

void SensorMode::readAndStoreSensorData(uint32_t current_time) {
    (void)current_time;  // No longer used - we use RTC Unix timestamp

    if (!sensor_) {
        logger.error("Sensor not initialized");
        return;
    }

    // Check if RTC has been synced - don't store readings with invalid timestamps
    if (!isRtcSynced()) {
        logger.warn("RTC not synced yet, skipping sensor storage");
        return;
    }

    auto reading = sensor_->read();

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

    logger.info("Sensor reading: %.2fC, %.2f%%RH (ts=%lu)", reading.temperature, reading.humidity, unix_timestamp);

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
            .crc16 = 0   // Will be calculated by writeRecord()
        };

        if (!flash_buffer_->writeRecord(record)) {
            logger.error("Failed to write record to flash!");
        } else {
            logger.info("Stored sensor data to flash (temp=%d, hum=%d)", temp_fixed, hum_fixed);
        }
    }
}

void SensorMode::sendHeartbeat(uint32_t current_time) {
    uint32_t uptime = current_time / 1000;  // Convert to seconds
    uint8_t battery_level = 255;            // External power (no battery monitoring yet)
    uint8_t signal_strength = 70;           // TODO: Get actual RSSI from LoRa
    uint8_t active_sensors = CAP_TEMPERATURE | CAP_HUMIDITY;
    uint8_t error_flags = 0;                // No errors

    logger.debug("Sending heartbeat (uptime=%lu s)", uptime);

    messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level,
                            signal_strength, active_sensors, error_flags);
}

void SensorMode::checkAndTransmitBacklog(uint32_t current_time) {
    if (!flash_buffer_) {
        return;
    }

    uint32_t untransmitted_count = flash_buffer_->getUntransmittedCount();

    if (untransmitted_count == 0) {
        logger.debug("No backlog to transmit");
        // Update last sync timestamp in flash - nothing to send counts as success
        flash_buffer_->updateLastSync(getUnixTimestamp());
        // Signal ready for sleep since there's no work to do
        signalReadyForSleep();
        return;
    }

    logger.info("Backlog check: %lu untransmitted records", untransmitted_count);

    // Read up to BATCH_SIZE records
    SensorDataRecord records[SensorFlashBuffer::BATCH_SIZE];
    size_t actual_count = 0;

    if (!flash_buffer_->readUntransmittedRecords(records, SensorFlashBuffer::BATCH_SIZE, actual_count)) {
        logger.error("Failed to read untransmitted records");
        return;
    }

    if (actual_count == 0) {
        logger.warn("No valid records to transmit - skipping %lu corrupt records", untransmitted_count);
        // All remaining records are corrupt - advance past them so we don't get stuck
        flash_buffer_->advanceReadIndex(static_cast<uint32_t>(untransmitted_count));
        signalReadyForSleep();
        return;
    }

    logger.info("Transmitting batch of %zu records", actual_count);

    // Transmit the batch
    if (transmitBatch(records, actual_count)) {
        logger.info("Batch transmission successful");
        // Update last sync timestamp in flash (persists across power cycles)
        flash_buffer_->updateLastSync(getUnixTimestamp());
    } else {
        logger.warn("Batch transmission failed, will retry later");
    }
}

bool SensorMode::transmitBatch(const SensorDataRecord* records, size_t count) {
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
        HUB_ADDRESS, start_index,
        batch_records, static_cast<uint8_t>(count), RELIABLE,
        [this, count](uint8_t seq_num, uint8_t ack_status, uint64_t context) {
            if (ack_status == 0 && flash_buffer_) {
                // ACK received - advance read index past transmitted records
                if (flash_buffer_->advanceReadIndex(static_cast<uint32_t>(count))) {
                    logger.info("Batch ACK received (seq=%d): %zu records transmitted",
                               seq_num, count);

                    // Check if we've cleared all backlog
                    if (flash_buffer_->getUntransmittedCount() == 0) {
                        logger.info("All backlog transmitted - ready for sleep");
                        signalReadyForSleep();
                    }
                }
            } else {
                logger.warn("Batch transmission failed (seq=%d, status=%d), will retry",
                           seq_num, ack_status);
            }
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

void SensorMode::signalReadyForSleep() {
    if (!pmu_available_ || !pmu_client_) {
        logger.debug("PMU not available, skipping ready for sleep signal");
        return;
    }

    // Flush flash buffer metadata before power down
    // Without this, write_index isn't persisted and records get corrupted on next boot
    if (flash_buffer_) {
        flash_buffer_->flush();
    }

    // Set flag for deferred processing in onLoop()
    // Don't call PMU protocol directly here - may be inside callback chain
    pmu_logger.debug("Requesting sleep (will send in main loop)");
    sleep_requested_ = true;
}

void SensorMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry* entry) {
    switch (reason) {
        case PMU::WakeReason::Periodic:
            pmu_logger.info("Periodic wake - requesting backlog check");
            // Set flag for deferred processing in onLoop() to avoid deep call stack
            backlog_check_requested_ = true;
            break;

        case PMU::WakeReason::Scheduled:
            // Sensor mode doesn't have scheduled events, but log if received
            pmu_logger.warn("Unexpected scheduled wake in sensor mode");
            break;

        case PMU::WakeReason::External:
            pmu_logger.info("External wake trigger");
            break;
    }
}
