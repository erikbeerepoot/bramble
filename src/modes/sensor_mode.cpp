#include "sensor_mode.h"
#include "../hal/cht832x.h"
#include "../hal/logger.h"
#include "../lora/reliable_messenger.h"
#include "../lora/message.h"
#include "../led_patterns.h"
#include "hardware/i2c.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;

static Logger logger("SENSOR");

SensorMode::~SensorMode() = default;

void SensorMode::onStart() {
    logger.info("=== SENSOR MODE ACTIVE ===");
    logger.info("- Temperature/humidity data logger");
    logger.info("- 30 second reading interval");
    logger.info("- Purple LED breathing pattern");

    // Initialize external flash for sensor data storage
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

    // Purple breathing pattern for sensor nodes
    led_pattern_ = std::make_unique<BreathingPattern>(led_, 128, 0, 255);

    // Add periodic sensor reading task
    task_manager_.addTask(
        [this](uint32_t time) {
            readAndTransmitSensorData(time);
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
    // Nothing special needed in the main loop
    // Sensor reading is handled by periodic task
}

void SensorMode::readAndTransmitSensorData(uint32_t current_time) {
    if (!sensor_) {
        logger.error("Sensor not initialized");
        return;
    }

    auto reading = sensor_->read();

    if (!reading.valid) {
        logger.error("Failed to read sensor");
        return;
    }

    logger.info("Sensor reading: %.2fC, %.2f%%RH", reading.temperature, reading.humidity);

    // Convert to fixed-point format
    // Temperature: int16_t in 0.01C units (e.g., 2350 = 23.50C)
    // Humidity: uint16_t in 0.01% units (e.g., 6500 = 65.00%)
    int16_t temp_fixed = static_cast<int16_t>(reading.temperature * 100.0f);
    uint16_t hum_fixed = static_cast<uint16_t>(reading.humidity * 100.0f);

    // ALWAYS write to flash first (ensures zero data loss)
    uint32_t write_index = 0;
    if (flash_buffer_) {
        SensorDataRecord record = {
            .timestamp = static_cast<uint32_t>(current_time / 1000),  // Convert ms to seconds
            .temperature = temp_fixed,
            .humidity = hum_fixed,
            .flags = 0,  // Not transmitted yet
            .reserved = 0,
            .crc16 = 0   // Will be calculated by writeRecord()
        };

        // Get current write index BEFORE writing (this is what we'll mark as transmitted)
        SensorFlashMetadata stats;
        flash_buffer_->getStatistics(stats);
        write_index = stats.write_index;

        if (!flash_buffer_->writeRecord(record)) {
            logger.error("Failed to write record to flash!");
        } else {
            logger.debug("Record written to flash at index %lu (temp=%d, hum=%d)",
                        write_index, temp_fixed, hum_fixed);
        }
    }

    // Then attempt LoRa transmission (when network available)
    // Use RELIABLE criticality to get ACKs and mark records as transmitted
    uint8_t temp_data[2] = {
        static_cast<uint8_t>(temp_fixed & 0xFF),
        static_cast<uint8_t>((temp_fixed >> 8) & 0xFF)
    };

    // Send with callback to mark as transmitted on ACK
    uint8_t temp_seq = messenger_.sendSensorDataWithCallback(
        HUB_ADDRESS, SENSOR_TEMPERATURE,
        temp_data, sizeof(temp_data), RELIABLE,
        [this, write_index](uint8_t seq_num, uint8_t ack_status, uint64_t context) {
            if (ack_status == 0 && flash_buffer_) {
                // ACK received successfully - mark record as transmitted
                uint32_t flash_index = static_cast<uint32_t>(context);
                if (flash_buffer_->markTransmitted(flash_index)) {
                    logger.debug("Marked flash record %lu as transmitted (seq=%d)",
                                flash_index, seq_num);
                } else {
                    logger.warn("Failed to mark flash record %lu as transmitted", flash_index);
                }
            }
        },
        write_index  // Pass flash index as user_context
    );

    logger.debug("Transmitted temp via LoRa: seq=%d, flash_index=%lu", temp_seq, write_index);

    // Note: We're only tracking temperature ACKs for simplicity.
    // Humidity is sent separately but not tracked. In production, you might want
    // to combine temp+humidity into a single message or track both separately.
    uint8_t hum_data[2] = {
        static_cast<uint8_t>(hum_fixed & 0xFF),
        static_cast<uint8_t>((hum_fixed >> 8) & 0xFF)
    };
    messenger_.sendSensorData(HUB_ADDRESS, SENSOR_HUMIDITY,
                             hum_data, sizeof(hum_data), BEST_EFFORT);
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
        logger.debug("No valid records to transmit");
        return;
    }

    logger.info("Transmitting batch of %zu records", actual_count);

    // Transmit the batch
    if (transmitBatch(records, actual_count)) {
        logger.info("Batch transmission successful");
        // Update last sync timestamp
        flash_buffer_->updateLastSync(static_cast<uint32_t>(current_time / 1000));
    } else {
        logger.warn("Batch transmission failed, will retry later");
    }
}

bool SensorMode::transmitBatch(const SensorDataRecord* records, size_t count) {
    if (!records || count == 0) {
        return false;
    }

    // TODO: Implement batch message protocol (Phase 2)
    // For now, transmit individual records
    for (size_t i = 0; i < count; i++) {
        const auto& record = records[i];

        // Send temperature
        uint8_t temp_data[2] = {
            static_cast<uint8_t>(record.temperature & 0xFF),
            static_cast<uint8_t>((record.temperature >> 8) & 0xFF)
        };
        messenger_.sendSensorData(HUB_ADDRESS, SENSOR_TEMPERATURE,
                                 temp_data, sizeof(temp_data), RELIABLE);

        // Send humidity
        uint8_t hum_data[2] = {
            static_cast<uint8_t>(record.humidity & 0xFF),
            static_cast<uint8_t>((record.humidity >> 8) & 0xFF)
        };
        messenger_.sendSensorData(HUB_ADDRESS, SENSOR_HUMIDITY,
                                 hum_data, sizeof(hum_data), RELIABLE);

        logger.debug("Batch record %zu: temp=%d, hum=%d, timestamp=%lu",
                    i, record.temperature, record.humidity, record.timestamp);
    }

    // TODO: Mark records as transmitted only after receiving ACK from hub
    // This will be implemented in Phase 2 with proper batch protocol

    return true;
}
