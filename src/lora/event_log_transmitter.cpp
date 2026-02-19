#include "event_log_transmitter.h"

#include <cstring>

#include "../hal/logger.h"
#include "reliable_messenger.h"

static Logger logger("EVT_TX");

EventLogTransmitter::EventLogTransmitter(ReliableMessenger &messenger, EventLog<> &event_log)
    : messenger_(messenger), event_log_(event_log)
{
}

bool EventLogTransmitter::transmitIfPending()
{
    if (!event_log_.hasPending()) {
        return true;
    }

    // Read pending events
    EventRecord records[MAX_EVENT_BATCH_RECORDS];
    size_t count = event_log_.readPending(records, MAX_EVENT_BATCH_RECORDS);

    if (count == 0) {
        return true;
    }

    // Build the message manually (same pattern as sendBatchAck in hub_mode)
    uint8_t buffer[MESSAGE_MAX_SIZE];
    Message *message = reinterpret_cast<Message *>(buffer);

    message->header.magic = MESSAGE_MAGIC;
    message->header.type = MSG_TYPE_EVENT_LOG;
    message->header.flags = 0;  // BEST_EFFORT: no ACK required
    message->header.src_addr = messenger_.getNodeAddress();
    message->header.dst_addr = ADDRESS_HUB;
    message->header.seq_num = 0;  // Will be assigned by messenger

    // Pack the payload header
    EventLogBatchPayload *payload = reinterpret_cast<EventLogBatchPayload *>(message->payload);
    payload->node_addr = messenger_.getNodeAddress();
    payload->reference_uptime = event_log_.getReferenceUptime();
    payload->reference_timestamp = event_log_.getReferenceTimestamp();
    payload->record_count = static_cast<uint8_t>(count);

    // Copy event records after the fixed header fields
    size_t payload_header_size = sizeof(EventLogBatchPayload);
    size_t records_size = count * sizeof(EventRecord);
    memcpy(message->payload + payload_header_size, records, records_size);

    size_t message_size = MESSAGE_HEADER_SIZE + payload_header_size + records_size;

    if (messenger_.send(buffer, message_size, BEST_EFFORT)) {
        logger.info("Sent %zu event log records", count);
        // BEST_EFFORT: no ACK, advance immediately
        event_log_.advanceReadIndex(count);
        return true;
    }

    logger.warn("Failed to send event log batch");
    return false;
}
