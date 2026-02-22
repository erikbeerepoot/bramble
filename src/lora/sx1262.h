#pragma once

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "../hal/logger.h"
#include "../hal/spi_device.h"
#include "radio_interface.h"

// SX1262 Command Opcodes
#define SX1262_CMD_SET_SLEEP 0x84
#define SX1262_CMD_SET_STANDBY 0x80
#define SX1262_CMD_SET_TX 0x83
#define SX1262_CMD_SET_RX 0x82
#define SX1262_CMD_SET_PACKET_TYPE 0x8A
#define SX1262_CMD_SET_RF_FREQUENCY 0x86
#define SX1262_CMD_SET_MODULATION_PARAMS 0x8B
#define SX1262_CMD_SET_PACKET_PARAMS 0x8C
#define SX1262_CMD_SET_PA_CONFIG 0x95
#define SX1262_CMD_SET_TX_PARAMS 0x8E
#define SX1262_CMD_SET_DIO_IRQ_PARAMS 0x08
#define SX1262_CMD_SET_DIO3_AS_TCXO_CTRL 0x97
#define SX1262_CMD_SET_BUFFER_BASE_ADDRESS 0x8F
#define SX1262_CMD_WRITE_BUFFER 0x0E
#define SX1262_CMD_READ_BUFFER 0x1E
#define SX1262_CMD_GET_IRQ_STATUS 0x12
#define SX1262_CMD_CLEAR_IRQ_STATUS 0x02
#define SX1262_CMD_GET_RX_BUFFER_STATUS 0x13
#define SX1262_CMD_GET_PACKET_STATUS 0x14
#define SX1262_CMD_GET_STATUS 0xC0
#define SX1262_CMD_SET_REGULATOR_MODE 0x96
#define SX1262_CMD_CALIBRATE 0x89
#define SX1262_CMD_SET_DIO2_AS_RF_SWITCH_CTRL 0x9D
#define SX1262_CMD_WRITE_REGISTER 0x0D
#define SX1262_CMD_READ_REGISTER 0x1D

// Standby modes
#define SX1262_STDBY_RC 0x00
#define SX1262_STDBY_XOSC 0x01

// Packet types
#define SX1262_PACKET_TYPE_LORA 0x01

// Regulator modes
#define SX1262_REGULATOR_LDO 0x00
#define SX1262_REGULATOR_DC_DC 0x01

// IRQ flags (16-bit)
#define SX1262_IRQ_TX_DONE (1 << 0)
#define SX1262_IRQ_RX_DONE (1 << 1)
#define SX1262_IRQ_PREAMBLE_DETECTED (1 << 2)
#define SX1262_IRQ_SYNC_WORD_VALID (1 << 3)
#define SX1262_IRQ_HEADER_VALID (1 << 4)
#define SX1262_IRQ_CRC_ERR (1 << 5)
#define SX1262_IRQ_HEADER_ERR (1 << 6)
#define SX1262_IRQ_CAD_DONE (1 << 7)
#define SX1262_IRQ_CAD_ACTIVITY_DETECTED (1 << 8)
#define SX1262_IRQ_TIMEOUT (1 << 9)
#define SX1262_IRQ_ALL 0x03FF

// TCXO voltages (DIO3)
#define SX1262_TCXO_1_6V 0x00
#define SX1262_TCXO_1_7V 0x01
#define SX1262_TCXO_1_8V 0x02
#define SX1262_TCXO_2_2V 0x03
#define SX1262_TCXO_2_4V 0x04
#define SX1262_TCXO_2_7V 0x05
#define SX1262_TCXO_3_0V 0x06
#define SX1262_TCXO_3_3V 0x07

// LoRa header types
#define SX1262_LORA_HEADER_EXPLICIT 0x00
#define SX1262_LORA_HEADER_IMPLICIT 0x01

// LoRa CRC types
#define SX1262_LORA_CRC_OFF 0x00
#define SX1262_LORA_CRC_ON 0x01

// LoRa IQ modes
#define SX1262_LORA_IQ_STANDARD 0x00
#define SX1262_LORA_IQ_INVERTED 0x01

// Ramp times for SetTxParams
#define SX1262_RAMP_10US 0x00
#define SX1262_RAMP_20US 0x01
#define SX1262_RAMP_40US 0x02
#define SX1262_RAMP_80US 0x03
#define SX1262_RAMP_200US 0x04
#define SX1262_RAMP_800US 0x05
#define SX1262_RAMP_1700US 0x06
#define SX1262_RAMP_3400US 0x07

// Register addresses (for direct register access)
#define SX1262_REG_RX_GAIN 0x08AC
#define SX1262_REG_OCP 0x08E7
#define SX1262_REG_SYNC_WORD_MSB 0x0740
#define SX1262_REG_SYNC_WORD_LSB 0x0741

// Default configuration values
#define SX1262_DEFAULT_FREQUENCY 915000000   // 915 MHz
#define SX1262_DEFAULT_TX_POWER 17           // 17 dBm
#define SX1262_DEFAULT_SPREADING_FACTOR 7    // SF7
#define SX1262_DEFAULT_BANDWIDTH 125000      // 125 kHz
#define SX1262_DEFAULT_CODING_RATE 5         // 4/5
#define SX1262_DEFAULT_PREAMBLE_LENGTH 8     // 8 symbols
#define SX1262_DEFAULT_CRC true              // CRC enabled

// Timing constants
#define SX1262_BUSY_TIMEOUT_MS 10    // Max wait for BUSY to go low
#define SX1262_RESET_DELAY_MS 5      // Delay after reset pulse

/**
 * @brief SX1262 LoRa transceiver driver for Raspberry Pi Pico
 *
 * Implements RadioInterface for the SX1262 (used on NiceRF LORA1262-915TCXO).
 * Key differences from SX1276: command-based SPI, BUSY pin, DIO1 interrupts,
 * TCXO control via DIO3.
 */
class SX1262 : public RadioInterface {
public:
    /**
     * @brief Construct SX1262 driver
     * @param spi_port SPI port (spi0 or spi1)
     * @param cs_pin Chip select GPIO pin
     * @param rst_pin Reset GPIO pin
     * @param dio1_pin DIO1 interrupt pin
     * @param busy_pin BUSY status pin
     */
    SX1262(spi_inst_t *spi_port, uint cs_pin, int rst_pin, int dio1_pin, int busy_pin);

    // --- RadioInterface implementation ---

    bool begin() override;
    bool isConnected() override;
    void reset() override;

    void setTxPower(int power_db) override;
    void setFrequency(uint32_t frequency_hz) override;
    void setSpreadingFactor(int sf) override;
    void setBandwidth(uint32_t bandwidth_hz) override;
    void setCodingRate(int denominator) override;
    void setPreambleLength(int length) override;
    void setCrc(bool enable_crc) override;

    bool send(const uint8_t *data, size_t length) override;
    bool sendAsync(const uint8_t *data, size_t length) override;
    bool isTxDone() override;

    int receive(uint8_t *buffer, size_t max_length) override;
    bool available() override;
    void startReceive() override;

    int getRssi() override;
    float getSnr() override;

    void sleep() override;
    void wakeup() override;

    bool enableInterruptMode(gpio_irq_callback_t callback = nullptr) override;
    void disableInterruptMode() override;
    bool isInterruptPending() const override { return interrupt_pending_; }
    uint8_t handleInterrupt() override;
    bool checkForMissedRxInterrupt() override;
    bool isMessageReady() const override { return message_ready_; }
    bool isTxComplete() const override { return tx_complete_; }
    void clearInterruptFlags() override;

    // SX1262-specific (not in RadioInterface)
    int getDio1Pin() const { return dio1_pin_; }
    void setInterruptPending() { interrupt_pending_ = true; }

private:
    SPIDevice spi_;
    int rst_pin_;
    int dio1_pin_;
    int busy_pin_;

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

    // Last received packet info
    int last_rssi_;
    float last_snr_;

    /**
     * @brief Wait for BUSY pin to go LOW
     * @param timeout_ms Maximum wait time in milliseconds
     * @return true if BUSY went low, false on timeout
     */
    bool waitBusy(uint32_t timeout_ms = SX1262_BUSY_TIMEOUT_MS);

    /**
     * @brief Send a command with no response data
     * @param opcode Command opcode
     * @param params Parameter bytes (can be nullptr)
     * @param param_count Number of parameter bytes
     */
    void sendCommand(uint8_t opcode, const uint8_t *params = nullptr, size_t param_count = 0);

    /**
     * @brief Send a command and read response data
     * @param opcode Command opcode
     * @param params Parameter bytes for command (can be nullptr)
     * @param param_count Number of parameter bytes
     * @param response Buffer to store response
     * @param response_count Number of response bytes to read
     */
    void readCommand(uint8_t opcode, const uint8_t *params, size_t param_count, uint8_t *response,
                     size_t response_count);

    /**
     * @brief Write data to the TX buffer
     * @param offset Buffer offset
     * @param data Data to write
     * @param length Number of bytes
     */
    void writeBuffer(uint8_t offset, const uint8_t *data, size_t length);

    /**
     * @brief Read data from the RX buffer
     * @param offset Buffer offset
     * @param data Buffer to store read data
     * @param length Number of bytes to read
     */
    void readBuffer(uint8_t offset, uint8_t *data, size_t length);

    /**
     * @brief Write to an SX1262 register
     * @param address 16-bit register address
     * @param value Byte to write
     */
    void writeRegister(uint16_t address, uint8_t value);

    /**
     * @brief Read from an SX1262 register
     * @param address 16-bit register address
     * @return Register value
     */
    uint8_t readRegister(uint16_t address);

    /**
     * @brief Get current IRQ status
     * @return 16-bit IRQ flags
     */
    uint16_t getIrqStatus();

    /**
     * @brief Clear specified IRQ flags
     * @param flags Bitmask of flags to clear
     */
    void clearIrqStatus(uint16_t flags);

    /**
     * @brief Configure DIO1 IRQ mapping
     * @param irq_mask IRQ sources to enable
     * @param dio1_mask IRQ sources to route to DIO1
     */
    void setDioIrqParams(uint16_t irq_mask, uint16_t dio1_mask);

    /**
     * @brief Configure modulation parameters
     */
    void configureModulation();

    /**
     * @brief Configure packet parameters
     * @param payload_length Payload length for implicit header mode (ignored for explicit)
     */
    void configurePacketParams(uint8_t payload_length = 255);

    /**
     * @brief Get bandwidth register value from Hz
     * @param bandwidth_hz Bandwidth in Hz
     * @return SX1262 bandwidth code
     */
    uint8_t getBandwidthCode(uint32_t bandwidth_hz);

    /**
     * @brief Set LoRa sync word for public/private network
     * @param sync_word 16-bit sync word (0x3444 = public, 0x1424 = private)
     */
    void setSyncWord(uint16_t sync_word);
};
