#include "cht832x.h"

#include "pico/stdlib.h"

#include "logger.h"

static Logger logger("CHT832X");

CHT832X::CHT832X(i2c_inst_t *i2c, uint sda_pin, uint scl_pin)
    : i2c_(i2c), sda_pin_(sda_pin), scl_pin_(scl_pin), initialized_(false)
{
}

bool CHT832X::init()
{
    // Initialize I2C
    i2c_init(i2c_, I2C_BAUDRATE);

    // Set up GPIO pins for I2C
    gpio_set_function(sda_pin_, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin_, GPIO_FUNC_I2C);

    // Enable pull-ups (sensor has built-in 4.7k, but internal helps with longer wires)
    gpio_pull_up(sda_pin_);
    gpio_pull_up(scl_pin_);

    logger.debug("I2C initialized on SDA=%d, SCL=%d", sda_pin_, scl_pin_);

    // Check if sensor responds
    if (!isConnected()) {
        logger.error("Sensor not found at address 0x%02X", I2C_ADDRESS);
        return false;
    }

    initialized_ = true;
    logger.debug("Sensor initialized successfully");
    return true;
}

bool CHT832X::isConnected()
{
    // Try to read a single byte to check if device responds
    uint8_t dummy;
    int result = i2c_read_blocking(i2c_, I2C_ADDRESS, &dummy, 1, false);
    return result >= 0;
}

CHT832X::Reading CHT832X::read()
{
    Reading reading = {0.0f, 0.0f, false};

    if (!initialized_) {
        logger.error("Sensor not initialized");
        return reading;
    }

    // Send measurement command (high precision mode)
    uint8_t cmd[2] = {CMD_MEASURE_HIGH, CMD_MEASURE_LOW};
    int result = i2c_write_blocking(i2c_, I2C_ADDRESS, cmd, 2, false);

    if (result != 2) {
        logger.error("Failed to send measurement command");
        return reading;
    }

    // Wait for measurement to complete
    sleep_ms(MEASUREMENT_DELAY_MS);

    // Read 6 bytes: temp_msb, temp_lsb, temp_crc, hum_msb, hum_lsb, hum_crc
    uint8_t data[6];
    result = i2c_read_blocking(i2c_, I2C_ADDRESS, data, 6, false);

    if (result != 6) {
        logger.error("Failed to read sensor data (got %d bytes)", result);
        return reading;
    }

    // Parse raw values (skip CRC bytes at positions 2 and 5)
    uint16_t raw_temp = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint16_t raw_hum = (static_cast<uint16_t>(data[3]) << 8) | data[4];

    // Convert to physical values using CHT832X formulas
    reading.temperature = -45.0f + 175.0f * (static_cast<float>(raw_temp) / 65535.0f);
    reading.humidity = 100.0f * (static_cast<float>(raw_hum) / 65535.0f);
    reading.valid = true;

    logger.debug("Raw: temp=0x%04X, hum=0x%04X", raw_temp, raw_hum);
    logger.debug("Converted: %.2fC, %.2f%%RH", reading.temperature, reading.humidity);

    return reading;
}
