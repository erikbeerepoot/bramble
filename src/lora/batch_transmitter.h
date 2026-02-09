#pragma once

#include <cstdint>
#include <functional>

#include "../storage/sensor_data_record.h"
#include "message.h"

class ReliableMessenger;

/**
 * @brief Batch sensor data transmitter
 *
 * Handles converting SensorDataRecords to BatchSensorRecords and sending
 * them via ReliableMessenger. Owns transmission bookkeeping (failure count,
 * batches-per-cycle limit) but delegates all side effects to caller via
 * completion callbacks.
 *
 * Follows the HeartbeatClient pattern: owns I/O + bookkeeping, caller owns
 * the policy (flash advance, sleep decisions, etc).
 */
class BatchTransmitter {
public:
    struct Config {
        uint8_t max_batches_per_cycle;
        uint16_t hub_address;
    };

    using CompletionCallback = std::function<void(bool success)>;

    BatchTransmitter(ReliableMessenger &messenger, Config config);

    /**
     * @brief Send a batch of sensor records to the hub
     *
     * Converts SensorDataRecord[] to BatchSensorRecord[], sends via messenger,
     * and invokes callback on ACK or failure.
     *
     * @param records Array of sensor data records to transmit
     * @param count Number of records in the array
     * @param callback Called with true on ACK, false on failure
     * @param context Optional user context passed to messenger (e.g. flash start index)
     * @return true if send was initiated; false means callback will NOT be called
     */
    bool transmit(const SensorDataRecord *records, size_t count, CompletionCallback callback,
                  uint32_t context = 0);

    void resetCycleCounter();
    bool canSendMore() const;
    uint8_t consecutiveFailures() const;

private:
    ReliableMessenger &messenger_;
    Config config_;

    uint8_t consecutive_failures_ = 0;
    uint8_t batches_this_cycle_ = 0;
};
