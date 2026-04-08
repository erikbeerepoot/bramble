#include "event_log_transmitter.h"

#include <cstring>

#include "hal/logger.h"
#include "lora/message.h"
#include "lora/message_builder.h"
#include "lora/reliable_messenger.h"

static Logger logger("EventLogTX");

EventLogTransmitter::EventLogTransmitter(ReliableMessenger &messenger) : messenger_(messenger) {}

bool EventLogTransmitter::transmitIfPending(EventLog<64> &log)
{
    if (!log.hasPending()) {
        return true;
    }

    uint8_t count = log.pendingCount();
    if (count > MAX_EVENT_BATCH_RECORDS) {
        count = MAX_EVENT_BATCH_RECORDS;
    }

    // Read pending records
    EventRecord records[MAX_EVENT_BATCH_RECORDS];
    uint8_t actual = log.readPending(records, count);
    if (actual == 0) {
        return true;
    }

    // Build message manually (same pattern as createSensorDataBatchMessage)
    uint8_t buffer[MESSAGE_MAX_SIZE];
    Message *message = reinterpret_cast<Message *>(buffer);

    // Header
    message->header.magic = MESSAGE_MAGIC;
    message->header.type = MSG_TYPE_EVENT_LOG;
    message->header.flags = 0;  // BEST_EFFORT
    message->header.src_addr = messenger_.getNodeAddress();
    message->header.dst_addr = ADDRESS_HUB;
    message->header.seq_num = 0;  // Assigned by send()

    // Payload
    EventLogBatchPayload *payload = reinterpret_cast<EventLogBatchPayload *>(message->payload);
    payload->time_ref_unix = log.timeRefUnix();
    payload->time_ref_uptime = log.timeRefUptime();
    payload->record_count = actual;
    memcpy(payload->records, records, actual * sizeof(EventRecord));

    size_t payload_size = 9 + (actual * sizeof(EventRecord));  // 4+4+1 + records
    size_t total_size = MESSAGE_HEADER_SIZE + payload_size;

    logger.info("Transmitting %u event records (%zu bytes)", actual, total_size);

    bool sent = messenger_.send(buffer, total_size, BEST_EFFORT);
    if (sent) {
        log.advanceRead(actual);
        logger.info("Event log batch sent successfully");
    } else {
        logger.error("Failed to send event log batch");
    }

    return sent;
}
