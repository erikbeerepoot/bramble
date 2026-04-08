#pragma once

#include <cstdint>

#include "util/event_log.h"

class ReliableMessenger;

/**
 * @brief Transmits pending event log records to the hub
 *
 * Reads pending events from an EventLog, packs them into an
 * EventLogBatchPayload, and sends as BEST_EFFORT. No ACK wait —
 * read index is advanced immediately after send.
 *
 * Follows the BatchTransmitter pattern: owns I/O, caller owns policy.
 */
class EventLogTransmitter {
public:
    explicit EventLogTransmitter(ReliableMessenger &messenger);

    /**
     * @brief Transmit pending events if any exist
     * @param log Event log to read from and advance
     * @return true if sent or nothing to send; false on send failure
     */
    bool transmitIfPending(EventLog<64> &log);

private:
    ReliableMessenger &messenger_;
};
