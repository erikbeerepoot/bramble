#include "sx1262.h"

#include <string.h>

#include "hardware/gpio.h"

#include "../hal/gpio_interrupt_manager.h"

// Static instance pointer for raw ISR
SX1262 *SX1262::isr_instance_ = nullptr;

void SX1262::dio1Isr(uint gpio, uint32_t events)
{
    if (isr_instance_ && (events & GPIO_IRQ_EDGE_RISE)) {
        isr_instance_->isr_call_count_++;
        isr_instance_->interrupt_pending_ = true;
    }
}

SX1262::SX1262(spi_inst_t *spi_port, uint cs_pin, int rst_pin, int dio1_pin, int busy_pin)
    : spi_(spi_port, cs_pin), rst_pin_(rst_pin), dio1_pin_(dio1_pin), busy_pin_(busy_pin),
      frequency_(SX1262_DEFAULT_FREQUENCY), tx_power_(SX1262_DEFAULT_TX_POWER),
      spreading_factor_(SX1262_DEFAULT_SPREADING_FACTOR), bandwidth_(SX1262_DEFAULT_BANDWIDTH),
      coding_rate_(SX1262_DEFAULT_CODING_RATE), preamble_length_(SX1262_DEFAULT_PREAMBLE_LENGTH),
      crc_enabled_(SX1262_DEFAULT_CRC), logger_("SX1262"), interrupt_pending_(false),
      message_ready_(false), tx_complete_(false), interrupt_enabled_(false),
      user_callback_(nullptr), last_rssi_(0), last_snr_(0.0f)
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

    // Hardware reset with retry — chip can get stuck if previous session ended mid-TX
    bool chip_ready = false;
    for (int reset_attempt = 0; reset_attempt < 3 && !chip_ready; reset_attempt++) {
        if (reset_attempt > 0) {
            logger_.warn("Reset retry %d/3", reset_attempt + 1);
        }
        if (rst_pin_ >= 0) {
            // Aggressive reset: 5ms low pulse, 10ms recovery
            gpio_put(rst_pin_, 0);
            sleep_ms(5);
            gpio_put(rst_pin_, 1);
            sleep_ms(10);
        }

        if (waitBusy(100)) {
            chip_ready = true;
        } else {
            logger_.error("Chip not ready after reset (BUSY stuck high), attempt %d/3",
                          reset_attempt + 1);
        }
    }
    if (!chip_ready) {
        logger_.error("Chip unresponsive after 3 reset attempts");
        return false;
    }

    // 1. Set standby mode (RC oscillator)
    uint8_t standby_mode = SX1262_STDBY_RC;
    if (!sendCommand(SX1262_CMD_SET_STANDBY, &standby_mode, 1)) {
        logger_.error("Failed to enter standby");
        return false;
    }

    // 2. Configure DIO3 as TCXO control (3.3V, 10ms timeout)
    //    Timeout = value * 15.625 us; 640 * 15.625 = 10ms
    uint8_t tcxo_params[4] = {SX1262_TCXO_3_3V, 0x00, 0x02, 0x80};  // 3.3V, 640 = 0x000280
    if (!sendCommand(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, tcxo_params, 4)) {
        logger_.error("Failed to configure TCXO");
        return false;
    }

    // 3. Calibrate all blocks
    //    TCXO starts automatically during calibration. With TCXO delay (10ms) +
    //    calibration (~3.5ms * 7 blocks = ~25ms), total BUSY time can reach ~35ms.
    //    Use 100ms timeout for margin.
    uint8_t calib_param = 0x7F;  // Calibrate all
    if (!waitBusy()) {
        logger_.error("BUSY before calibration");
        return false;
    }
    {
        uint8_t tx_buf[2] = {SX1262_CMD_CALIBRATE, calib_param};
        spi_.transfer(tx_buf, nullptr, 2);
    }
    if (!waitBusy(100)) {  // 100ms timeout for TCXO startup + full calibration
        logger_.error("Calibration failed (BUSY timeout)");
        return false;
    }

    // 4. Set regulator mode to DC-DC (more efficient)
    uint8_t reg_mode = SX1262_REGULATOR_DC_DC;
    if (!sendCommand(SX1262_CMD_SET_REGULATOR_MODE, &reg_mode, 1)) {
        logger_.error("Failed to set regulator mode");
        return false;
    }

    // 5. Configure DIO2 as RF switch control
    //    The LoRa1262 module has an integrated antenna switch controlled by DIO2.
    //    Without this, no RF signal reaches the antenna.
    uint8_t rf_switch_enable = 0x01;
    if (!sendCommand(SX1262_CMD_SET_DIO2_AS_RF_SWITCH_CTRL, &rf_switch_enable, 1)) {
        logger_.error("Failed to configure DIO2 as RF switch");
        return false;
    }

    // Force back to known state after DIO2 config — it can leave the chip in RX/FS mode
    sleep_ms(5);
    standby_mode = SX1262_STDBY_RC;
    if (!sendCommand(SX1262_CMD_SET_STANDBY, &standby_mode, 1)) {
        logger_.error("Failed to return to standby after DIO2 config");
        return false;
    }

    // 6. Set packet type to LoRa
    uint8_t packet_type = SX1262_PACKET_TYPE_LORA;
    sendCommand(SX1262_CMD_SET_PACKET_TYPE, &packet_type, 1);

    // 6. Set RF frequency
    setFrequency(frequency_);

    // 6b. Calibrate image rejection for 902-928 MHz ISM band
    //     Must be done AFTER SetRfFrequency. The Calibrate(0x7F) above used the
    //     default frequency which is wrong for 915 MHz.
    {
        uint8_t cal_freq[2] = {0xE1, 0xE9};  // 902 MHz, 928 MHz
        sendCommand(SX1262_CMD_CALIBRATE_IMAGE, cal_freq, 2);
    }

    // 7. Set PA config for SX1262 (optimized for +22 dBm max)
    uint8_t pa_config[4] = {0x04, 0x07, 0x00,
                            0x01};  // paDutyCycle=4, hpMax=7, deviceSel=0 (SX1262), paLut=1
    sendCommand(SX1262_CMD_SET_PA_CONFIG, pa_config, 4);

    // 7b. Set OCP (Over-Current Protection) for high-power PA
    //     Default after reset is 60 mA — far too low for +22 dBm PA config.
    //     Set to 140 mA (0x38 * 2.5 mA) per Semtech reference design.
    uint8_t ocp_before = readRegister(SX1262_REG_OCP);
    writeRegister(SX1262_REG_OCP, 0x38);
    uint8_t ocp_after = readRegister(SX1262_REG_OCP);
    logger_.info("OCP: 0x%02X -> 0x%02X (expect 0x38)", ocp_before, ocp_after);

    // 7c. TxClampConfig workaround (datasheet section 15.2 errata)
    //     PA clamping threshold is "overly protective" by default, causing
    //     5-6 dB less output power. Fix: set register 0x08D8 bits [4:1] = 1111.
    uint8_t tx_clamp = readRegister(SX1262_REG_TX_CLAMP_CONFIG);
    tx_clamp |= 0x1E;  // Set bits 4:1 to 1111
    writeRegister(SX1262_REG_TX_CLAMP_CONFIG, tx_clamp);

    // 8. Set TX power and ramp time
    setTxPower(tx_power_);

    // 9. Set modulation parameters
    configureModulation();

    // 10. Set packet parameters
    configurePacketParams(255);

    // 11. Configure DIO1 IRQ mapping: TxDone + RxDone + CRC error + Timeout
    uint16_t irq_mask =
        SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR | SX1262_IRQ_TIMEOUT;
    setDioIrqParams(irq_mask, irq_mask);

    // 12. Set buffer base addresses (TX at 0, RX at 128)
    uint8_t buf_base[2] = {0x00, 0x80};
    sendCommand(SX1262_CMD_SET_BUFFER_BASE_ADDRESS, buf_base, 2);

    // 13. Enable RX boosted gain mode for maximum receive sensitivity
    //     Register 0x08AC defaults to 0x94 (power-saving RX). Writing 0x96
    //     enables boosted gain (~3dB better sensitivity, ~2mA extra during RX).
    //     Equivalent to the SX1276 LNA boost + AGC enable.
    writeRegister(SX1262_REG_RX_GAIN, 0x96);

    // 14. Clear any stale IRQ flags
    clearIrqStatus(SX1262_IRQ_ALL);

    // 15. Set sync word LAST — other commands (especially setDioIrqParams) can
    //     corrupt the sync word register. Verify and retry until correct.
    for (int sw_attempt = 0; sw_attempt < 5; sw_attempt++) {
        setSyncWord(0x1424);
        uint8_t sw_msb = readRegister(SX1262_REG_SYNC_WORD_MSB);
        uint8_t sw_lsb = readRegister(SX1262_REG_SYNC_WORD_LSB);
        if (sw_msb == 0x14 && sw_lsb == 0x24) {
            logger_.info("Sync word: 0x1424 (ok%s)", sw_attempt > 0 ? ", after retry" : "");
            break;
        }
        logger_.warn("Sync word mismatch: 0x%02X%02X (expected 0x1424), retry %d/5",
                     sw_msb, sw_lsb, sw_attempt + 1);
        sleep_us(200);
        if (sw_attempt == 4) {
            logger_.error("Sync word could not be set correctly after 5 attempts!");
        }
    }

    // Verify chip is responding by reading status
    if (!isConnected()) {
        logger_.error("Chip not responding after initialization");
        return false;
    }

    // Check and clear any device errors from calibration
    {
        uint8_t dev_errors[2] = {0};
        readCommand(SX1262_CMD_GET_DEVICE_ERRORS, nullptr, 0, dev_errors, 2);
        uint16_t errors = ((uint16_t)dev_errors[0] << 8) | dev_errors[1];
        if (errors) {
            logger_.warn("Device errors after init: 0x%04X (clearing)", errors);
            uint8_t clear[2] = {0x00, 0x00};
            sendCommand(SX1262_CMD_CLEAR_DEVICE_ERRORS, clear, 2);
        }
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
        sleep_ms(5);   // Datasheet min 100µs, use 5ms for margin
        gpio_put(rst_pin_, 1);
        sleep_ms(10);  // Wait for startup (BUSY goes low when ready)
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

    logger_.info("TX start: SF=%d, BW=%lu, %zu bytes", spreading_factor_, bandwidth_, length);

    // Switch to STDBY_XOSC so TCXO starts now (datasheet section 13.3.6).
    // Going directly to TX from STDBY_RC would hold BUSY for the full TCXO delay.
    uint8_t standby_mode = SX1262_STDBY_XOSC;
    if (!sendCommand(SX1262_CMD_SET_STANDBY, &standby_mode, 1)) {
        logger_.error("TX: Failed to enter standby — performing full recovery");
        reset();
        begin();
        startReceive();
        return false;
    }

    // Clear IRQ flags and verify they're cleared
    clearIrqStatus(SX1262_IRQ_ALL);
    uint16_t irq_after_clear = getIrqStatus();
    if (irq_after_clear & SX1262_IRQ_TX_DONE) {
        logger_.warn("TX: Stale TX_DONE after clear (irq=0x%04X), retrying clear", irq_after_clear);
        clearIrqStatus(SX1262_IRQ_ALL);
    }

    // Re-apply packet type + modulation params from STDBY
    // (datasheet: SetPacketType must precede SetModulationParams)
    uint8_t packet_type = SX1262_PACKET_TYPE_LORA;
    sendCommand(SX1262_CMD_SET_PACKET_TYPE, &packet_type, 1);
    configureModulation();

    // Re-apply sync word (can be corrupted by state transitions)
    setSyncWord(0x1424);

    // Re-apply PA config — SPI corruption can reset PA registers, causing
    // the chip to get stuck in FS mode (mode=4) instead of ramping to TX.
    // Must be done in STDBY before SetTx.
    uint8_t pa_config[4] = {0x04, 0x07, 0x00, 0x01};  // paDutyCycle=4, hpMax=7, SX1262, paLut=1
    sendCommand(SX1262_CMD_SET_PA_CONFIG, pa_config, 4);
    writeRegister(SX1262_REG_OCP, 0x38);  // 140mA OCP (default 60mA too low for +22dBm)

    // TxClampConfig errata fix (datasheet section 15.2)
    uint8_t tx_clamp = readRegister(SX1262_REG_TX_CLAMP_CONFIG);
    tx_clamp |= 0x1E;  // Set bits [4:1] = 1111
    writeRegister(SX1262_REG_TX_CLAMP_CONFIG, tx_clamp);

    // Re-apply TX power (depends on PA config being set first)
    setTxPower(tx_power_);

    // Set payload length in packet params
    configurePacketParams(length);

    // Write payload to TX buffer (offset 0)
    if (!writeBuffer(0x00, data, length)) {
        logger_.error("TX: Failed to write buffer (BUSY timeout) — performing full recovery");
        reset();
        begin();
        startReceive();
        return false;
    }

    // Re-apply DIO1 IRQ mapping — mode transitions can corrupt it
    {
        uint16_t irq_mask =
            SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR | SX1262_IRQ_TIMEOUT;
        setDioIrqParams(irq_mask, irq_mask);
    }

    // Start TX (timeout 0 = no timeout, TX until done)
    uint8_t tx_params[3] = {0x00, 0x00, 0x00};
    if (!sendCommand(SX1262_CMD_SET_TX, tx_params, 3)) {
        logger_.error("TX: SetTx BUSY timeout — performing full recovery");
        reset();
        begin();
        startReceive();
        return false;
    }

    // Verify chip entered TX mode (chip_mode: 2=STDBY_RC, 3=STDBY_XOSC, 4=FS, 5=RX, 6=TX)
    {
        uint8_t st = 0;
        readCommand(SX1262_CMD_GET_STATUS, nullptr, 0, &st, 1);
        uint8_t chip_mode = (st >> 4) & 0x07;

        if (chip_mode == 0x04) {
            // Stuck in FS — PA failed to ramp. Full recovery needed.
            uint8_t dev_errors[2] = {0};
            readCommand(SX1262_CMD_GET_DEVICE_ERRORS, nullptr, 0, dev_errors, 2);
            uint16_t errors = ((uint16_t)dev_errors[0] << 8) | dev_errors[1];
            logger_.error("TX: stuck in FS mode (errors=0x%04X) — performing full recovery", errors);
            reset();
            begin();
            startReceive();
            return false;
        } else if (chip_mode != 0x06) {
            // Not in TX and not FS — could be STDBY (TX completed instantly?) or unknown
            uint16_t irq = getIrqStatus();
            if (irq & SX1262_IRQ_TX_DONE) {
                logger_.info("TX: completed immediately (mode=%d, irq=0x%04X)", chip_mode, irq);
            } else {
                logger_.error("TX: unexpected mode=%d after SetTx (irq=0x%04X)", chip_mode, irq);
            }
        }
    }

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
            uint8_t status = 0;
            readCommand(SX1262_CMD_GET_STATUS, nullptr, 0, &status, 1);
            uint8_t chip_mode = (status >> 4) & 0x07;
            if (chip_mode == 0x06) {
                // Still in TX — TX_DONE is spurious (SPI read corruption)
                return false;
            }
            if (chip_mode == 0x04) {
                // Stuck in FS — PA never ramped, packet never sent.
                // Recover and report TX "done" so caller moves to ACK-wait
                // (which will timeout and retry with a healthy chip).
                logger_.error("TX_DONE with chip stuck in FS (mode=4) — packet not sent, recovering");
                clearIrqStatus(SX1262_IRQ_TX_DONE);
                reset();
                begin();
                startReceive();  // Back to RX so we can receive ACKs/packets
                tx_complete_ = true;
                return true;
            }
            logger_.info("TX done (polled, DIO1=%s, chip_mode=%d, isr_count=%lu)",
                         gpio_get(dio1_pin_) ? "HIGH" : "LOW", chip_mode, isr_call_count_);
            clearIrqStatus(SX1262_IRQ_TX_DONE);
            tx_complete_ = true;
            return true;
        }
        return false;
    } else {
        // Polling mode
        uint16_t irq = getIrqStatus();
        if (irq & SX1262_IRQ_TX_DONE) {
            uint8_t status = 0;
            readCommand(SX1262_CMD_GET_STATUS, nullptr, 0, &status, 1);
            uint8_t chip_mode = (status >> 4) & 0x07;
            if (chip_mode == 0x06) {
                return false;  // Spurious — still transmitting
            }
            if (chip_mode == 0x04) {
                clearIrqStatus(SX1262_IRQ_TX_DONE);
                reset();
                begin();
                startReceive();
                return true;
            }
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
    if (!readCommand(SX1262_CMD_GET_RX_BUFFER_STATUS, nullptr, 0, rx_status, 2)) {
        logger_.error("Failed to read RX buffer status (BUSY timeout)");
        return -1;
    }
    uint8_t payload_length = rx_status[0];
    uint8_t rx_start_offset = rx_status[1];

    if (payload_length > max_length) {
        return -1;
    }

    // Read payload from buffer
    if (!readBuffer(rx_start_offset, buffer, payload_length)) {
        logger_.error("Failed to read RX buffer (BUSY timeout)");
        return -1;
    }

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
    // Switch to STDBY_XOSC so TCXO starts now (datasheet section 13.3.6).
    // SetRx from STDBY_RC would hold BUSY for the full TCXO delay.
    uint8_t standby_mode = SX1262_STDBY_XOSC;
    sendCommand(SX1262_CMD_SET_STANDBY, &standby_mode, 1);

    // Clear IRQ flags
    clearIrqStatus(SX1262_IRQ_ALL);

    // Re-apply DIO1 IRQ mapping — mode transitions can corrupt it
    uint16_t irq_mask =
        SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR | SX1262_IRQ_TIMEOUT;
    setDioIrqParams(irq_mask, irq_mask);

    // Set RX mode with continuous receive (timeout = 0xFFFFFF)
    uint8_t rx_params[3] = {0xFF, 0xFF, 0xFF};
    sendCommand(SX1262_CMD_SET_RX, rx_params, 3);

    // Re-apply RX boosted gain — SetRx resets register 0x08AC to power-saving (0x94)
    writeRegister(SX1262_REG_RX_GAIN, 0x96);
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
    if (!readCommand(SX1262_CMD_GET_STATUS, nullptr, 0, &status, 1)) {
        logger_.error("Failed to wake chip (BUSY timeout)");
    }

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
    uint16_t irq_mask =
        SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR | SX1262_IRQ_TIMEOUT;
    setDioIrqParams(irq_mask, irq_mask);

    // Verify DIO IRQ params were accepted by reading back chip status
    uint8_t status = 0;
    readCommand(SX1262_CMD_GET_STATUS, nullptr, 0, &status, 1);
    uint8_t chip_mode = (status >> 4) & 0x07;
    uint8_t cmd_status = (status >> 1) & 0x07;
    logger_.info("enableInterruptMode: chip_mode=%d, cmd_status=%d (2=ok, 3/4/5=err)",
                 chip_mode, cmd_status);

    // Verify by reading GetIrqStatus — if mask was set, pending IRQs would show
    uint16_t irq_check = getIrqStatus();
    logger_.info("IRQ status after DIO config: 0x%04X (should be 0x0000 if cleared)", irq_check);

    // Register interrupt handler — use direct static ISR (bypasses GpioInterruptManager
    // to eliminate std::function overhead in ISR context as potential issue)
    isr_instance_ = this;
    isr_call_count_ = 0;
    gpio_set_irq_enabled_with_callback(dio1_pin_, GPIO_IRQ_EDGE_RISE, true, &SX1262::dio1Isr);

    interrupt_enabled_ = true;
    interrupt_pending_ = false;
    message_ready_ = false;
    tx_complete_ = false;

    // Diagnostic: check DIO1 physical state after setup
    bool dio1_state = gpio_get(dio1_pin_);
    logger_.info("DIO1 (GPIO %d) state after IRQ setup: %s", dio1_pin_,
                 dio1_state ? "HIGH" : "LOW");

    if (dio1_state) {
        logger_.warn("DIO1 already HIGH - clearing stale IRQ flags");
        clearIrqStatus(SX1262_IRQ_ALL);
        sleep_us(100);
        dio1_state = gpio_get(dio1_pin_);
        logger_.debug("DIO1 after clear: %s", dio1_state ? "HIGH (stuck!)" : "LOW (ok)");
    }

    logger_.debug("Interrupt mode enabled on DIO1 (GPIO %d)", dio1_pin_);
    return true;
}

void SX1262::disableInterruptMode()
{
    if (dio1_pin_ >= 0 && interrupt_enabled_) {
        gpio_set_irq_enabled(dio1_pin_, GPIO_IRQ_EDGE_RISE, false);
        isr_instance_ = nullptr;
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

    // Clear all active flags then re-apply DIO1 mapping — SX1262 DIO config
    // can get corrupted by mode transitions and repeated IRQ clear cycles
    clearIrqStatus(irq_flags);
    {
        uint16_t irq_mask =
            SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR | SX1262_IRQ_TIMEOUT;
        setDioIrqParams(irq_mask, irq_mask);
    }

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
        bool dio1_state = gpio_get(dio1_pin_);
        logger_.warn("RX done flag set but interrupt was missed! (DIO1=%s, isr_count=%lu)",
                     dio1_state ? "HIGH" : "LOW", isr_call_count_);
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

bool SX1262::sendCommand(uint8_t opcode, const uint8_t *params, size_t param_count)
{
    if (!waitBusy()) {
        return false;
    }

    // Largest SPI command is 9 bytes (opcode + 8 params for setDioIrqParams)
    static constexpr size_t MAX_PARAM_COUNT = 15;
    if (param_count > MAX_PARAM_COUNT) {
        logger_.error("sendCommand: param_count %u exceeds max %u", param_count, MAX_PARAM_COUNT);
        return false;
    }

    // Build TX buffer: opcode + params
    uint8_t tx_buf[1 + MAX_PARAM_COUNT];
    tx_buf[0] = opcode;
    if (params && param_count > 0) {
        memcpy(&tx_buf[1], params, param_count);
    }

    spi_.transfer(tx_buf, nullptr, 1 + param_count);

    // Wait for command to complete (except sleep commands)
    if (opcode != SX1262_CMD_SET_SLEEP) {
        return waitBusy();
    }
    return true;
}

bool SX1262::readCommand(uint8_t opcode, const uint8_t *params, size_t param_count,
                         uint8_t *response, size_t response_count)
{
    if (!waitBusy()) {
        return false;
    }

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

    return waitBusy();
}

bool SX1262::writeBuffer(uint8_t offset, const uint8_t *data, size_t length)
{
    if (!waitBusy()) {
        return false;
    }

    // WriteBuffer: opcode(0x0E) + offset + data bytes
    uint8_t tx_buf[258];  // 1 opcode + 1 offset + 256 max data
    tx_buf[0] = SX1262_CMD_WRITE_BUFFER;
    tx_buf[1] = offset;
    memcpy(&tx_buf[2], data, length);

    spi_.transfer(tx_buf, nullptr, 2 + length);

    return waitBusy();
}

bool SX1262::readBuffer(uint8_t offset, uint8_t *data, size_t length)
{
    if (!waitBusy()) {
        return false;
    }

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

    return waitBusy();
}

bool SX1262::writeRegister(uint16_t address, uint8_t value)
{
    if (!waitBusy()) {
        return false;
    }

    uint8_t tx_buf[4] = {SX1262_CMD_WRITE_REGISTER, (uint8_t)(address >> 8),
                         (uint8_t)(address & 0xFF), value};
    spi_.transfer(tx_buf, nullptr, 4);

    return waitBusy();
}

uint8_t SX1262::readRegister(uint16_t address)
{
    if (!waitBusy()) {
        logger_.error("readRegister(0x%04X): BUSY timeout before transfer", address);
        return 0;
    }

    // ReadRegister: opcode + addr_msb + addr_lsb + NOP + data
    uint8_t tx_buf[5] = {SX1262_CMD_READ_REGISTER, (uint8_t)(address >> 8),
                         (uint8_t)(address & 0xFF), 0x00, 0x00};
    uint8_t rx_buf[5] = {0};

    spi_.transfer(tx_buf, rx_buf, 5);

    if (!waitBusy()) {
        logger_.error("readRegister(0x%04X): BUSY timeout after transfer", address);
    }
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
        (uint8_t)(irq_mask >> 8),
        (uint8_t)(irq_mask & 0xFF),  // IRQ mask
        (uint8_t)(dio1_mask >> 8),
        (uint8_t)(dio1_mask & 0xFF),  // DIO1 mask
        0x00,
        0x00,  // DIO2 mask (unused)
        0x00,
        0x00  // DIO3 mask (unused)
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
    bool ok = sendCommand(SX1262_CMD_SET_MODULATION_PARAMS, params, 4);
    if (!ok) {
        logger_.error("SetModulationParams FAILED: SF=%d BW=%lu CR=4/%d",
                      spreading_factor_, bandwidth_, coding_rate_);
    }
}

void SX1262::configurePacketParams(uint8_t payload_length)
{
    uint8_t params[6] = {
        (uint8_t)(preamble_length_ >> 8),  // Preamble MSB
        (uint8_t)(preamble_length_),       // Preamble LSB
        SX1262_LORA_HEADER_EXPLICIT,       // Explicit header
        payload_length,                    // Payload length
        (uint8_t)(crc_enabled_ ? SX1262_LORA_CRC_ON : SX1262_LORA_CRC_OFF),
        SX1262_LORA_IQ_STANDARD  // Standard IQ
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
