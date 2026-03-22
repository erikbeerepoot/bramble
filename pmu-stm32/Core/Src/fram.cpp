#include "fram.h"

#include <algorithm>

FRAM::FRAM(I2C_HandleTypeDef& i2c) : i2c_(i2c), present_(false) {}

bool FRAM::init()
{
    // Probe the base address to check if the FRAM is on the bus
    HAL_StatusTypeDef status =
        HAL_I2C_IsDeviceReady(&i2c_, static_cast<uint16_t>(BASE_ADDRESS) << 1, 3, TIMEOUT_MS);
    present_ = (status == HAL_OK);
    return present_;
}

bool FRAM::read(uint16_t address, uint8_t* data, uint16_t length)
{
    return transfer(address, data, length, HAL_I2C_Mem_Read);
}

bool FRAM::write(uint16_t address, const uint8_t* data, uint16_t length)
{
    return transfer(address, const_cast<uint8_t*>(data), length, HAL_I2C_Mem_Write);
}

bool FRAM::transfer(uint16_t address, uint8_t* data, uint16_t length, I2COperation operation)
{
    if (!present_ || address + length > CAPACITY) {
        return false;
    }

    uint16_t remaining = length;
    uint16_t offset = 0;

    while (remaining > 0) {
        uint16_t currentAddress = address + offset;
        uint8_t slave = slaveAddress(currentAddress);
        uint8_t word = wordAddress(currentAddress);

        // Bytes left in the current 256-byte page
        uint16_t bytesInPage = PAGE_SIZE - word;
        uint16_t chunk = std::min(remaining, bytesInPage);

        HAL_StatusTypeDef status = operation(
            &i2c_, static_cast<uint16_t>(slave) << 1, word, I2C_MEMADD_SIZE_8BIT,
            data + offset, chunk, TIMEOUT_MS);

        if (status != HAL_OK) {
            return false;
        }

        offset += chunk;
        remaining -= chunk;
    }

    return true;
}

uint8_t FRAM::slaveAddress(uint16_t memoryAddress) const
{
    // Page = bits [10:8] of the memory address
    uint8_t page = (memoryAddress >> 8) & 0x07;
    return BASE_ADDRESS | page;
}

uint8_t FRAM::wordAddress(uint16_t memoryAddress) const
{
    return static_cast<uint8_t>(memoryAddress & 0xFF);
}
