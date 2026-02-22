#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hardware/gpio.h"

/**
 * @brief Abstract interface for radio modules (SX1276, SX1262, mocks)
 *
 * Provides a common API for LoRa transceivers, allowing the protocol stack
 * (ReliableMessenger, ApplicationMode) to work with any radio chip.
 */
class RadioInterface {
public:
    virtual ~RadioInterface() = default;

    // --- Lifecycle ---
    virtual bool begin() = 0;
    virtual bool isConnected() = 0;
    virtual void reset() = 0;

    // --- Configuration ---
    virtual void setTxPower(int power_db) = 0;
    virtual void setFrequency(uint32_t frequency_hz) = 0;
    virtual void setSpreadingFactor(int sf) = 0;
    virtual void setBandwidth(uint32_t bandwidth_hz) = 0;
    virtual void setCodingRate(int denominator) = 0;
    virtual void setPreambleLength(int length) = 0;
    virtual void setCrc(bool enable_crc) = 0;

    // --- Transmit ---
    virtual bool send(const uint8_t *data, size_t length) = 0;
    virtual bool sendAsync(const uint8_t *data, size_t length) = 0;
    virtual bool isTxDone() = 0;

    // --- Receive ---
    virtual int receive(uint8_t *buffer, size_t max_length) = 0;
    virtual bool available() = 0;
    virtual void startReceive() = 0;

    // --- Signal quality ---
    virtual int getRssi() = 0;
    virtual float getSnr() = 0;

    // --- Power management ---
    virtual void sleep() = 0;
    virtual void wakeup() = 0;

    // --- Interrupt mode ---
    virtual bool enableInterruptMode(gpio_irq_callback_t callback = nullptr) = 0;
    virtual void disableInterruptMode() = 0;
    virtual bool isInterruptPending() const = 0;
    virtual uint8_t handleInterrupt() = 0;
    virtual bool checkForMissedRxInterrupt() = 0;
    virtual bool isMessageReady() const = 0;
    virtual bool isTxComplete() const = 0;
    virtual void clearInterruptFlags() = 0;
};
