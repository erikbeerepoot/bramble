#pragma once

#include "lora/sx1276.h"
#include "radio_interface.h"

/**
 * @brief Adapter to make SX1276 implement RadioInterface
 */
class SX1276Adapter : public RadioInterface {
public:
    SX1276Adapter(SX1276 *sx1276) : sx1276_(sx1276) {}

    bool begin() override { return sx1276_->begin(); }
    bool send(const uint8_t *data, size_t length) override { return sx1276_->send(data, length); }
    bool isTxDone() override { return sx1276_->isTxDone(); }
    int receive(uint8_t *buffer, size_t max_length) override
    {
        return sx1276_->receive(buffer, max_length);
    }
    void startReceive() override { sx1276_->startReceive(); }
    void setTxPower(int power_db) override { sx1276_->setTxPower(power_db); }
    void setFrequency(uint32_t frequency_hz) override { sx1276_->setFrequency(frequency_hz); }
    int getRssi() override { return sx1276_->getRssi(); }
    float getSnr() override { return sx1276_->getSnr(); }

private:
    SX1276 *sx1276_;
};