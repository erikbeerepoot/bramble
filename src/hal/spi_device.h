#pragma once

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "logger.h"

// SPI Error codes
enum SPIError {
    SPI_SUCCESS = 0,
    SPI_ERROR_TRANSFER = -1,
    SPI_ERROR_INVALID_PARAM = -2,
    SPI_ERROR_VERIFY_FAILED = -3
};

/**
 * @brief Simple SPI device wrapper
 *
 * Provides a clean SPI interface with:
 * - CS pin management
 * - Input validation
 * - Transfer verification
 * - Future DMA compatibility
 */
class SPIDevice {
public:
    /**
     * @brief Construct a new SPI device wrapper
     * @param spi_port SPI instance (spi0 or spi1)
     * @param cs_pin Chip select GPIO pin
     */
    SPIDevice(spi_inst_t *spi_port, uint cs_pin);

    /**
     * @brief Write to a register with error handling
     * @param reg Register address
     * @param value Value to write
     * @param verify Whether to read back and verify the write
     * @return SPIError code
     */
    SPIError writeRegister(uint8_t reg, uint8_t value, bool verify = false);

    /**
     * @brief Read from a register with error handling
     * @param reg Register address
     * @param value Output value pointer
     * @return SPIError code
     */
    SPIError readRegister(uint8_t reg, uint8_t *value);

    /**
     * @brief Write multiple bytes (caller provides buffer)
     * @param reg Starting register address
     * @param data Data to write
     * @param length Number of data bytes to write
     * @param tx_buf Caller-provided buffer (must be at least length+1 bytes)
     * @return SPIError code
     */
    SPIError writeBuffer(uint8_t reg, const uint8_t *data, size_t length, uint8_t *tx_buf);

    /**
     * @brief Read multiple bytes (caller provides buffers)
     * @param reg Starting register address
     * @param data Output data buffer
     * @param length Number of bytes to read
     * @param tx_buf Caller-provided TX buffer (must be at least length+1 bytes)
     * @param rx_buf Caller-provided RX buffer (must be at least length+1 bytes)
     * @return SPIError code
     */
    SPIError readBuffer(uint8_t reg, uint8_t *data, size_t length, uint8_t *tx_buf,
                        uint8_t *rx_buf);

    /**
     * @brief Perform raw SPI transfer
     * @param tx_buf Transmit buffer
     * @param rx_buf Receive buffer (can be NULL for write-only)
     * @param length Transfer length
     * @return SPIError code
     */
    SPIError transfer(const uint8_t *tx_buf, uint8_t *rx_buf, size_t length);

    /**
     * @brief Set custom read/write bit masks
     * @param read_mask Mask to apply for read operations (default: clear MSB)
     * @param write_mask Mask to apply for write operations (default: set MSB)
     */
    void setRegisterMasks(uint8_t read_mask, uint8_t write_mask);

private:
    spi_inst_t *spi_;
    uint cs_pin_;
    uint8_t read_mask_;
    uint8_t write_mask_;
    Logger logger_;
};