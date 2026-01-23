#pragma once

#include "lora/reliable_messenger.h"
#include "mock_sx1276.h"
#include "test_framework.h"

/**
 * @brief Integration tests for ReliableMessenger reliability features
 * Tests ACK/retry mechanism using MockSX1276 for controlled conditions
 */

// Simple test harness that directly uses MockSX1276 for testing
class ReliabilityTestHarness {
public:
    ReliabilityTestHarness(uint16_t node_addr);
    ~ReliabilityTestHarness() = default;

    // Test utilities
    void reset();
    void setPacketLossRate(float loss_rate);
    void simulateAckResponse(uint16_t src_addr, uint8_t seq_num);
    void runUpdates(uint32_t duration_ms, uint32_t step_ms = 10);

    // Simplified message sending for testing
    bool sendBestEffortMessage(uint16_t dst_addr, const uint8_t *data, size_t length);
    bool sendReliableMessage(uint16_t dst_addr, const uint8_t *data, size_t length);
    bool sendCriticalMessage(uint16_t dst_addr, const uint8_t *data, size_t length);

    // Process incoming message (e.g., ACKs)
    void processIncomingMessage(const uint8_t *buffer, size_t length);

    // Access to components
    MockSX1276 &getMockRadio() { return mock_radio_; }

    // Statistics
    uint32_t getSentMessageCount() const { return mock_radio_.getSentMessages().size(); }
    uint32_t getRetryCount() const;

private:
    MockSX1276 mock_radio_;
    uint16_t node_addr_;
    uint8_t next_seq_num_;
    uint32_t start_time_;

    // Simple retry tracking
    struct PendingMsg {
        uint8_t seq_num;
        uint32_t last_send_time;
        uint8_t retry_count;
        uint32_t next_retry_time;
        bool is_critical;
        uint8_t buffer[MESSAGE_MAX_SIZE];
        size_t length;
    };
    std::map<uint8_t, PendingMsg> pending_messages_;

    bool sendMessage(const uint8_t *buffer, size_t length, bool reliable, bool critical);
    void retryMessages();
};

// Test function declarations
bool test_best_effort_no_retries();
bool test_reliable_with_retries();
bool test_critical_max_retries();
bool test_ack_response_handling();
bool test_retry_backoff_timing();
bool test_message_timeout();
bool test_concurrent_reliability_levels();

// Test suite array
extern const TestCase reliability_test_suite[];
extern const size_t reliability_test_suite_size;