#include "bitbang_spi_device.h"

BitBangSPIDevice::BitBangSPIDevice(uint mosi_pin, uint miso_pin, uint sck_pin, uint cs_pin)
    : mosi_pin_(mosi_pin), miso_pin_(miso_pin), sck_pin_(sck_pin), cs_pin_(cs_pin),
      logger_("BitBangSPI")
{
    // MOSI: output, idle low
    gpio_init(mosi_pin_);
    gpio_set_dir(mosi_pin_, GPIO_OUT);
    gpio_put(mosi_pin_, 0);

    // MISO: input
    gpio_init(miso_pin_);
    gpio_set_dir(miso_pin_, GPIO_IN);

    // SCK: output, idle low (Mode 0: CPOL=0)
    gpio_init(sck_pin_);
    gpio_set_dir(sck_pin_, GPIO_OUT);
    gpio_put(sck_pin_, 0);

    // CS: output, idle high (deselected)
    gpio_init(cs_pin_);
    gpio_set_dir(cs_pin_, GPIO_OUT);
    gpio_put(cs_pin_, 1);
}

uint8_t BitBangSPIDevice::transferByte(uint8_t tx_byte)
{
    uint8_t rx_byte = 0;

    for (int bit = 7; bit >= 0; bit--) {
        // Set MOSI on falling edge of SCK (SCK is already low)
        gpio_put(mosi_pin_, (tx_byte >> bit) & 1);

        // Rising edge — peripheral latches MOSI, drives MISO
        gpio_put(sck_pin_, 1);

        // Sample MISO
        rx_byte |= (gpio_get(miso_pin_) ? 1 : 0) << bit;

        // Falling edge
        gpio_put(sck_pin_, 0);
    }

    return rx_byte;
}

SPIError BitBangSPIDevice::transfer(const uint8_t *tx_buf, uint8_t *rx_buf, size_t length)
{
    if (!tx_buf || length == 0) {
        logger_.error("Invalid transfer parameters");
        return SPI_ERROR_INVALID_PARAM;
    }

    gpio_put(cs_pin_, 0);  // Assert CS

    for (size_t i = 0; i < length; i++) {
        uint8_t rx = transferByte(tx_buf[i]);
        if (rx_buf) {
            rx_buf[i] = rx;
        }
    }

    gpio_put(cs_pin_, 1);  // Deassert CS

    return SPI_SUCCESS;
}
