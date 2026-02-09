#pragma once

#include <cstdint>
#include <functional>

#include "message.h"

class ReliableMessenger;

struct HeartbeatStatus {
    uint32_t uptime;
    uint8_t battery_level;
    uint8_t signal_strength;
    uint8_t active_sensors;
    uint16_t error_flags;
    uint16_t pending_records;
};

/**
 * @brief Heartbeat message protocol client
 *
 * Handles sending heartbeat status to the hub and parsing heartbeat responses.
 * Does NOT own side effects (RTC, PMU, state machine) — those are the mode's
 * responsibility via callbacks.
 */
class HeartbeatClient {
public:
    using StatusProvider = std::function<HeartbeatStatus()>;
    using ResponseCallback = std::function<void(const HeartbeatResponsePayload &response)>;
    using DeliveryCallback = std::function<void(bool success)>;

    explicit HeartbeatClient(ReliableMessenger &messenger);

    void setStatusProvider(StatusProvider provider);
    void setResponseCallback(ResponseCallback callback);
    void setDeliveryCallback(DeliveryCallback callback);

    /**
     * @brief Collect status from provider and send heartbeat to hub
     *
     * Queries the StatusProvider for current node status, then sends a
     * RELIABLE heartbeat to ADDRESS_HUB via the messenger.
     */
    void send();

    /**
     * @brief Handle an incoming heartbeat response
     *
     * Parses the response payload and invokes the ResponseCallback.
     * @param payload Heartbeat response from hub
     */
    void handleResponse(const HeartbeatResponsePayload *payload);

private:
    ReliableMessenger &messenger_;
    StatusProvider status_provider_;
    ResponseCallback response_callback_;
    DeliveryCallback delivery_callback_;
};
