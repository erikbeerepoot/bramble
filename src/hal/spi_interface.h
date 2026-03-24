#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Error codes for SPI operations
 */
enum SPIError {
    SPI_SUCCESS = 0,
    SPI_ERROR_TRANSFER = -1,
    SPI_ERROR_INVALID_PARAM = -2,
    SPI_ERROR_VERIFY_FAILED = -3
};

/**
 * @brief Abstract SPI interface for hardware and bit-bang implementations
 *
 * Provides the minimal interface needed by SPI peripherals (SX1262, ExternalFlash).
 * Implementations handle CS pin management and the actual byte transfer.
 */
class SPIInterface {
public:
    virtual ~SPIInterface() = default;

    /**
     * @brief Perform raw SPI transfer with CS assertion
     * @param tx_buf Transmit buffer
     * @param rx_buf Receive buffer (can be nullptr for write-only)
     * @param length Transfer length in bytes
     * @return SPIError code
     */
    virtual SPIError transfer(const uint8_t *tx_buf, uint8_t *rx_buf, size_t length) = 0;
};
