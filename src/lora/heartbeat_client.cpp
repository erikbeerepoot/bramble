#include "heartbeat_client.h"

#include "../hal/logger.h"
#include "reliable_messenger.h"

static Logger logger("HEARTBEAT");

HeartbeatClient::HeartbeatClient(ReliableMessenger &messenger) : messenger_(messenger) {}

void HeartbeatClient::setStatusProvider(StatusProvider provider)
{
    status_provider_ = std::move(provider);
}

void HeartbeatClient::setResponseCallback(ResponseCallback callback)
{
    response_callback_ = std::move(callback);
}

void HeartbeatClient::setDeliveryCallback(DeliveryCallback callback)
{
    delivery_callback_ = std::move(callback);
}

void HeartbeatClient::send()
{
    if (!status_provider_) {
        logger.error("No status provider set");
        return;
    }

    HeartbeatStatus status = status_provider_();

    logger.debug("Sending heartbeat (uptime=%lu s, battery=%u, errors=0x%04X, pending=%u)",
                 status.uptime, status.battery_level, status.error_flags, status.pending_records);

    messenger_.sendHeartbeat(ADDRESS_HUB, status.uptime, status.battery_level,
                             status.signal_strength, status.active_sensors, status.error_flags,
                             status.pending_records, RELIABLE,
                             [this](uint8_t seq_num, uint8_t ack_status, uint64_t) {
                                 bool success = (ack_status == 0);
                                 if (!success) {
                                     logger.warn("Heartbeat delivery failed (seq=%d)", seq_num);
                                 }
                                 if (delivery_callback_) {
                                     delivery_callback_(success);
                                 }
                                 // Success case: onHeartbeatResponse() handles the
                                 // HEARTBEAT_RESPONSE message
                             });
}

void HeartbeatClient::handleResponse(const HeartbeatResponsePayload *payload)
{
    if (!payload) {
        return;
    }

    if (response_callback_) {
        response_callback_(*payload);
    }
}
