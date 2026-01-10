#pragma once

#include "application_mode.h"
#include "../hal/cht832x.h"
#include <memory>

/**
 * @brief Data logger sensor mode for temperature/humidity monitoring
 *
 * Reads temperature and humidity from CHT832X sensor (DFRobot SEN0546)
 * every 30 seconds and transmits via LoRa to the hub.
 */
class SensorMode : public ApplicationMode {
public:
    using ApplicationMode::ApplicationMode;

    ~SensorMode();

protected:
    void onStart() override;
    void onLoop() override;

private:
    std::unique_ptr<CHT832X> sensor_;

    // I2C pin configuration for CHT832X sensor
    static constexpr uint PIN_I2C_SDA = 26;  // GPIO26 (A0)
    static constexpr uint PIN_I2C_SCL = 27;  // GPIO27 (A1)

    // Timing configuration
    static constexpr uint32_t SENSOR_READ_INTERVAL_MS = 30000;   // 30 seconds
    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;     // 60 seconds

    /**
     * @brief Read sensor and transmit data
     * @param current_time Current system time in milliseconds
     */
    void readAndTransmitSensorData(uint32_t current_time);

    /**
     * @brief Send heartbeat with current status
     * @param current_time Current system time in milliseconds
     */
    void sendHeartbeat(uint32_t current_time);
};
