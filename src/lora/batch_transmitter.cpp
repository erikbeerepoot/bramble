#include "batch_transmitter.h"

#include "../hal/logger.h"
#include "reliable_messenger.h"

static Logger logger("BATCH_TX");

BatchTransmitter::BatchTransmitter(ReliableMessenger &messenger, Config config)
    : messenger_(messenger), config_(config)
{
}

bool BatchTransmitter::transmit(const SensorDataRecord *records, size_t count,
                                CompletionCallback callback, uint32_t context)
{
    if (!records || count == 0 || count > MAX_BATCH_RECORDS) {
        return false;
    }

    // Convert SensorDataRecord to BatchSensorRecord format
    BatchSensorRecord batch_records[MAX_BATCH_RECORDS];
    for (size_t i = 0; i < count; i++) {
        batch_records[i].timestamp = records[i].timestamp;
        batch_records[i].temperature = records[i].temperature;
        batch_records[i].humidity = records[i].humidity;
        batch_records[i].flags = records[i].flags;
        batch_records[i].transmission_status = records[i].transmission_status;
        batch_records[i].crc16 = records[i].crc16;
    }

    uint8_t seq = messenger_.sendSensorDataBatchWithCallback(
        config_.hub_address, context, batch_records, static_cast<uint8_t>(count), RELIABLE,
        [this, count, cb = std::move(callback)](uint8_t seq_num, uint8_t ack_status, uint64_t) {
            batches_this_cycle_++;

            bool success = (ack_status == 0);
            if (success) {
                logger.info("Batch ACK (seq=%d): %zu records transmitted", seq_num, count);
                consecutive_failures_ = 0;
            } else {
                logger.warn("Batch failed (seq=%d, status=%d)", seq_num, ack_status);
                if (consecutive_failures_ < 255) {
                    consecutive_failures_++;
                }
            }

            if (cb) {
                cb(success);
            }
        },
        context);

    if (seq == 0) {
        logger.error("Failed to send batch message");
        return false;
    }

    logger.debug("Sent batch of %zu records (context=%lu, seq=%d)", count,
                 static_cast<unsigned long>(context), seq);
    return true;
}

void BatchTransmitter::resetCycleCounter()
{
    batches_this_cycle_ = 0;
}

bool BatchTransmitter::canSendMore() const
{
    return batches_this_cycle_ < config_.max_batches_per_cycle;
}

uint8_t BatchTransmitter::consecutiveFailures() const
{
    return consecutive_failures_;
}
