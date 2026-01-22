#pragma once

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "../hal/logger.h"
#include "../hal/spi_device.h"

// SX1276 Register Map
#define SX1276_REG_FIFO 0x00
#define SX1276_REG_OP_MODE 0x01
#define SX1276_REG_FRF_MSB 0x06
#define SX1276_REG_FRF_MID 0x07
#define SX1276_REG_FRF_LSB 0x08
#define SX1276_REG_PA_CONFIG 0x09
#define SX1276_REG_PA_RAMP 0x0A
#define SX1276_REG_OCP 0x0B
#define SX1276_REG_LNA 0x0C
#define SX1276_REG_FIFO_ADDR_PTR 0x0D
#define SX1276_REG_FIFO_TX_BASE_ADDR 0x0E
#define SX1276_REG_FIFO_RX_BASE_ADDR 0x0F
#define SX1276_REG_FIFO_RX_CURRENT_ADDR 0x10
#define SX1276_REG_IRQ_FLAGS_MASK 0x11
#define SX1276_REG_IRQ_FLAGS 0x12
#define SX1276_REG_RX_NB_BYTES 0x13
#define SX1276_REG_PKT_SNR_VALUE 0x19
#define SX1276_REG_PKT_RSSI_VALUE 0x1A
#define SX1276_REG_RSSI_VALUE 0x1B
#define SX1276_REG_MODEM_CONFIG_1 0x1D
#define SX1276_REG_MODEM_CONFIG_2 0x1E
#define SX1276_REG_SYMB_TIMEOUT_LSB 0x1F
#define SX1276_REG_PREAMBLE_MSB 0x20
#define SX1276_REG_PREAMBLE_LSB 0x21
#define SX1276_REG_PAYLOAD_LENGTH 0x22
#define SX1276_REG_MAX_PAYLOAD_LENGTH 0x23
#define SX1276_REG_HOP_PERIOD 0x24
#define SX1276_REG_FIFO_RX_BYTE_ADDR 0x25
#define SX1276_REG_MODEM_CONFIG_3 0x26
#define SX1276_REG_DIO_MAPPING_1 0x40
#define SX1276_REG_DIO_MAPPING_2 0x41
#define SX1276_REG_VERSION 0x42
#define SX1276_REG_PA_DAC 0x4D

// Operating Modes
#define SX1276_MODE_LONG_RANGE_MODE 0x80
#define SX1276_MODE_SLEEP 0x00
#define SX1276_MODE_STDBY 0x01
#define SX1276_MODE_TX 0x03
#define SX1276_MODE_RXCONTINUOUS 0x05
#define SX1276_MODE_RXSINGLE 0x06

// IRQ Flags
#define SX1276_IRQ_TX_DONE_MASK 0x08
#define SX1276_IRQ_RX_DONE_MASK 0x40
#define SX1276_IRQ_CAD_DONE_MASK 0x04
#define SX1276_IRQ_FHSS_CHANGE_MASK 0x02
#define SX1276_IRQ_CAD_DETECTED_MASK 0x01

// PA Config
#define SX1276_PA_SELECT_PA_BOOST 0x80
#define SX1276_PA_SELECT_RFO 0x00

// Default configuration values
#define SX1276_DEFAULT_FREQUENCY 915000000  // 915 MHz
#define SX1276_DEFAULT_TX_POWER 14          // 14 dBm
#define SX1276_DEFAULT_SPREADING_FACTOR 7   // SF7
#define SX1276_DEFAULT_BANDWIDTH 125000     // 125 kHz
#define SX1276_DEFAULT_CODING_RATE 5        // 4/5
#define SX1276_DEFAULT_PREAMBLE_LENGTH 8    // 8 symbols
#define SX1276_DEFAULT_CRC true             // CRC enabled

// Timing constants
#define SX1276_MODE_READY_TIMEOUT_MS 100  // 100ms timeout for mode changes
#define SX1276_TX_READY_TIMEOUT_MS 1000   // 1s timeout for TX ready

/**
 * @brief SX1276 LoRa transceiver driver for Raspberry Pi Pico
 */
class SX1276 {
public:
    /**
     * @brief Construct SX1276 driver
     * @param spi_port SPI port (spi0 or spi1)
     * @param cs_pin Chip select GPIO pin
     * @param rst_pin Reset GPIO pin (optional, use -1 if not connected)
     * @param dio0_pin DIO0 interrupt pin (optional, use -1 for polling mode)
     */
    SX1276(spi_inst_t *spi_port, uint cs_pin, int rst_pin = -1, int dio0_pin = -1);

    /**
     * @brief Initialize the SX1276 module
     * @return true if initialization successful, false otherwise
     */
    bool begin();

    /**
     * @brief Check if SX1276 is connected and responding
     * @return true if module responds correctly
     */
    bool isConnected();

    /**
     * @brief Set transmit power
     * @param power_db Power in dBm (2-20)
     */
    void setTxPower(int power_db);

    /**
     * @brief Set carrier frequency
     * @param frequency_hz Frequency in Hz (e.g., 915000000 for 915 MHz)
     */
    void setFrequency(uint32_t frequency_hz);

    /**
     * @brief Set spreading factor
     * @param sf Spreading factor (6-12)
     */
    void setSpreadingFactor(int sf);

    /**
     * @brief Set signal bandwidth
     * @param bandwidth_hz Bandwidth in Hz (7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000,
     * 250000, 500000)
     */
    void setBandwidth(uint32_t bandwidth_hz);

    /**
     * @brief Set coding rate
     * @param denominator Coding rate denominator (5-8 for 4/5, 4/6, 4/7, 4/8)
     */
    void setCodingRate(int denominator);

    /**
     * @brief Set preamble length
     * @param length Preamble length in symbols (6-65535)
     */
    void setPreambleLength(int length);

    /**
     * @brief Enable or disable CRC
     * @param enable_crc true to enable CRC, false to disable
     */
    void setCrc(bool enable_crc);

    /**
     * @brief Send a packet (blocking in non-interrupt mode)
     * @param data Pointer to data buffer
     * @param length Length of data to send (max 255 bytes)
     * @return true if packet was queued for transmission
     */
    bool send(const uint8_t *data, size_t length);

    /**
     * @brief Send a packet asynchronously (non-blocking, requires interrupt mode)
     * @param data Data to send
     * @param length Data length (max 255 bytes)
     * @return true if transmission started successfully
     */
    bool sendAsync(const uint8_t *data, size_t length);

    /**
     * @brief Check if transmission is complete
     * @return true if transmission finished
     */
    bool isTxDone();

    /**
     * @brief Receive a packet
     * @param buffer Buffer to store received data
     * @param max_length Maximum buffer size
     * @return Number of bytes received, 0 if no packet available, -1 on error
     */
    int receive(uint8_t *buffer, size_t max_length);

    /**
     * @brief Check if a packet is available for reading
     * @return true if packet available
     */
    bool available();

    /**
     * @brief Get RSSI of last received packet
     * @return RSSI in dBm
     */
    int getRssi();

    /**
     * @brief Get SNR of last received packet
     * @return SNR in dB
     */
    float getSnr();

    /**
     * @brief Put module in sleep mode to save power
     */
    void sleep();

    /**
     * @brief Wake module from sleep mode
     */
    void wakeup();

    /**
     * @brief Set module to continuous receive mode
     */
    void startReceive();

    /**
     * @brief Enable interrupt mode using DIO0 pin
     * @param callback Function to call when interrupt fires (optional)
     * @return true if interrupt mode enabled successfully
     */
    bool enableInterruptMode(gpio_irq_callback_t callback = nullptr);

    /**
     * @brief Disable interrupt mode
     */
    void disableInterruptMode();

    /**
     * @brief Check if an interrupt has fired (must be called from main context)
     * @return true if interrupt pending
     */
    bool isInterruptPending() const { return interrupt_pending_; }

    /**
     * @brief Handle pending interrupt (call after isInterruptPending returns true)
     * @return Interrupt flags that were set
     */
    uint8_t handleInterrupt();

    /**
     * @brief Check if a message is ready after interrupt
     * @return true if RX done with valid message
     */
    bool isMessageReady() const { return message_ready_; }

    /**
     * @brief Check if transmission is complete after interrupt
     * @return true if TX done
     */
    bool isTxComplete() const { return tx_complete_; }

    /**
     * @brief Clear interrupt flags
     */
    void clearInterruptFlags();

    /**
     * @brief Reset the module (if reset pin connected)
     */
    void reset();

    // For interrupt handler access
    int getDio0Pin() const { return dio0_pin_; }
    void setInterruptPending() { interrupt_pending_ = true; }

private:
    SPIDevice spi_;
    int rst_pin_;
    int dio0_pin_;

    uint32_t frequency_;
    int tx_power_;
    int spreading_factor_;
    uint32_t bandwidth_;
    int coding_rate_;
    int preamble_length_;
    bool crc_enabled_;

    Logger logger_;

    // Interrupt mode support
    volatile bool interrupt_pending_;
    volatile bool message_ready_;
    volatile bool tx_complete_;
    bool interrupt_enabled_;
    gpio_irq_callback_t user_callback_;

    /**
     * @brief Write to SX1276 register
     * @param reg Register address
     * @param value Value to write
     */
    void writeRegister(uint8_t reg, uint8_t value);

    /**
     * @brief Read from SX1276 register
     * @param reg Register address
     * @return Register value
     */
    uint8_t readRegister(uint8_t reg);

    /**
     * @brief Set operating mode
     * @param mode Operating mode
     */
    void setMode(uint8_t mode);

    /**
     * @brief Get current operating mode
     * @return Current mode
     */
    uint8_t getMode();

    /**
     * @brief Configure LoRa mode parameters
     */
    void configureLoRa();

    /**
     * @brief Convert bandwidth to register value
     * @param bandwidth_hz Bandwidth in Hz
     * @return Register value
     */
    uint8_t getBandwidthRegValue(uint32_t bandwidth_hz);

    /**
     * @brief Wait for mode to be ready
     * @param target_mode Expected mode
     * @param timeout_ms Timeout in milliseconds
     * @return true if mode ready, false on timeout
     */
    bool waitForModeReady(uint8_t target_mode, uint32_t timeout_ms = SX1276_MODE_READY_TIMEOUT_MS);
};