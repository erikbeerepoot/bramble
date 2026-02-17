#pragma once

#include <cstdint>

#include "../util/event_log.h"
#include "message.h"

class ReliableMessenger;

/**
 * @brief Transmits pending event log records to the hub
 *
 * Reads pending events from an EventLog, packs them into an EventLogBatchPayload,
 * and sends via ReliableMessenger with BEST_EFFORT criticality. Since there's no
 * ACK, the read index is advanced immediately after sending.
 */
class EventLogTransmitter {
public:
    EventLogTransmitter(ReliableMessenger &messenger, EventLog<> &event_log);

    /**
     * @brief Transmit pending events if any exist
     *
     * Reads up to MAX_EVENT_BATCH_RECORDS events, packs them into a batch
     * payload with time reference information, sends BEST_EFFORT, and advances
     * the read index.
     *
     * @return true if events were sent (or none pending), false on send failure
     */
    bool transmitIfPending();

private:
    ReliableMessenger &messenger_;
    EventLog<> &event_log_;
};
