#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Abstract interface for radio modules
 * Allows for mocking and testing without hardware dependencies
 */
class RadioInterface {
public:
    virtual ~RadioInterface() = default;
    
    virtual bool begin() = 0;
    virtual bool send(const uint8_t* data, size_t length) = 0;
    virtual bool isTxDone() = 0;
    virtual int receive(uint8_t* buffer, size_t max_length) = 0;
    virtual void startReceive() = 0;
    virtual void setTxPower(int power_db) = 0;
    virtual void setFrequency(uint32_t frequency_hz) = 0;
    virtual int getRssi() = 0;
    virtual float getSnr() = 0;
};