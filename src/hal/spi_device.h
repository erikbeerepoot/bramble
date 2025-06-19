#pragma once

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "logger.h"

// SPI Communication configuration
#define SPI_DEFAULT_PERF_THRESHOLD_MS  10   // 10ms performance monitoring threshold
#define SPI_DEFAULT_MAX_RETRIES        3    // Maximum retries for SPI operations

// SPI Error codes
enum SPIError {
    SPI_SUCCESS = 0,
    SPI_ERROR_TIMEOUT = -1,
    SPI_ERROR_TRANSFER = -2,
    SPI_ERROR_INVALID_PARAM = -3,
    SPI_ERROR_VERIFY_FAILED = -4
};

/**
 * @brief Safe SPI device wrapper with error handling and retry logic
 * 
 * This class provides a robust SPI interface with:
 * - Timeout protection
 * - Automatic retry on failure
 * - Optional write verification
 * - Detailed error logging
 */
class SPIDevice {
public:
    /**
     * @brief Construct a new SPI device wrapper
     * @param spi_port SPI instance (spi0 or spi1)
     * @param cs_pin Chip select GPIO pin
     * @param perf_threshold_ms Performance monitoring threshold in milliseconds
     * @param max_retries Maximum retry attempts
     */
    SPIDevice(spi_inst_t* spi_port, uint cs_pin, 
              uint32_t perf_threshold_ms = SPI_DEFAULT_PERF_THRESHOLD_MS,
              uint8_t max_retries = SPI_DEFAULT_MAX_RETRIES);
    
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
    SPIError readRegister(uint8_t reg, uint8_t* value);
    
    /**
     * @brief Write multiple bytes to a register
     * @param reg Starting register address
     * @param data Data buffer to write
     * @param length Number of bytes to write
     * @return SPIError code
     */
    SPIError writeBuffer(uint8_t reg, const uint8_t* data, size_t length);
    
    /**
     * @brief Read multiple bytes from a register
     * @param reg Starting register address
     * @param data Output data buffer
     * @param length Number of bytes to read
     * @return SPIError code
     */
    SPIError readBuffer(uint8_t reg, uint8_t* data, size_t length);
    
    /**
     * @brief Perform raw SPI transfer
     * @param tx_buf Transmit buffer
     * @param rx_buf Receive buffer (can be NULL for write-only)
     * @param length Transfer length
     * @return SPIError code
     */
    SPIError transfer(const uint8_t* tx_buf, uint8_t* rx_buf, size_t length);
    
    /**
     * @brief Set custom read/write bit masks
     * @param read_mask Mask to apply for read operations (default: clear MSB)
     * @param write_mask Mask to apply for write operations (default: set MSB)
     */
    void setRegisterMasks(uint8_t read_mask, uint8_t write_mask);
    
    /**
     * @brief Update performance threshold and retry settings
     * @param perf_threshold_ms New performance monitoring threshold in milliseconds
     * @param max_retries New maximum retry count
     */
    void setErrorHandling(uint32_t perf_threshold_ms, uint8_t max_retries);
    
    /**
     * @brief Get the last error details
     * @return Human-readable error description
     */
    const char* getLastError() const { return last_error_; }
    
private:
    spi_inst_t* spi_;
    uint cs_pin_;
    uint32_t perf_threshold_ms_;
    uint8_t max_retries_;
    uint8_t read_mask_;
    uint8_t write_mask_;
    char last_error_[128];
    Logger logger_;
    
    /**
     * @brief Perform SPI transfer with timeout protection
     * @param tx_buf Transmit buffer
     * @param rx_buf Receive buffer (can be NULL)
     * @param length Transfer length
     * @return true on success, false on timeout/error
     */
    bool performTransfer(const uint8_t* tx_buf, uint8_t* rx_buf, size_t length);
    
    /**
     * @brief Set chip select state
     * @param active true to activate (low), false to deactivate (high)
     */
    void setChipSelect(bool active);
};