#include "spi_device.h"
#include <string.h>
#include <stdio.h>

SPIDevice::SPIDevice(spi_inst_t* spi_port, uint cs_pin, uint32_t perf_threshold_ms, uint8_t max_retries)
    : spi_(spi_port), cs_pin_(cs_pin), perf_threshold_ms_(perf_threshold_ms), max_retries_(max_retries),
      read_mask_(0x7F), write_mask_(0x80), logger_("SPIDevice") {
    
    // Initialize CS pin
    gpio_init(cs_pin_);
    gpio_set_dir(cs_pin_, GPIO_OUT);
    gpio_put(cs_pin_, 1); // CS high (inactive)
    
    last_error_[0] = '\0';
}

void SPIDevice::setRegisterMasks(uint8_t read_mask, uint8_t write_mask) {
    read_mask_ = read_mask;
    write_mask_ = write_mask;
}

void SPIDevice::setErrorHandling(uint32_t perf_threshold_ms, uint8_t max_retries) {
    perf_threshold_ms_ = perf_threshold_ms;
    max_retries_ = max_retries;
}

void SPIDevice::setChipSelect(bool active) {
    gpio_put(cs_pin_, active ? 0 : 1);
}

bool SPIDevice::performTransfer(const uint8_t* tx_buf, uint8_t* rx_buf, size_t length) {
    if (!tx_buf || length == 0 || length > 256) {
        snprintf(last_error_, sizeof(last_error_), "Invalid transfer parameters: len=%zu", length);
        return false;
    }
    
    setChipSelect(true); // CS low
    
    int result;
    if (rx_buf) {
        // Read operation - bidirectional transfer
        result = spi_write_read_blocking(spi_, tx_buf, rx_buf, length);
    } else {
        // Write-only operation
        result = spi_write_blocking(spi_, tx_buf, length);
    }
    
    setChipSelect(false); // CS high
    
    // Check if transfer completed successfully
    if (result != (int)length) {
        snprintf(last_error_, sizeof(last_error_), "Incomplete transfer: %d/%zu bytes", result, length);
        logger_.error(last_error_);
        return false;
    }
    
    return true;
}

SPIError SPIDevice::transfer(const uint8_t* tx_buf, uint8_t* rx_buf, size_t length) {
    for (uint8_t retry = 0; retry < max_retries_; retry++) {
        if (performTransfer(tx_buf, rx_buf, length)) {
            return SPI_SUCCESS;
        }
        
        if (retry < max_retries_ - 1) {
            logger_.debug("SPI retry %d/%d", retry + 1, max_retries_);
            sleep_ms(1);
        }
    }
    
    return SPI_ERROR_TRANSFER;
}

SPIError SPIDevice::writeRegister(uint8_t reg, uint8_t value, bool verify) {
    uint8_t tx_buf[2] = {static_cast<uint8_t>(reg | write_mask_), value};
    
    SPIError result = transfer(tx_buf, nullptr, 2);
    if (result != SPI_SUCCESS) {
        return result;
    }
    
    // Optionally verify the write
    if (verify) {
        uint8_t read_value;
        result = readRegister(reg, &read_value);
        if (result != SPI_SUCCESS) {
            return result;
        }
        
        if (read_value != value) {
            snprintf(last_error_, sizeof(last_error_), 
                    "Verify failed reg 0x%02X: wrote 0x%02X, read 0x%02X", 
                    reg, value, read_value);
            logger_.warn(last_error_);
            return SPI_ERROR_VERIFY_FAILED;
        }
    }
    
    return SPI_SUCCESS;
}

SPIError SPIDevice::readRegister(uint8_t reg, uint8_t* value) {
    if (!value) {
        snprintf(last_error_, sizeof(last_error_), "Null value pointer");
        return SPI_ERROR_INVALID_PARAM;
    }
    
    uint8_t tx_buf[2] = {static_cast<uint8_t>(reg & read_mask_), 0x00};
    uint8_t rx_buf[2] = {0, 0};
    
    SPIError result = transfer(tx_buf, rx_buf, 2);
    if (result == SPI_SUCCESS) {
        *value = rx_buf[1];
    }
    
    return result;
}

SPIError SPIDevice::writeBuffer(uint8_t reg, const uint8_t* data, size_t length) {
    if (!data || length == 0 || length > 255) {
        snprintf(last_error_, sizeof(last_error_), "Invalid buffer parameters");
        return SPI_ERROR_INVALID_PARAM;
    }
    
    // Allocate buffer for register + data
    uint8_t tx_buf[256];
    tx_buf[0] = static_cast<uint8_t>(reg | write_mask_);
    memcpy(&tx_buf[1], data, length);
    
    return transfer(tx_buf, nullptr, length + 1);
}

SPIError SPIDevice::readBuffer(uint8_t reg, uint8_t* data, size_t length) {
    if (!data || length == 0 || length > 255) {
        snprintf(last_error_, sizeof(last_error_), "Invalid buffer parameters");
        return SPI_ERROR_INVALID_PARAM;
    }
    
    // Allocate buffers
    uint8_t tx_buf[256];
    uint8_t rx_buf[256];
    
    tx_buf[0] = static_cast<uint8_t>(reg & read_mask_);
    memset(&tx_buf[1], 0, length); // Dummy data for clocking
    
    SPIError result = transfer(tx_buf, rx_buf, length + 1);
    if (result == SPI_SUCCESS) {
        // Copy received data (skip first byte which is during register transmission)
        memcpy(data, &rx_buf[1], length);
    }
    
    return result;
}