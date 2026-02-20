#include "sx1276.h"

#include <string.h>

#include "hardware/gpio.h"

#include "../hal/gpio_interrupt_manager.h"

// Global pointer for interrupt handler (single radio instance)
static SX1276 *g_sx1276_instance = nullptr;

SX1276::SX1276(spi_inst_t *spi_port, uint cs_pin, int rst_pin, int dio0_pin)
    : spi_(spi_port, cs_pin), rst_pin_(rst_pin), dio0_pin_(dio0_pin),
      frequency_(SX1276_DEFAULT_FREQUENCY), tx_power_(SX1276_DEFAULT_TX_POWER),
      spreading_factor_(SX1276_DEFAULT_SPREADING_FACTOR), bandwidth_(SX1276_DEFAULT_BANDWIDTH),
      coding_rate_(SX1276_DEFAULT_CODING_RATE), preamble_length_(SX1276_DEFAULT_PREAMBLE_LENGTH),
      crc_enabled_(SX1276_DEFAULT_CRC), logger_("SX1276"), interrupt_pending_(false),
      message_ready_(false), tx_complete_(false), interrupt_enabled_(false), user_callback_(nullptr)
{
    // Configure reset pin if provided
    if (rst_pin_ >= 0) {
        gpio_init(rst_pin_);
        gpio_set_dir(rst_pin_, GPIO_OUT);
        gpio_put(rst_pin_, 1);  // Not in reset
    }

    // Configure DIO0 pin if provided
    if (dio0_pin_ >= 0) {
        gpio_init(dio0_pin_);
        gpio_set_dir(dio0_pin_, GPIO_IN);
        gpio_pull_down(dio0_pin_);
    }
}

bool SX1276::begin()
{
    logger_.debug("begin() - Starting initialization, RST pin: %d, DIO0 pin: %d", rst_pin_,
                  dio0_pin_);

    // Reset module if reset pin connected
    if (rst_pin_ >= 0) {
        logger_.debug("Resetting module (RST pin %d)...", rst_pin_);
        reset();
        sleep_ms(10);
        logger_.debug("Reset complete");
    }

    // Check if module is responding
    uint8_t version = readRegister(SX1276_REG_VERSION);
    logger_.debug("VERSION register (0x42) = 0x%02X (expected 0x12 for SX1276/RFM95W)", version);

    if (version != 0x12) {
        logger_.error("Failed to detect module! VERSION=0x%02X", version);
        // Read a few more registers for debugging
        uint8_t opmode = readRegister(SX1276_REG_OP_MODE);
        uint8_t paconfig = readRegister(SX1276_REG_PA_CONFIG);
        logger_.error("  OP_MODE=0x%02X, PA_CONFIG=0x%02X", opmode, paconfig);
        logger_.error("  If all 0x00 or 0xFF: check SPI wiring/power");
        return false;
    }

    // Put in sleep mode first
    setMode(SX1276_MODE_SLEEP);
    sleep_ms(10);

    // Enable LoRa mode
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_SLEEP);
    sleep_ms(10);

    // Configure LoRa parameters
    configureLoRa();

    // Set to standby mode
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_STDBY);
    sleep_ms(10);

    // Clear any stale interrupt flags from power-on or previous boot
    writeRegister(SX1276_REG_IRQ_FLAGS, 0xFF);

    logger_.info("Initialized successfully");
    return true;
}

bool SX1276::isConnected()
{
    uint8_t version = readRegister(SX1276_REG_VERSION);
    return version == 0x12;  // SX1276 version register should read 0x12
}

void SX1276::setTxPower(int power_db)
{
    tx_power_ = power_db;

    if (power_db < 2)
        power_db = 2;
    if (power_db > 20)
        power_db = 20;

    // Always use PA_BOOST — the Feather RP2040 RFM95W antenna is wired to PA_BOOST, not RFO.
    // PA_BOOST formula: Pout = 2 + OutputPower [dBm] (normal mode, PA_DAC disabled)
    //                   Pout = 5 + OutputPower [dBm] (high power mode, PA_DAC enabled)
    if (power_db > 17) {
        writeRegister(SX1276_REG_PA_DAC, 0x87);  // Enable high power mode (+20 dBm)
        // OutputPower = power_db - 5 (high power formula)
        writeRegister(SX1276_REG_PA_CONFIG,
                      SX1276_PA_SELECT_PA_BOOST | 0x70 | (power_db - 5));
    } else {
        writeRegister(SX1276_REG_PA_DAC, 0x84);  // Normal PA_BOOST mode
        // OutputPower = power_db - 2 (normal formula)
        writeRegister(SX1276_REG_PA_CONFIG,
                      SX1276_PA_SELECT_PA_BOOST | 0x70 | (power_db - 2));
    }
}

void SX1276::setFrequency(uint32_t frequency_hz)
{
    frequency_ = frequency_hz;

    // Calculate frequency register value
    // Frf = (Frf * 32MHz) / (2^19)
    uint64_t frf = ((uint64_t)frequency_hz << 19) / 32000000;

    writeRegister(SX1276_REG_FRF_MSB, (uint8_t)(frf >> 16));
    writeRegister(SX1276_REG_FRF_MID, (uint8_t)(frf >> 8));
    writeRegister(SX1276_REG_FRF_LSB, (uint8_t)(frf >> 0));
}

void SX1276::setSpreadingFactor(int sf)
{
    if (sf < 6)
        sf = 6;
    if (sf > 12)
        sf = 12;

    spreading_factor_ = sf;

    uint8_t config2 = readRegister(SX1276_REG_MODEM_CONFIG_2);
    config2 = (config2 & 0x0F) | ((sf << 4) & 0xF0);
    writeRegister(SX1276_REG_MODEM_CONFIG_2, config2);

    // Enable/disable low data rate optimization for SF11 and SF12
    uint8_t config3 = readRegister(SX1276_REG_MODEM_CONFIG_3);
    if (sf >= 11) {
        config3 |= 0x08;  // Enable low data rate optimization
    } else {
        config3 &= ~0x08;  // Disable low data rate optimization
    }
    writeRegister(SX1276_REG_MODEM_CONFIG_3, config3);
}

void SX1276::setBandwidth(uint32_t bandwidth_hz)
{
    bandwidth_ = bandwidth_hz;

    uint8_t bw_reg = getBandwidthRegValue(bandwidth_hz);
    uint8_t config1 = readRegister(SX1276_REG_MODEM_CONFIG_1);
    config1 = (config1 & 0x0F) | (bw_reg << 4);
    writeRegister(SX1276_REG_MODEM_CONFIG_1, config1);
}

void SX1276::setCodingRate(int denominator)
{
    if (denominator < 5)
        denominator = 5;
    if (denominator > 8)
        denominator = 8;

    coding_rate_ = denominator;

    uint8_t cr = denominator - 4;
    uint8_t config1 = readRegister(SX1276_REG_MODEM_CONFIG_1);
    config1 = (config1 & 0xF1) | ((cr << 1) & 0x0E);
    writeRegister(SX1276_REG_MODEM_CONFIG_1, config1);
}

void SX1276::setPreambleLength(int length)
{
    if (length < 6)
        length = 6;
    if (length > 65535)
        length = 65535;

    preamble_length_ = length;

    writeRegister(SX1276_REG_PREAMBLE_MSB, (uint8_t)(length >> 8));
    writeRegister(SX1276_REG_PREAMBLE_LSB, (uint8_t)(length & 0xFF));
}

void SX1276::setCrc(bool enable_crc)
{
    crc_enabled_ = enable_crc;

    uint8_t config2 = readRegister(SX1276_REG_MODEM_CONFIG_2);
    if (enable_crc) {
        config2 |= 0x04;
    } else {
        config2 &= ~0x04;
    }
    writeRegister(SX1276_REG_MODEM_CONFIG_2, config2);
}

bool SX1276::send(const uint8_t *data, size_t length)
{
    if (length > 255) {
        return false;
    }

    // Clear TX complete flag (prevents false positive from previous transmission)
    tx_complete_ = false;

    // Put in standby mode
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_STDBY);

    // Clear IRQ flags
    writeRegister(SX1276_REG_IRQ_FLAGS, 0xFF);

    // Set FIFO TX base address
    writeRegister(SX1276_REG_FIFO_ADDR_PTR, 0x80);
    writeRegister(SX1276_REG_FIFO_TX_BASE_ADDR, 0x80);

    // Write payload length
    writeRegister(SX1276_REG_PAYLOAD_LENGTH, length);

    // Write payload to FIFO using bulk transfer
    uint8_t tx_buf[256];  // Max LoRa packet size + 1 for register
    SPIError result = spi_.writeBuffer(SX1276_REG_FIFO, data, length, tx_buf);
    if (result != SPI_SUCCESS) {
        logger_.error("Failed to write FIFO");
        return false;
    }

    // Switch DIO0 mapping to TxDone (01) before entering TX mode
    uint8_t dio_mapping1 = readRegister(SX1276_REG_DIO_MAPPING_1);
    dio_mapping1 &= 0x3F;                // Clear DIO0 bits [7:6]
    dio_mapping1 |= SX1276_DIO0_TXDONE;  // Set mapping to 01 (TxDone)
    writeRegister(SX1276_REG_DIO_MAPPING_1, dio_mapping1);

    // Start transmission
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_TX);
    logger_.debug("Started transmission, waiting for TX done interrupt");

    return true;
}

bool SX1276::sendAsync(const uint8_t *data, size_t length)
{
    if (!interrupt_enabled_) {
        logger_.error("Async send requires interrupt mode");
        return false;
    }

    if (length > 255) {
        return false;
    }

    // Clear TX complete flag
    tx_complete_ = false;

    // Put in standby mode
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_STDBY);

    // Clear IRQ flags
    writeRegister(SX1276_REG_IRQ_FLAGS, 0xFF);

    // Set FIFO TX base address
    writeRegister(SX1276_REG_FIFO_ADDR_PTR, 0x80);
    writeRegister(SX1276_REG_FIFO_TX_BASE_ADDR, 0x80);

    // Write payload length
    writeRegister(SX1276_REG_PAYLOAD_LENGTH, length);

    // Write payload to FIFO using bulk transfer
    uint8_t tx_buf[256];  // Max LoRa packet size + 1 for register
    SPIError result = spi_.writeBuffer(SX1276_REG_FIFO, data, length, tx_buf);
    if (result != SPI_SUCCESS) {
        logger_.error("Failed to write FIFO");
        return false;
    }

    // Switch DIO0 mapping to TxDone (01) before entering TX mode
    uint8_t dio_mapping1 = readRegister(SX1276_REG_DIO_MAPPING_1);
    dio_mapping1 &= 0x3F;                // Clear DIO0 bits [7:6]
    dio_mapping1 |= SX1276_DIO0_TXDONE;  // Set mapping to 01 (TxDone)
    writeRegister(SX1276_REG_DIO_MAPPING_1, dio_mapping1);

    // Start transmission - interrupt will fire when done
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_TX);

    return true;
}

bool SX1276::isTxDone()
{
    if (interrupt_enabled_) {
        // Process any pending interrupt first — the ISR sets interrupt_pending_,
        // but handleInterrupt() must run to translate that into tx_complete_.
        if (interrupt_pending_) {
            handleInterrupt();
        }

        if (tx_complete_) {
            return true;
        }

        // Fallback: check register directly in case interrupt was truly missed
        uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
        if (irq_flags & SX1276_IRQ_TX_DONE_MASK) {
            logger_.warn("TX done flag set in register but interrupt was missed!");
            // Clear TX done flag
            writeRegister(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_TX_DONE_MASK);
            tx_complete_ = true;
            return true;
        }
        return false;
    } else {
        // In polling mode, check register
        uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
        if (irq_flags & SX1276_IRQ_TX_DONE_MASK) {
            // Clear TX done flag
            writeRegister(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_TX_DONE_MASK);
            return true;
        }
        return false;
    }
}

int SX1276::receive(uint8_t *buffer, size_t max_length)
{
    // In interrupt mode, check if message is ready
    if (interrupt_enabled_ && !message_ready_) {
        return 0;  // No packet ready
    }

    uint8_t irq_flags;

    if (interrupt_enabled_) {
        // Interrupt already cleared the flags, just check message_ready
        message_ready_ = false;  // Clear for next message
    } else {
        // Polling mode - check flags directly
        irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);

        if (!(irq_flags & SX1276_IRQ_RX_DONE_MASK)) {
            return 0;  // No packet received
        }

        // Clear RX done flag
        writeRegister(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_RX_DONE_MASK);

        // Check for CRC error
        if (irq_flags & 0x20) {  // CRC error flag
            writeRegister(SX1276_REG_IRQ_FLAGS, 0x20);
            return -1;  // CRC error
        }
    }

    // Get packet length
    uint8_t packet_length = readRegister(SX1276_REG_RX_NB_BYTES);
    if (packet_length > max_length) {
        return -1;  // Buffer too small
    }

    // Set FIFO address pointer to start of received packet
    uint8_t fifo_addr = readRegister(SX1276_REG_FIFO_RX_CURRENT_ADDR);
    writeRegister(SX1276_REG_FIFO_ADDR_PTR, fifo_addr);

    // Read packet data from FIFO using bulk transfer
    uint8_t tx_buf[256];  // Max LoRa packet size + 1 for register
    uint8_t rx_buf[256];
    SPIError result = spi_.readBuffer(SX1276_REG_FIFO, buffer, packet_length, tx_buf, rx_buf);
    if (result != SPI_SUCCESS) {
        logger_.error("Failed to read FIFO");
        return -1;
    }

    return packet_length;
}

bool SX1276::available()
{
    if (interrupt_enabled_) {
        return message_ready_;
    } else {
        uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
        return (irq_flags & SX1276_IRQ_RX_DONE_MASK) != 0;
    }
}

int SX1276::getRssi()
{
    uint8_t rssi_reg = readRegister(SX1276_REG_PKT_RSSI_VALUE);

    if (frequency_ < 868000000) {
        return -164 + rssi_reg;  // LF port
    } else {
        return -157 + rssi_reg;  // HF port
    }
}

float SX1276::getSnr()
{
    uint8_t snr_reg = readRegister(SX1276_REG_PKT_SNR_VALUE);
    return ((int8_t)snr_reg) * 0.25f;
}

void SX1276::sleep()
{
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_SLEEP);
}

void SX1276::wakeup()
{
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_STDBY);
}

void SX1276::startReceive()
{
    // Clear IRQ flags
    writeRegister(SX1276_REG_IRQ_FLAGS, 0xFF);

    // Set FIFO RX base address
    writeRegister(SX1276_REG_FIFO_RX_BASE_ADDR, 0x00);
    writeRegister(SX1276_REG_FIFO_ADDR_PTR, 0x00);

    // Switch DIO0 mapping to RxDone (00) before entering RX mode
    uint8_t dio_mapping1 = readRegister(SX1276_REG_DIO_MAPPING_1);
    dio_mapping1 &= 0x3F;  // Clear DIO0 bits [7:6], leaving 00 = RxDone
    writeRegister(SX1276_REG_DIO_MAPPING_1, dio_mapping1);

    // Start continuous receive
    setMode(SX1276_MODE_LONG_RANGE_MODE | SX1276_MODE_RXCONTINUOUS);
}

void SX1276::reset()
{
    if (rst_pin_ >= 0) {
        gpio_put(rst_pin_, 0);
        sleep_ms(1);
        gpio_put(rst_pin_, 1);
        sleep_ms(5);
    }
}

void SX1276::writeRegister(uint8_t reg, uint8_t value)
{
    SPIError result = spi_.writeRegister(reg, value);
    if (result != SPI_SUCCESS) {
        logger_.error("Failed to write register 0x%02X", reg);
    }
}

uint8_t SX1276::readRegister(uint8_t reg)
{
    uint8_t value = 0;
    SPIError result = spi_.readRegister(reg, &value);
    if (result != SPI_SUCCESS) {
        logger_.error("Failed to read register 0x%02X", reg);
    }
    return value;
}

void SX1276::setMode(uint8_t mode)
{
    writeRegister(SX1276_REG_OP_MODE, mode);
}

uint8_t SX1276::getMode()
{
    return readRegister(SX1276_REG_OP_MODE);
}

void SX1276::configureLoRa()
{
    // Set default frequency
    setFrequency(frequency_);

    // Set default TX power
    setTxPower(tx_power_);

    // Set default spreading factor
    setSpreadingFactor(spreading_factor_);

    // Set default bandwidth
    setBandwidth(bandwidth_);

    // Set default coding rate
    setCodingRate(coding_rate_);

    // Set default preamble length
    setPreambleLength(preamble_length_);

    // Set default CRC
    setCrc(crc_enabled_);

    // Set explicit header mode
    uint8_t config1 = readRegister(SX1276_REG_MODEM_CONFIG_1);
    config1 &= ~0x01;  // Explicit header mode
    writeRegister(SX1276_REG_MODEM_CONFIG_1, config1);

    // Set RX timeout to max
    writeRegister(SX1276_REG_SYMB_TIMEOUT_LSB, 0xFF);

    // Set max payload length
    writeRegister(SX1276_REG_MAX_PAYLOAD_LENGTH, 0xFF);
}

uint8_t SX1276::getBandwidthRegValue(uint32_t bandwidth_hz)
{
    switch (bandwidth_hz) {
        case 7800:
            return 0x00;
        case 10400:
            return 0x01;
        case 15600:
            return 0x02;
        case 20800:
            return 0x03;
        case 31250:
            return 0x04;
        case 41700:
            return 0x05;
        case 62500:
            return 0x06;
        case 125000:
            return 0x07;
        case 250000:
            return 0x08;
        case 500000:
            return 0x09;
        default:
            return 0x07;  // Default to 125kHz
    }
}

bool SX1276::waitForModeReady(uint8_t target_mode, uint32_t timeout_ms)
{
    uint32_t start_time = to_ms_since_boot(get_absolute_time());

    // Mask out the low power mode bit for comparison
    uint8_t mode_mask = 0x07;
    uint8_t target = target_mode & mode_mask;

    while (true) {
        uint8_t current_mode = readRegister(SX1276_REG_OP_MODE);

        if ((current_mode & mode_mask) == target) {
            return true;
        }

        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_time;
        if (elapsed >= timeout_ms) {
            logger_.error("Mode change timeout: target=0x%02X, current=0x%02X, elapsed=%lu ms",
                          target_mode, current_mode, elapsed);
            return false;
        }

        sleep_ms(1);
    }
}

bool SX1276::enableInterruptMode(gpio_irq_callback_t callback)
{
    if (dio0_pin_ < 0) {
        logger_.error("DIO0 pin not configured");
        return false;
    }

    // Store callback if provided
    user_callback_ = callback;

    // Configure DIO0 default mapping for RxDone (mapping = 00)
    // Note: TxDone requires mapping 01, set dynamically in send()/sendAsync()
    uint8_t dio_mapping1 = readRegister(SX1276_REG_DIO_MAPPING_1);
    dio_mapping1 &= 0x3F;  // Clear DIO0 mapping bits [7:6], leaving 00 = RxDone
    writeRegister(SX1276_REG_DIO_MAPPING_1, dio_mapping1);

    // Verify the mapping was set correctly
    uint8_t verify = readRegister(SX1276_REG_DIO_MAPPING_1);
    logger_.debug("DIO mapping register set to 0x%02X", verify);

    // Ensure interrupts are not masked - enable all interrupts
    writeRegister(SX1276_REG_IRQ_FLAGS_MASK, 0x00);  // 0x00 = all interrupts enabled
    uint8_t mask_verify = readRegister(SX1276_REG_IRQ_FLAGS_MASK);
    logger_.debug("IRQ mask register set to 0x%02X (0x00 = all enabled)", mask_verify);

    // Set up global instance pointer for ISR
    g_sx1276_instance = this;

    // Register interrupt handler with the global manager
    GpioInterruptManager::getInstance().registerHandler(
        dio0_pin_, GPIO_IRQ_EDGE_RISE, [this](uint gpio, uint32_t events) {
            if (gpio == dio0_pin_ && (events & GPIO_IRQ_EDGE_RISE)) {
                setInterruptPending();
            }
        });

    interrupt_enabled_ = true;
    interrupt_pending_ = false;
    message_ready_ = false;
    tx_complete_ = false;

    logger_.debug("Interrupt mode enabled on DIO0 (GPIO %d)", dio0_pin_);
    return true;
}

void SX1276::disableInterruptMode()
{
    if (dio0_pin_ >= 0 && interrupt_enabled_) {
        GpioInterruptManager::getInstance().unregisterHandler(dio0_pin_);
        interrupt_enabled_ = false;
        g_sx1276_instance = nullptr;
        logger_.info("Interrupt mode disabled");
    }
}

uint8_t SX1276::handleInterrupt()
{
    if (!interrupt_pending_) {
        return 0;
    }

    // Clear pending flag
    interrupt_pending_ = false;

    // Read interrupt flags
    uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
    logger_.debug("Interrupt fired, flags=0x%02X", irq_flags);

    // Clear all interrupt flags by writing them back
    writeRegister(SX1276_REG_IRQ_FLAGS, irq_flags);

    // Process interrupt flags
    if (irq_flags & SX1276_IRQ_RX_DONE_MASK) {
        // Check for CRC error
        if (irq_flags & 0x20) {  // CRC error flag
            logger_.warn("RX CRC error");
            message_ready_ = false;
        } else {
            message_ready_ = true;
            logger_.debug("RX done, message ready");
        }
    }

    if (irq_flags & SX1276_IRQ_TX_DONE_MASK) {
        tx_complete_ = true;
        logger_.debug("TX done interrupt received!");
        // Note: caller (sendMessage) handles returning to RX mode via startReceive()
        // to avoid double startReceive() which clears IRQ flags and can lose RX packets
    }

    // Call user callback if provided
    if (user_callback_) {
        user_callback_(dio0_pin_, GPIO_IRQ_EDGE_RISE);
    }

    return irq_flags;
}

void SX1276::clearInterruptFlags()
{
    message_ready_ = false;
    tx_complete_ = false;
    interrupt_pending_ = false;
}

bool SX1276::checkForMissedRxInterrupt()
{
    if (!interrupt_enabled_ || message_ready_) {
        return false;
    }

    // Process any pending interrupt first — the ISR may have fired
    // but handleInterrupt() hasn't been called yet
    if (interrupt_pending_) {
        handleInterrupt();
        if (message_ready_) {
            return true;
        }
    }

    uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
    if (irq_flags & SX1276_IRQ_RX_DONE_MASK) {
        logger_.warn("RX done flag set in register but interrupt was missed!");
        // Process via handleInterrupt by setting the pending flag
        interrupt_pending_ = true;
        handleInterrupt();
        return true;
    }

    return false;
}