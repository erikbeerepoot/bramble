#pragma once

#include <map>

#include "lora/message.h"
#include "mock_sx1276.h"

// Retry configuration
#define MAX_RETRIES 3
#define ACK_TIMEOUT_MS 5000
#define RETRY_BASE_DELAY_MS 1000

/**
 * @brief Pending message for retry tracking
 */
struct TestPendingMessage {
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length;
    uint8_t seq_num;
    uint16_t dst_addr;
    uint32_t last_send_time;
    uint8_t retry_count;
    uint32_t next_retry_time;
    bool ack_received;
};

/**
 * @brief Test version of ReliableMessenger that works with MockSX1276
 * Mirrors the real ReliableMessenger but accepts MockSX1276*
 */
class TestReliableMessenger {
public:
    TestReliableMessenger(MockSX1276 *mock_radio, uint16_t node_addr);

    bool sendActuatorCommand(uint16_t dst_addr, uint8_t actuator_type, uint8_t command,
                             const uint8_t *params, uint8_t param_length,
                             DeliveryCriticality criticality = RELIABLE);

    bool sendSensorData(uint16_t dst_addr, uint8_t sensor_type, const uint8_t *data,
                        uint8_t data_length, DeliveryCriticality criticality = BEST_EFFORT);

    bool send(const uint8_t *buffer, size_t length, DeliveryCriticality criticality = BEST_EFFORT);

    bool processIncomingMessage(const uint8_t *buffer, size_t length);
    void update();

    size_t getPendingCount() const { return pending_messages_.size(); }
    bool wasAcknowledged(uint8_t seq_num);
    uint16_t getNodeAddress() const { return node_addr_; }

private:
    MockSX1276 *mock_radio_;
    uint16_t node_addr_;
    uint8_t next_seq_num_;
    std::map<uint8_t, TestPendingMessage> pending_messages_;

    bool sendMessage(const uint8_t *buffer, size_t length);
    void handleAck(const AckPayload *ack_payload);
    void sendAck(uint16_t src_addr, uint8_t seq_num, uint8_t status);
    uint32_t calculateRetryDelay(uint8_t retry_count);
    uint32_t getCurrentTime();

    // Helper methods
    bool requiresAck(const uint8_t *buffer, size_t length);
    bool isCritical(const uint8_t *buffer, size_t length);
};