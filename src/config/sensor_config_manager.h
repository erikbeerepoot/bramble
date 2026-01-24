#pragma once

#include <stdint.h>

#include "config_base.h"
#include "hal/flash.h"
#include "hal/logger.h"
#include "sensor_config.h"

/**
 * @brief Manager for sensor node configuration persistence
 *
 * Handles loading/saving sensor configuration to internal flash.
 * Uses a dedicated sector (second-to-last) to avoid conflicts with NodeConfiguration.
 *
 * Flash layout (from end):
 * - Last sector: NodeConfiguration (primary)
 * - Second-to-last: NodeConfiguration (backup)
 * - Third-to-last: SensorNodeConfig (primary)
 * - Fourth-to-last: SensorNodeConfig (backup)
 */
class SensorConfigManager : public ConfigurationBase {
public:
    /**
     * @brief Construct sensor config manager
     * @param flash_hal Flash hardware abstraction layer
     */
    explicit SensorConfigManager(Flash &flash_hal);

    /**
     * @brief Initialize and load configuration from flash
     *
     * Loads configuration if valid, otherwise creates defaults.
     * Call this once during sensor mode initialization.
     *
     * @return true if initialization successful (even if using defaults)
     */
    bool init();

    /**
     * @brief Check if valid configuration exists in flash
     * @return true if valid configuration found
     */
    bool hasValidConfiguration();

    /**
     * @brief Get current configuration
     * @return Reference to current configuration
     */
    const SensorNodeConfig &getConfig() const { return config_; }

    /**
     * @brief Set a single configuration parameter
     *
     * Validates the value and saves to flash if valid.
     *
     * @param param_id Parameter ID to set
     * @param value New value for the parameter
     * @return true if parameter set and saved successfully
     */
    bool setParameter(ConfigParamId param_id, int32_t value);

    /**
     * @brief Get sensor read interval in milliseconds
     * @return Sensor read interval (ms)
     */
    uint32_t getSensorReadIntervalMs() const { return static_cast<uint32_t>(config_.sensor_read_interval_s) * 1000; }

    /**
     * @brief Get transmit interval in seconds
     * @return Transmit interval (seconds)
     */
    uint32_t getTransmitIntervalS() const { return config_.transmit_interval_s; }

    /**
     * @brief Get temperature calibration offset in centidegrees
     * @return Temperature offset (0.01°C units)
     */
    int16_t getTemperatureOffset() const { return config_.temperature_offset; }

    /**
     * @brief Get humidity calibration offset in centipercent
     * @return Humidity offset (0.01% units)
     */
    int16_t getHumidityOffset() const { return config_.humidity_offset; }

    /**
     * @brief Get TX power setting
     * @return TX power in dBm
     */
    uint8_t getTxPower() const { return config_.tx_power; }

    /**
     * @brief Check if status LED is enabled
     * @return true if LED is enabled
     */
    bool isLedEnabled() const { return config_.led_enabled != 0; }

    /**
     * @brief Apply temperature calibration offset to a reading
     * @param raw_centidegrees Raw temperature in 0.01°C units
     * @return Calibrated temperature in 0.01°C units
     */
    int16_t applyTemperatureCalibration(int16_t raw_centidegrees) const;

    /**
     * @brief Apply humidity calibration offset to a reading
     * @param raw_centipercent Raw humidity in 0.01% units
     * @return Calibrated humidity in 0.01% units (clamped to 0-10000)
     */
    uint16_t applyHumidityCalibration(uint16_t raw_centipercent) const;

    /**
     * @brief Reset configuration to defaults
     * @return true if reset successful
     */
    bool resetToDefaults();

    /**
     * @brief Validate a parameter value before applying
     * @param param_id Parameter ID
     * @param value Value to validate
     * @return true if value is within valid range
     */
    static bool validateParameter(ConfigParamId param_id, int32_t value);

private:
    SensorNodeConfig config_;
    Logger logger_;
    bool initialized_;

    /**
     * @brief Load configuration from flash
     * @param config Output configuration structure
     * @return true if load successful and valid
     */
    bool loadConfiguration(SensorNodeConfig &config);

    /**
     * @brief Save current configuration to flash
     * @return true if save successful
     */
    bool saveConfiguration();

    /**
     * @brief Initialize configuration with default values
     */
    void initializeDefaults();

    /**
     * @brief Verify configuration CRC
     * @param config Configuration to verify
     * @return true if CRC matches
     */
    bool verifyCRC(const SensorNodeConfig &config);
};
