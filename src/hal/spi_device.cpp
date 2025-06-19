#include "spi_device.h"
#include <string.h>

SPIDevice::SPIDevice(spi_inst_t* spi_port, uint cs_pin)
    : spi_(spi_port), cs_pin_(cs_pin), 
      read_mask_(0x7F), write_mask_(0x80), logger_("SPIDevice") {
    
    // Initialize CS pin
    gpio_init(cs_pin_);
    gpio_set_dir(cs_pin_, GPIO_OUT);
    gpio_put(cs_pin_, 1); // CS high (inactive)
}

void SPIDevice::setRegisterMasks(uint8_t read_mask, uint8_t write_mask) {
    read_mask_ = read_mask;
    write_mask_ = write_mask;
}

SPIError SPIDevice::transfer(const uint8_t* tx_buf, uint8_t* rx_buf, size_t length) {
    if (!tx_buf || length == 0) {
        logger_.error("Invalid transfer parameters");
        return SPI_ERROR_INVALID_PARAM;
    }
    
    gpio_put(cs_pin_, 0); // CS low
    
    int result;
    if (rx_buf) {
        result = spi_write_read_blocking(spi_, tx_buf, rx_buf, length);
    } else {
        result = spi_write_blocking(spi_, tx_buf, length);
    }
    
    gpio_put(cs_pin_, 1); // CS high
    
    if (result != (int)length) {
        logger_.error("SPI transfer incomplete: %d/%zu bytes", result, length);
        return SPI_ERROR_TRANSFER;
    }
    
    return SPI_SUCCESS;
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
            logger_.warn("Write verify failed reg 0x%02X: wrote 0x%02X, read 0x%02X", 
                        reg, value, read_value);
            return SPI_ERROR_TRANSFER;
        }
    }
    
    return SPI_SUCCESS;
}

SPIError SPIDevice::readRegister(uint8_t reg, uint8_t* value) {
    if (!value) {
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

SPIError SPIDevice::writeBuffer(uint8_t reg, const uint8_t* data, size_t length, uint8_t* tx_buf) {
    if (!data || !tx_buf || length == 0) {
        return SPI_ERROR_INVALID_PARAM;
    }
    
    // Build transfer buffer: register + data
    tx_buf[0] = static_cast<uint8_t>(reg | write_mask_);
    memcpy(&tx_buf[1], data, length);
    
    return transfer(tx_buf, nullptr, length + 1);
}

SPIError SPIDevice::readBuffer(uint8_t reg, uint8_t* data, size_t length, 
                               uint8_t* tx_buf, uint8_t* rx_buf) {
    if (!data || !tx_buf || !rx_buf || length == 0) {
        return SPI_ERROR_INVALID_PARAM;
    }
    
    // Build transfer buffer: register + dummy bytes
    tx_buf[0] = static_cast<uint8_t>(reg & read_mask_);
    memset(&tx_buf[1], 0, length);
    
    SPIError result = transfer(tx_buf, rx_buf, length + 1);
    if (result == SPI_SUCCESS) {
        // Copy received data (skip first byte)
        memcpy(data, &rx_buf[1], length);
    }
    
    return result;
}