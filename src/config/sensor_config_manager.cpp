#include "sensor_config_manager.h"

#include <cstddef>
#include <cstring>

SensorConfigManager::SensorConfigManager(Flash &flash_hal)
    : ConfigurationBase(flash_hal, flash_hal.getFlashSize() - (3 * FLASH_SECTOR_SIZE)),
      logger_("SCONFIG"),
      initialized_(false)
{
    // Sensor config uses third-to-last sector (NodeConfig uses last two)
    logger_.debug("Using sensor config offset 0x%08lx", flash_offset_);
    initializeDefaults();
}

bool SensorConfigManager::init()
{
    if (initialized_) {
        return true;
    }

    SensorNodeConfig loaded_config;
    if (loadConfiguration(loaded_config)) {
        config_ = loaded_config;
        logger_.info("Loaded sensor configuration from flash");
    } else {
        logger_.info("No valid sensor configuration found, using defaults");
        initializeDefaults();
        if (!saveConfiguration()) {
            logger_.warn("Failed to save default configuration to flash");
        }
    }

    initialized_ = true;
    return true;
}

bool SensorConfigManager::hasValidConfiguration()
{
    SensorNodeConfig config;
    return loadConfiguration(config);
}

bool SensorConfigManager::setParameter(ConfigParamId param_id, int32_t value)
{
    if (!validateParameter(param_id, value)) {
        logger_.error("Invalid value %ld for parameter %d", value, static_cast<int>(param_id));
        return false;
    }

    switch (param_id) {
        case ConfigParamId::SENSOR_READ_INTERVAL_S:
            config_.sensor_read_interval_s = static_cast<uint16_t>(value);
            logger_.info("Sensor read interval set to %u seconds", config_.sensor_read_interval_s);
            break;

        case ConfigParamId::TRANSMIT_INTERVAL_S:
            config_.transmit_interval_s = static_cast<uint16_t>(value);
            logger_.info("Transmit interval set to %u seconds", config_.transmit_interval_s);
            break;

        case ConfigParamId::TEMPERATURE_OFFSET:
            config_.temperature_offset = static_cast<int16_t>(value);
            logger_.info("Temperature offset set to %d centidegrees", config_.temperature_offset);
            break;

        case ConfigParamId::HUMIDITY_OFFSET:
            config_.humidity_offset = static_cast<int16_t>(value);
            logger_.info("Humidity offset set to %d centipercent", config_.humidity_offset);
            break;

        case ConfigParamId::TX_POWER:
            config_.tx_power = static_cast<uint8_t>(value);
            logger_.info("TX power set to %u dBm", config_.tx_power);
            break;

        case ConfigParamId::LED_ENABLED:
            config_.led_enabled = static_cast<uint8_t>(value);
            logger_.info("LED %s", config_.led_enabled ? "enabled" : "disabled");
            break;

        default:
            logger_.error("Unknown parameter ID: %d", static_cast<int>(param_id));
            return false;
    }

    return saveConfiguration();
}

int16_t SensorConfigManager::applyTemperatureCalibration(int16_t raw_centidegrees) const
{
    // Apply offset with overflow protection
    int32_t calibrated = static_cast<int32_t>(raw_centidegrees) + config_.temperature_offset;

    // Clamp to int16_t range
    if (calibrated > INT16_MAX) {
        return INT16_MAX;
    }
    if (calibrated < INT16_MIN) {
        return INT16_MIN;
    }
    return static_cast<int16_t>(calibrated);
}

uint16_t SensorConfigManager::applyHumidityCalibration(uint16_t raw_centipercent) const
{
    // Apply offset with overflow protection
    int32_t calibrated = static_cast<int32_t>(raw_centipercent) + config_.humidity_offset;

    // Clamp to valid humidity range (0-100%)
    if (calibrated < 0) {
        return 0;
    }
    if (calibrated > 10000) {
        return 10000;
    }
    return static_cast<uint16_t>(calibrated);
}

bool SensorConfigManager::resetToDefaults()
{
    logger_.info("Resetting sensor configuration to defaults");
    initializeDefaults();
    return saveConfiguration();
}

bool SensorConfigManager::validateParameter(ConfigParamId param_id, int32_t value)
{
    switch (param_id) {
        case ConfigParamId::SENSOR_READ_INTERVAL_S:
            return value >= MIN_SENSOR_READ_INTERVAL_S && value <= MAX_SENSOR_READ_INTERVAL_S;

        case ConfigParamId::TRANSMIT_INTERVAL_S:
            return value >= MIN_TRANSMIT_INTERVAL_S && value <= MAX_TRANSMIT_INTERVAL_S;

        case ConfigParamId::TEMPERATURE_OFFSET:
        case ConfigParamId::HUMIDITY_OFFSET:
            return value >= MIN_CALIBRATION_OFFSET && value <= MAX_CALIBRATION_OFFSET;

        case ConfigParamId::TX_POWER:
            return value >= MIN_TX_POWER && value <= MAX_TX_POWER;

        case ConfigParamId::LED_ENABLED:
            return value == 0 || value == 1;

        default:
            return false;
    }
}

bool SensorConfigManager::loadConfiguration(SensorNodeConfig &config)
{
    if (!loadConfig(config, sizeof(config))) {
        logger_.warn("Failed to read sensor configuration from flash");
        return false;
    }

    if (config.magic != SENSOR_CONFIG_MAGIC) {
        logger_.debug("Sensor config magic mismatch (found 0x%08lx, expected 0x%08x)", config.magic,
                      SENSOR_CONFIG_MAGIC);
        return false;
    }

    if (!verifyCRC(config)) {
        logger_.warn("Sensor configuration CRC verification failed");
        return false;
    }

    return true;
}

bool SensorConfigManager::saveConfiguration()
{
    config_.magic = SENSOR_CONFIG_MAGIC;
    config_.crc32 = calculateStructCRC(config_, offsetof(SensorNodeConfig, crc32));

    if (!saveConfig(config_, sizeof(config_))) {
        logger_.error("Failed to save sensor configuration to flash");
        return false;
    }

    // Verify the save was successful
    SensorNodeConfig verify_config;
    if (!loadConfiguration(verify_config)) {
        logger_.error("Sensor configuration save verification failed");
        return false;
    }

    logger_.debug("Sensor configuration saved and verified");
    return true;
}

void SensorConfigManager::initializeDefaults()
{
    std::memset(&config_, 0, sizeof(config_));

    config_.magic = SENSOR_CONFIG_MAGIC;
    config_.sensor_read_interval_s = DEFAULT_SENSOR_READ_INTERVAL_S;
    config_.transmit_interval_s = DEFAULT_TRANSMIT_INTERVAL_S;
    config_.temperature_offset = DEFAULT_TEMPERATURE_OFFSET;
    config_.humidity_offset = DEFAULT_HUMIDITY_OFFSET;
    config_.tx_power = DEFAULT_TX_POWER;
    config_.led_enabled = DEFAULT_LED_ENABLED;
    config_.crc32 = 0;  // Will be set on save
}

bool SensorConfigManager::verifyCRC(const SensorNodeConfig &config)
{
    return ConfigurationBase::verifyCRC(config, config.crc32, offsetof(SensorNodeConfig, crc32));
}
