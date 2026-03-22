#ifndef FRAM_H
#define FRAM_H

#include "stm32l0xx_hal.h"

#include <cstdint>

// FM24CL16B-GTR FRAM driver
// 16Kbit (2048 bytes) I2C FRAM organized as 8 pages of 256 bytes.
// The 3 LSBs of the I2C device address select the page, and an 8-bit
// word address selects the byte within the page.
class FRAM {
public:
    explicit FRAM(I2C_HandleTypeDef& i2c);

    // Probe the device on the bus. Returns true if FRAM responds.
    bool init();

    // Read `length` bytes starting at `address` into `data`.
    // Handles cross-page boundaries transparently.
    bool read(uint16_t address, uint8_t* data, uint16_t length);

    // Write `length` bytes from `data` starting at `address`.
    // Handles cross-page boundaries transparently.
    // FRAM has no write delay — returns immediately after I2C transfer.
    bool write(uint16_t address, const uint8_t* data, uint16_t length);

    bool isPresent() const { return present_; }

    static constexpr uint16_t CAPACITY = 2048;

private:
    I2C_HandleTypeDef& i2c_;
    bool present_;

    static constexpr uint8_t BASE_ADDRESS = 0x50;
    static constexpr uint16_t PAGE_SIZE = 256;
    static constexpr uint32_t TIMEOUT_MS = 100;

    // Compute the 7-bit I2C slave address for a given memory address.
    // Upper 3 bits of the 11-bit address become bits [3:1] of the slave address.
    uint8_t slaveAddress(uint16_t memoryAddress) const;

    // Lower 8 bits of the memory address (word address within page).
    uint8_t wordAddress(uint16_t memoryAddress) const;
};

#endif // FRAM_H
