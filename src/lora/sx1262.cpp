#include "sx1262.h"

#include <string.h>

#include "hardware/gpio.h"

#include "../hal/gpio_interrupt_manager.h"

SX1262::SX1262(spi_inst_t *spi_port, uint cs_pin, int rst_pin, int dio1_pin, int busy_pin)
    : spi_(spi_port, cs_pin), rst_pin_(rst_pin), dio1_pin_(dio1_pin), busy_pin_(busy_pin),
      frequency_(SX1262_DEFAULT_FREQUENCY), tx_power_(SX1262_DEFAULT_TX_POWER),
      spreading_factor_(SX1262_DEFAULT_SPREADING_FACTOR), bandwidth_(SX1262_DEFAULT_BANDWIDTH),
      coding_rate_(SX1262_DEFAULT_CODING_RATE), preamble_length_(SX1262_DEFAULT_PREAMBLE_LENGTH),
      crc_enabled_(SX1262_DEFAULT_CRC), logger_("SX1262"), interrupt_pending_(false),
      message_ready_(false), tx_complete_(false), interrupt_enabled_(false), user_callback_(nullptr),
      last_rssi_(0), last_snr_(0.0f)
{
    // Configure reset pin
    if (rst_pin_ >= 0) {
        gpio_init(rst_pin_);
        gpio_set_dir(rst_pin_, GPIO_OUT);
        gpio_put(rst_pin_, 1);  // Not in reset
    }

    // Configure BUSY pin (input)
    if (busy_pin_ >= 0) {
        gpio_init(busy_pin_);
        gpio_set_dir(busy_pin_, GPIO_IN);
    }

    // Configure DIO1 pin (input with pull-down)
    if (dio1_pin_ >= 0) {
        gpio_init(dio1_pin_);
        gpio_set_dir(dio1_pin_, GPIO_IN);
        gpio_pull_down(dio1_pin_);
    }
}

bool SX1262::begin()
{
    logger_.debug("begin() - RST pin: %d, DIO1 pin: %d, BUSY pin: %d", rst_pin_, dio1_pin_,
                  busy_pin_);

    // Hardware reset
    if (rst_pin_ >= 0) {
        reset();
        sleep_ms(10);
    }

    // Wait for chip to be ready after reset
    if (!waitBusy(100)) {
        logger_.error("Chip not ready after reset (BUSY stuck high)");
        return false;
    }

    // 1. Set standby mode (RC oscillator)
    uint8_t standby_mode = SX1262_STDBY_RC;
    sendCommand(SX1262_CMD_SET_STANDBY, &standby_mode, 1);
    if (!waitBusy()) {
        logger_.error("Failed to enter standby");
        return false;
    }

    // 2. Configure DIO3 as TCXO control (3.3V, 5ms timeout)
    //    Timeout = value * 15.625 us; 320 * 15.625 = 5ms
    uint8_t tcxo_params[4] = {SX1262_TCXO_3_3V, 0x00, 0x01, 0x40};  // 3.3V, 320 = 0x000140
    sendCommand(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, tcxo_params, 4);
    if (!waitBusy()) {
        logger_.error("Failed to configure TCXO");
        return false;
    }

    // 3. Calibrate all blocks (after TCXO is running)
    uint8_t calib_param = 0x7F;  // Calibrate all
    sendCommand(SX1262_CMD_CALIBRATE, &calib_param, 1);
    if (!waitBusy(50)) {
        logger_.error("Calibration failed");
        return false;
    }

    // 4. Set regulator mode to DC-DC (more efficient)
    uint8_t reg_mode = SX1262_REGULATOR_DC_DC;
    sendCommand(SX1262_CMD_SET_REGULATOR_MODE, &reg_mode, 1);
    if (!waitBusy()) {
        logger_.error("Failed to set regulator mode");
        return false;
    }

    // 5. Set packet type to LoRa
    uint8_t packet_type = SX1262_PACKET_TYPE_LORA;
    sendCommand(SX1262_CMD_SET_PACKET_TYPE, &packet_type, 1);

    // 6. Set RF frequency
    setFrequency(frequency_);

    // 7. Set PA config for SX1262 (optimized for +22 dBm max)
    uint8_t pa_config[4] = {0x04, 0x07, 0x00, 0x01};  // paDutyCycle=4, hpMax=7, deviceSel=0 (SX1262), paLut=1
    sendCommand(SX1262_CMD_SET_PA_CONFIG, pa_config, 4);

    // 8. Set TX power and ramp time
    setTxPower(tx_power_);

    // 9. Set modulation parameters
    configureModulation();

    // 10. Set packet parameters
    configurePacketParams(255);

    // 11. Set sync word for private network (compatible with SX1276 sync word 0x12)
    setSyncWord(0x1424);

    // 12. Configure DIO1 IRQ mapping: TxDone + RxDone + CRC error + Timeout
    uint16_t irq_mask = SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR |
                        SX1262_IRQ_TIMEOUT;
    setDioIrqParams(irq_mask, irq_mask);

    // 13. Set buffer base addresses (TX at 0, RX at 128)
    uint8_t buf_base[2] = {0x00, 0x80};
    sendCommand(SX1262_CMD_SET_BUFFER_BASE_ADDRESS, buf_base, 2);

    // 14. Clear any stale IRQ flags
    clearIrqStatus(SX1262_IRQ_ALL);

    // Verify chip is responding by reading status
    if (!isConnected()) {
        logger_.error("Chip not responding after initialization");
        return false;
    }

    logger_.info("Initialized successfully");
    return true;
}

bool SX1262::isConnected()
{
    // SX1262 has no version register. Verify by reading status command.
    // GetStatus returns a status byte; chip mode should be STBY_RC (0x2) or similar.
    uint8_t status = 0;
    readCommand(SX1262_CMD_GET_STATUS, nullptr, 0, &status, 1);

    // Status byte format: bits [6:4] = chip mode, bits [3:1] = command status
    // Chip mode: 0x2 = STBY_RC, 0x3 = STBY_XOSC, 0x4 = FS, 0x5 = RX, 0x6 = TX
    // If we get 0x00 or 0xFF, the chip isn't responding
    uint8_t chip_mode = (status >> 4) & 0x07;
    return chip_mode >= 0x02 && chip_mode <= 0x06;
}

void SX1262::reset()
{
    if (rst_pin_ >= 0) {
        gpio_put(rst_pin_, 0);
        sleep_ms(1);
        gpio_put(rst_pin_, 1);
        sleep_ms(SX1262_RESET_DELAY_MS);
    }
}

void SX1262::setTxPower(int power_db)
{
    // SX1262 supports -9 to +22 dBm
    if (power_db < -9)
        power_db = -9;
    if (power_db > 22)
        power_db = 22;

    tx_power_ = power_db;

    // SetTxParams: power (signed), ramp time
    uint8_t params[2] = {(uint8_t)(int8_t)power_db, SX1262_RAMP_200US};
    sendCommand(SX1262_CMD_SET_TX_PARAMS, params, 2);
}

void SX1262::setFrequency(uint32_t frequency_hz)
{
    frequency_ = frequency_hz;

    // RF frequency = (freq_reg * F_XTAL) / 2^25
    // freq_reg = (frequency_hz * 2^25) / 32000000
    uint32_t freq_reg = (uint32_t)((uint64_t)frequency_hz * (1ULL << 25) / 32000000ULL);

    uint8_t params[4] = {(uint8_t)(freq_reg >> 24), (uint8_t)(freq_reg >> 16),
                         (uint8_t)(freq_reg >> 8), (uint8_t)(freq_reg)};
    sendCommand(SX1262_CMD_SET_RF_FREQUENCY, params, 4);
}

void SX1262::setSpreadingFactor(int sf)
{
    if (sf < 5)
        sf = 5;
    if (sf > 12)
        sf = 12;
    spreading_factor_ = sf;
    configureModulation();
}

void SX1262::setBandwidth(uint32_t bandwidth_hz)
{
    bandwidth_ = bandwidth_hz;
    configureModulation();
}

void SX1262::setCodingRate(int denominator)
{
    if (denominator < 5)
        denominator = 5;
    if (denominator > 8)
        denominator = 8;
    coding_rate_ = denominator;
    configureModulation();
}

void SX1262::setPreambleLength(int length)
{
    if (length < 1)
        length = 1;
    if (length > 65535)
        length = 65535;
    preamble_length_ = length;
    configurePacketParams(255);
}

void SX1262::setCrc(bool enable_crc)
{
    crc_enabled_ = enable_crc;
    configurePacketParams(255);
}

bool SX1262::send(const uint8_t *data, size_t length)
{
    if (length > 255) {
        return false;
    }

    // Clear TX complete flag
    tx_complete_ = false;

    // Switch to standby
    uint8_t standby_mode = SX1262_STDBY_RC;
    sendCommand(SX1262_CMD_SET_STANDBY, &standby_mode, 1);

    // Clear IRQ flags
    clearIrqStatus(SX1262_IRQ_ALL);

    // Set payload length in packet params
    configurePacketParams(length);

    // Write payload to TX buffer (offset 0)
    writeBuffer(0x00, data, length);

    // Set TX mode (timeout 0 = no timeout, TX until done)
    uint8_t tx_params[3] = {0x00, 0x00, 0x00};  // Timeout = 0 (wait for TxDone)
    sendCommand(SX1262_CMD_SET_TX, tx_params, 3);

    logger_.debug("Started transmission (%zu bytes)", length);
    return true;
}

bool SX1262::sendAsync(const uint8_t *data, size_t length)
{
    if (!interrupt_enabled_) {
        logger_.error("Async send requires interrupt mode");
        return false;
    }
    // sendAsync is the same as send for SX1262 — DIO1 fires when TX is done
    return send(data, length);
}

bool SX1262::isTxDone()
{
    if (interrupt_enabled_) {
        // Process pending interrupt first
        if (interrupt_pending_) {
            handleInterrupt();
        }

        if (tx_complete_) {
            return true;
        }

        // Fallback: poll IRQ register
        uint16_t irq = getIrqStatus();
        if (irq & SX1262_IRQ_TX_DONE) {
            logger_.warn("TX done in register but interrupt missed!");
            clearIrqStatus(SX1262_IRQ_TX_DONE);
            tx_complete_ = true;
            return true;
        }
        return false;
    } else {
        // Polling mode
        uint16_t irq = getIrqStatus();
        if (irq & SX1262_IRQ_TX_DONE) {
            clearIrqStatus(SX1262_IRQ_TX_DONE);
            return true;
        }
        return false;
    }
}

int SX1262::receive(uint8_t *buffer, size_t max_length)
{
    if (interrupt_enabled_ && !message_ready_) {
        return 0;
    }

    if (interrupt_enabled_) {
        message_ready_ = false;
    } else {
        uint16_t irq = getIrqStatus();
        if (!(irq & SX1262_IRQ_RX_DONE)) {
            return 0;
        }
        clearIrqStatus(SX1262_IRQ_RX_DONE);

        if (irq & SX1262_IRQ_CRC_ERR) {
            clearIrqStatus(SX1262_IRQ_CRC_ERR);
            return -1;
        }
    }

    // Get RX buffer status: payload length and start offset
    uint8_t rx_status[2] = {0};
    readCommand(SX1262_CMD_GET_RX_BUFFER_STATUS, nullptr, 0, rx_status, 2);
    uint8_t payload_length = rx_status[0];
    uint8_t rx_start_offset = rx_status[1];

    if (payload_length > max_length) {
        return -1;
    }

    // Read payload from buffer
    readBuffer(rx_start_offset, buffer, payload_length);

    // Get packet status for RSSI and SNR
    uint8_t pkt_status[3] = {0};
    readCommand(SX1262_CMD_GET_PACKET_STATUS, nullptr, 0, pkt_status, 3);
    // LoRa packet status: [0]=rssiPkt, [1]=snrPkt, [2]=signalRssiPkt
    last_rssi_ = -(int)pkt_status[0] / 2;
    last_snr_ = (int8_t)pkt_status[1] / 4.0f;

    return payload_length;
}

bool SX1262::available()
{
    if (interrupt_enabled_) {
        return message_ready_;
    } else {
        uint16_t irq = getIrqStatus();
        return (irq & SX1262_IRQ_RX_DONE) != 0;
    }
}

void SX1262::startReceive()
{
    // Clear IRQ flags
    clearIrqStatus(SX1262_IRQ_ALL);

    // Set RX mode with continuous receive (timeout = 0xFFFFFF)
    uint8_t rx_params[3] = {0xFF, 0xFF, 0xFF};
    sendCommand(SX1262_CMD_SET_RX, rx_params, 3);
}

int SX1262::getRssi()
{
    return last_rssi_;
}

float SX1262::getSnr()
{
    return last_snr_;
}

void SX1262::sleep()
{
    // SetSleep: bit 0 = warm start (retain config), bit 2 = RTC timeout disabled
    uint8_t sleep_config = 0x04;  // Warm start, no RTC
    sendCommand(SX1262_CMD_SET_SLEEP, &sleep_config, 1);
    // Note: after sleep command, do NOT wait for BUSY — chip is asleep
}

void SX1262::wakeup()
{
    // Any command wakes the SX1262. Send GetStatus to wake it.
    uint8_t status = 0;
    readCommand(SX1262_CMD_GET_STATUS, nullptr, 0, &status, 1);
    waitBusy();

    // Return to standby
    uint8_t standby_mode = SX1262_STDBY_RC;
    sendCommand(SX1262_CMD_SET_STANDBY, &standby_mode, 1);
}

bool SX1262::enableInterruptMode(gpio_irq_callback_t callback)
{
    if (dio1_pin_ < 0) {
        logger_.error("DIO1 pin not configured");
        return false;
    }

    user_callback_ = callback;

    // Configure IRQ mapping: TxDone + RxDone + CRC error + Timeout -> DIO1
    uint16_t irq_mask = SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR |
                        SX1262_IRQ_TIMEOUT;
    setDioIrqParams(irq_mask, irq_mask);

    // Register interrupt handler
    GpioInterruptManager::getInstance().registerHandler(
        dio1_pin_, GPIO_IRQ_EDGE_RISE, [this](uint gpio, uint32_t events) {
            if (gpio == (uint)dio1_pin_ && (events & GPIO_IRQ_EDGE_RISE)) {
                setInterruptPending();
            }
        });

    interrupt_enabled_ = true;
    interrupt_pending_ = false;
    message_ready_ = false;
    tx_complete_ = false;

    logger_.debug("Interrupt mode enabled on DIO1 (GPIO %d)", dio1_pin_);
    return true;
}

void SX1262::disableInterruptMode()
{
    if (dio1_pin_ >= 0 && interrupt_enabled_) {
        GpioInterruptManager::getInstance().unregisterHandler(dio1_pin_);
        interrupt_enabled_ = false;
        logger_.info("Interrupt mode disabled");
    }
}

uint8_t SX1262::handleInterrupt()
{
    if (!interrupt_pending_) {
        return 0;
    }

    interrupt_pending_ = false;

    uint16_t irq_flags = getIrqStatus();
    logger_.debug("Interrupt fired, flags=0x%04X", irq_flags);

    // Clear all active flags
    clearIrqStatus(irq_flags);

    if (irq_flags & SX1262_IRQ_RX_DONE) {
        if (irq_flags & SX1262_IRQ_CRC_ERR) {
            logger_.warn("RX CRC error");
            message_ready_ = false;
        } else {
            message_ready_ = true;
            logger_.debug("RX done, message ready");
        }
    }

    if (irq_flags & SX1262_IRQ_TX_DONE) {
        tx_complete_ = true;
        logger_.debug("TX done interrupt received!");
    }

    if (irq_flags & SX1262_IRQ_TIMEOUT) {
        logger_.debug("RX/TX timeout");
    }

    // Call user callback if provided
    if (user_callback_) {
        user_callback_(dio1_pin_, GPIO_IRQ_EDGE_RISE);
    }

    // Return lower 8 bits for compatibility with callers expecting uint8_t
    return (uint8_t)(irq_flags & 0xFF);
}

bool SX1262::checkForMissedRxInterrupt()
{
    if (!interrupt_enabled_ || message_ready_) {
        return false;
    }

    if (interrupt_pending_) {
        handleInterrupt();
        if (message_ready_) {
            return true;
        }
    }

    uint16_t irq = getIrqStatus();
    if (irq & SX1262_IRQ_RX_DONE) {
        logger_.warn("RX done flag set but interrupt was missed!");
        interrupt_pending_ = true;
        handleInterrupt();
        return true;
    }

    return false;
}

void SX1262::clearInterruptFlags()
{
    message_ready_ = false;
    tx_complete_ = false;
    interrupt_pending_ = false;
}

// --- Private methods ---

bool SX1262::waitBusy(uint32_t timeout_ms)
{
    if (busy_pin_ < 0) {
        // No BUSY pin — just wait a bit
        sleep_us(100);
        return true;
    }

    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (gpio_get(busy_pin_)) {
        if (to_ms_since_boot(get_absolute_time()) - start >= timeout_ms) {
            logger_.error("BUSY timeout (%lu ms)", timeout_ms);
            return false;
        }
        sleep_us(10);
    }
    return true;
}

void SX1262::sendCommand(uint8_t opcode, const uint8_t *params, size_t param_count)
{
    waitBusy();

    // Build TX buffer: opcode + params
    uint8_t tx_buf[16];
    tx_buf[0] = opcode;
    if (params && param_count > 0) {
        memcpy(&tx_buf[1], params, param_count);
    }

    spi_.transfer(tx_buf, nullptr, 1 + param_count);

    // Wait for command to complete (except sleep commands)
    if (opcode != SX1262_CMD_SET_SLEEP) {
        waitBusy();
    }
}

void SX1262::readCommand(uint8_t opcode, const uint8_t *params, size_t param_count,
                         uint8_t *response, size_t response_count)
{
    waitBusy();

    // TX: opcode + params + NOP byte + NOP bytes for response
    // RX: skip opcode + params + status byte, then read response
    size_t total_len = 1 + param_count + 1 + response_count;  // opcode + params + NOP + response
    uint8_t tx_buf[32] = {0};
    uint8_t rx_buf[32] = {0};

    tx_buf[0] = opcode;
    if (params && param_count > 0) {
        memcpy(&tx_buf[1], params, param_count);
    }
    // Remaining bytes are 0x00 (NOP) for clocking out response

    spi_.transfer(tx_buf, rx_buf, total_len);

    // Response starts after opcode + params + status byte
    size_t response_offset = 1 + param_count + 1;
    if (response && response_count > 0) {
        memcpy(response, &rx_buf[response_offset], response_count);
    }

    waitBusy();
}

void SX1262::writeBuffer(uint8_t offset, const uint8_t *data, size_t length)
{
    waitBusy();

    // WriteBuffer: opcode(0x0E) + offset + data bytes
    uint8_t tx_buf[258];  // 1 opcode + 1 offset + 256 max data
    tx_buf[0] = SX1262_CMD_WRITE_BUFFER;
    tx_buf[1] = offset;
    memcpy(&tx_buf[2], data, length);

    spi_.transfer(tx_buf, nullptr, 2 + length);

    waitBusy();
}

void SX1262::readBuffer(uint8_t offset, uint8_t *data, size_t length)
{
    waitBusy();

    // ReadBuffer: opcode(0x1E) + offset + NOP + data
    size_t total_len = 3 + length;  // opcode + offset + NOP + data
    uint8_t tx_buf[259] = {0};
    uint8_t rx_buf[259] = {0};

    tx_buf[0] = SX1262_CMD_READ_BUFFER;
    tx_buf[1] = offset;
    // tx_buf[2] = NOP, rest = NOP for clocking out data

    spi_.transfer(tx_buf, rx_buf, total_len);

    // Data starts at index 3 (after opcode + offset + status)
    memcpy(data, &rx_buf[3], length);

    waitBusy();
}

void SX1262::writeRegister(uint16_t address, uint8_t value)
{
    waitBusy();

    uint8_t tx_buf[4] = {SX1262_CMD_WRITE_REGISTER, (uint8_t)(address >> 8),
                         (uint8_t)(address & 0xFF), value};
    spi_.transfer(tx_buf, nullptr, 4);

    waitBusy();
}

uint8_t SX1262::readRegister(uint16_t address)
{
    waitBusy();

    // ReadRegister: opcode + addr_msb + addr_lsb + NOP + data
    uint8_t tx_buf[5] = {SX1262_CMD_READ_REGISTER, (uint8_t)(address >> 8),
                         (uint8_t)(address & 0xFF), 0x00, 0x00};
    uint8_t rx_buf[5] = {0};

    spi_.transfer(tx_buf, rx_buf, 5);

    waitBusy();
    return rx_buf[4];  // Data is in last byte
}

uint16_t SX1262::getIrqStatus()
{
    uint8_t status[2] = {0};
    readCommand(SX1262_CMD_GET_IRQ_STATUS, nullptr, 0, status, 2);
    return ((uint16_t)status[0] << 8) | status[1];
}

void SX1262::clearIrqStatus(uint16_t flags)
{
    uint8_t params[2] = {(uint8_t)(flags >> 8), (uint8_t)(flags & 0xFF)};
    sendCommand(SX1262_CMD_CLEAR_IRQ_STATUS, params, 2);
}

void SX1262::setDioIrqParams(uint16_t irq_mask, uint16_t dio1_mask)
{
    uint8_t params[8] = {
        (uint8_t)(irq_mask >> 8),  (uint8_t)(irq_mask & 0xFF),   // IRQ mask
        (uint8_t)(dio1_mask >> 8), (uint8_t)(dio1_mask & 0xFF),   // DIO1 mask
        0x00,                      0x00,                           // DIO2 mask (unused)
        0x00,                      0x00                            // DIO3 mask (unused)
    };
    sendCommand(SX1262_CMD_SET_DIO_IRQ_PARAMS, params, 8);
}

void SX1262::configureModulation()
{
    uint8_t bw_code = getBandwidthCode(bandwidth_);

    // Low data rate optimization: required for SF11/SF12 with 125kHz
    uint8_t ldro = 0;
    if (spreading_factor_ >= 11 && bandwidth_ <= 125000) {
        ldro = 1;
    }

    uint8_t params[4] = {(uint8_t)spreading_factor_, bw_code,
                         (uint8_t)(coding_rate_ - 4),  // CR 4/5 -> 0x01, 4/8 -> 0x04
                         ldro};
    sendCommand(SX1262_CMD_SET_MODULATION_PARAMS, params, 4);
}

void SX1262::configurePacketParams(uint8_t payload_length)
{
    uint8_t params[6] = {
        (uint8_t)(preamble_length_ >> 8),  // Preamble MSB
        (uint8_t)(preamble_length_),        // Preamble LSB
        SX1262_LORA_HEADER_EXPLICIT,        // Explicit header
        payload_length,                      // Payload length
        (uint8_t)(crc_enabled_ ? SX1262_LORA_CRC_ON : SX1262_LORA_CRC_OFF),
        SX1262_LORA_IQ_STANDARD             // Standard IQ
    };
    sendCommand(SX1262_CMD_SET_PACKET_PARAMS, params, 6);
}

uint8_t SX1262::getBandwidthCode(uint32_t bandwidth_hz)
{
    // SX1262 bandwidth codes (different from SX1276!)
    if (bandwidth_hz <= 7800)
        return 0x00;
    if (bandwidth_hz <= 10400)
        return 0x08;
    if (bandwidth_hz <= 15600)
        return 0x01;
    if (bandwidth_hz <= 20800)
        return 0x09;
    if (bandwidth_hz <= 31250)
        return 0x02;
    if (bandwidth_hz <= 41700)
        return 0x0A;
    if (bandwidth_hz <= 62500)
        return 0x03;
    if (bandwidth_hz <= 125000)
        return 0x04;
    if (bandwidth_hz <= 250000)
        return 0x05;
    return 0x06;  // 500 kHz
}

void SX1262::setSyncWord(uint16_t sync_word)
{
    writeRegister(SX1262_REG_SYNC_WORD_MSB, (uint8_t)(sync_word >> 8));
    writeRegister(SX1262_REG_SYNC_WORD_LSB, (uint8_t)(sync_word & 0xFF));
}
