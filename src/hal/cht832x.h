#pragma once

#include <cstdint>
#include "hardware/i2c.h"

/**
 * @brief Driver for CHT832X temperature/humidity sensor (DFRobot SEN0546)
 *
 * The CHT832X is a high-accuracy I2C temperature and humidity sensor.
 * - Temperature accuracy: +/-0.1C typical
 * - Humidity accuracy: +/-1.5% RH typical
 * - I2C address: 0x44
 */
class CHT832X {
public:
    static constexpr uint8_t I2C_ADDRESS = 0x44;

    /**
     * @brief Sensor reading result
     */
    struct Reading {
        float temperature;  // Celsius
        float humidity;     // Percent RH
        bool valid;         // True if reading succeeded
    };

    /**
     * @brief Constructor
     * @param i2c I2C instance (i2c0 or i2c1)
     * @param sda_pin GPIO pin for SDA
     * @param scl_pin GPIO pin for SCL
     */
    CHT832X(i2c_inst_t* i2c, uint sda_pin, uint scl_pin);

    /**
     * @brief Initialize the sensor
     * @return true if sensor responds on I2C bus
     */
    bool init();

    /**
     * @brief Read temperature and humidity
     * @return Reading struct with temperature, humidity, and validity flag
     */
    Reading read();

    /**
     * @brief Check if sensor is connected
     * @return true if sensor responds to I2C address probe
     */
    bool isConnected();

private:
    i2c_inst_t* i2c_;
    uint sda_pin_;
    uint scl_pin_;
    bool initialized_;

    static constexpr uint32_t I2C_BAUDRATE = 100000;  // 100kHz
    static constexpr uint32_t MEASUREMENT_DELAY_MS = 60;  // Wait for measurement

    // CHT832X commands
    static constexpr uint8_t CMD_MEASURE_HIGH = 0x24;
    static constexpr uint8_t CMD_MEASURE_LOW = 0x00;
};
