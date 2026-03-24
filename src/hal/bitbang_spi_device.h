#pragma once

#include "pico/stdlib.h"

#include "hardware/gpio.h"

#include "logger.h"
#include "spi_interface.h"

/**
 * @brief GPIO bit-bang SPI device (SPI Mode 0: CPOL=0, CPHA=0)
 *
 * Used on Bramble V4 where the LoRa SPI1 MOSI/SCK traces are swapped
 * relative to the RP2350 hardware pin mux, making hardware SPI unusable.
 * Bit-bang allows arbitrary GPIO assignment.
 */
class BitBangSPIDevice : public SPIInterface {
public:
    /**
     * @brief Construct bit-bang SPI device
     * @param mosi_pin GPIO pin connected to peripheral MOSI
     * @param miso_pin GPIO pin connected to peripheral MISO
     * @param sck_pin GPIO pin connected to peripheral SCK
     * @param cs_pin GPIO chip select pin (active low)
     */
    BitBangSPIDevice(uint mosi_pin, uint miso_pin, uint sck_pin, uint cs_pin);

    SPIError transfer(const uint8_t *tx_buf, uint8_t *rx_buf, size_t length) override;

private:
    uint mosi_pin_;
    uint miso_pin_;
    uint sck_pin_;
    uint cs_pin_;
    Logger logger_;

    /**
     * @brief Transfer a single byte (full-duplex)
     * @param tx_byte Byte to transmit
     * @return Byte received
     */
    uint8_t transferByte(uint8_t tx_byte);
};
