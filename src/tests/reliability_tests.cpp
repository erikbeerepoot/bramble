#include "reliability_tests.h"
#include "lora/message.h"
#include "pico/stdlib.h"
#include <cstring>

// Test harness implementation
ReliabilityTestHarness::ReliabilityTestHarness(uint16_t node_addr) 
    : node_addr_(node_addr), next_seq_num_(1) {
    start_time_ = to_ms_since_boot(get_absolute_time());
}

void ReliabilityTestHarness::reset() {
    mock_radio_.reset();
    next_seq_num_ = 1;
    pending_messages_.clear();
    start_time_ = to_ms_since_boot(get_absolute_time());
}

void ReliabilityTestHarness::setPacketLossRate(float loss_rate) {
    mock_radio_.setPacketLossRate(loss_rate);
}

void ReliabilityTestHarness::simulateAckResponse(uint16_t src_addr, uint8_t seq_num) {
    // Create ACK message
    MessageHeader ack_header = {
        .magic = MESSAGE_MAGIC,
        .type = MSG_TYPE_ACK,
        .flags = 0,
        .src_addr = src_addr,
        .dst_addr = node_addr_,
        .seq_num = next_seq_num_++
    };
    
    AckPayload ack_payload = {
        .ack_seq_num = seq_num,
        .status = 0
    };
    
    uint8_t ack_buffer[MESSAGE_MAX_SIZE];
    memcpy(ack_buffer, &ack_header, sizeof(ack_header));
    memcpy(ack_buffer + sizeof(ack_header), &ack_payload, sizeof(ack_payload));
    
    mock_radio_.queueIncomingMessage(ack_buffer, sizeof(ack_header) + sizeof(ack_payload));
}

void ReliabilityTestHarness::runUpdates(uint32_t duration_ms, uint32_t step_ms) {
    uint32_t end_time = to_ms_since_boot(get_absolute_time()) + duration_ms;
    
    while (to_ms_since_boot(get_absolute_time()) < end_time) {
        // Process any incoming messages
        uint8_t rx_buffer[MESSAGE_MAX_SIZE];
        int rx_len = mock_radio_.receive(rx_buffer, sizeof(rx_buffer));
        if (rx_len > 0) {
            processIncomingMessage(rx_buffer, rx_len);
        }
        
        // Update retry logic
        retryMessages();
        
        sleep_ms(step_ms);
    }
}

bool ReliabilityTestHarness::sendBestEffortMessage(uint16_t dst_addr, const uint8_t* data, size_t length) {
    uint8_t buffer[MESSAGE_MAX_SIZE];
    MessageHandler handler;
    
    size_t msg_len = handler.createSensorMessage(
        node_addr_, dst_addr, next_seq_num_++,
        SENSOR_TEMPERATURE, data, length, 0, buffer);
    
    return sendMessage(buffer, msg_len, false, false);
}

bool ReliabilityTestHarness::sendReliableMessage(uint16_t dst_addr, const uint8_t* data, size_t length) {
    uint8_t buffer[MESSAGE_MAX_SIZE];
    MessageHandler handler;
    
    size_t msg_len = handler.createSensorMessage(
        node_addr_, dst_addr, next_seq_num_,
        SENSOR_SOIL_MOISTURE, data, length, MSG_FLAG_RELIABLE, buffer);
    
    return sendMessage(buffer, msg_len, true, false);
}

bool ReliabilityTestHarness::sendCriticalMessage(uint16_t dst_addr, const uint8_t* data, size_t length) {
    uint8_t buffer[MESSAGE_MAX_SIZE];
    MessageHandler handler;
    
    size_t msg_len = handler.createActuatorMessage(
        node_addr_, dst_addr, next_seq_num_,
        ACTUATOR_VALVE, CMD_TURN_OFF, data, length, 
        MSG_FLAG_RELIABLE | MSG_FLAG_CRITICAL, buffer);
    
    return sendMessage(buffer, msg_len, true, true);
}

bool ReliabilityTestHarness::sendMessage(const uint8_t* buffer, size_t length, bool reliable, bool critical) {
    bool sent = mock_radio_.send(buffer, length);
    
    if (sent && reliable) {
        // Track for retries
        PendingMsg pending;
        MessageHeader* header = (MessageHeader*)buffer;
        
        pending.seq_num = header->seq_num;
        pending.last_send_time = to_ms_since_boot(get_absolute_time());
        pending.retry_count = 0;
        pending.next_retry_time = pending.last_send_time + RETRY_BASE_DELAY_MS;
        pending.is_critical = critical;
        pending.length = length;
        memcpy(pending.buffer, buffer, length);
        
        pending_messages_[pending.seq_num] = pending;
        next_seq_num_++;
    } else if (!reliable) {
        next_seq_num_++;
    }
    
    return sent;
}

void ReliabilityTestHarness::processIncomingMessage(const uint8_t* buffer, size_t length) {
    if (length < sizeof(MessageHeader)) return;
    
    MessageHeader* header = (MessageHeader*)buffer;
    
    if (header->type == MSG_TYPE_ACK && length >= sizeof(MessageHeader) + sizeof(AckPayload)) {
        AckPayload* ack = (AckPayload*)(buffer + sizeof(MessageHeader));
        
        // Remove acknowledged message from pending
        auto it = pending_messages_.find(ack->ack_seq_num);
        if (it != pending_messages_.end()) {
            pending_messages_.erase(it);
        }
    }
}

void ReliabilityTestHarness::retryMessages() {
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    
    for (auto& [seq_num, pending] : pending_messages_) {
        if (current_time >= pending.next_retry_time) {
            uint8_t max_retries = pending.is_critical ? MAX_RETRIES * 2 : MAX_RETRIES;
            
            if (pending.retry_count < max_retries) {
                // Retry the message
                bool sent = mock_radio_.send(pending.buffer, pending.length);
                if (sent) {
                    pending.retry_count++;
                    pending.last_send_time = current_time;
                    
                    // Exponential backoff
                    uint32_t delay = RETRY_BASE_DELAY_MS * (1 << pending.retry_count);
                    if (delay > 8000) delay = 8000; // Cap at 8 seconds
                    pending.next_retry_time = current_time + delay;
                }
            }
            // If max retries reached, keep the message in pending_messages_ for statistics
        }
    }
}

uint32_t ReliabilityTestHarness::getRetryCount() const {
    uint32_t total_retries = 0;
    for (const auto& [seq_num, pending] : pending_messages_) {
        total_retries += pending.retry_count;
    }
    return total_retries;
}

// Test implementations using simplified harness

bool test_best_effort_no_retries() {
    printf("  Testing BEST_EFFORT messages have no retries\n");
    
    ReliabilityTestHarness harness(0x0001);
    
    // Set 100% packet loss to ensure no ACK comes back
    harness.setPacketLossRate(1.0f);
    
    // Send BEST_EFFORT message
    uint8_t test_data[] = {0x11, 0x22};
    bool sent = harness.sendBestEffortMessage(0x0002, test_data, sizeof(test_data));
    
    TEST_ASSERT(sent);
    
    // Wait and update - should NOT retry
    harness.runUpdates(2000, 50);  // Wait 2 seconds
    
    TEST_ASSERT_EQUAL(1, harness.getSentMessageCount());  // Only 1 attempt
    TEST_ASSERT_EQUAL(0, harness.getRetryCount());
    
    return true;
}

bool test_reliable_with_retries() {
    printf("  Testing RELIABLE messages retry on packet loss\n");
    
    ReliabilityTestHarness harness(0x0001);
    
    // Set 100% packet loss to ensure no ACK comes back
    harness.setPacketLossRate(1.0f);
    
    // Send RELIABLE message
    uint8_t test_data[] = {0x33, 0x44};
    bool sent = harness.sendReliableMessage(0x0002, test_data, sizeof(test_data));
    
    TEST_ASSERT(sent);
    
    // Wait for retries to occur
    harness.runUpdates(5000, 100);  // Wait 5 seconds for retries
    
    // Should have multiple attempts
    TEST_ASSERT(harness.getSentMessageCount() > 1);
    TEST_ASSERT(harness.getRetryCount() > 0);
    
    return true;
}

bool test_critical_max_retries() {
    printf("  Testing CRITICAL messages use maximum retries\n");
    
    ReliabilityTestHarness harness(0x0001);
    
    // Set 100% packet loss to ensure no ACK comes back
    harness.setPacketLossRate(1.0f);
    
    // Send CRITICAL message
    uint8_t valve_param = 0x01;
    bool sent = harness.sendCriticalMessage(0x0002, &valve_param, 1);
    
    TEST_ASSERT(sent);
    
    // Wait for maximum retries
    harness.runUpdates(15000, 100);  // Wait 15 seconds
    
    // Should have made maximum attempts (1 initial + retries)
    uint32_t total_attempts = harness.getSentMessageCount();
    printf("    Total attempts: %d\n", (int)total_attempts);
    
    // CRITICAL should retry more than RELIABLE
    TEST_ASSERT(total_attempts >= 4);  // At least 1 initial + 3 retries
    
    return true;
}

bool test_ack_response_handling() {
    printf("  Testing ACK response stops retries\n");
    
    ReliabilityTestHarness harness(0x0001);
    
    // Send RELIABLE message (will retry if no ACK)
    uint8_t test_data[] = {0x55, 0x66};
    bool sent = harness.sendReliableMessage(0x0002, test_data, sizeof(test_data));
    
    TEST_ASSERT(sent);
    
    // Get the sequence number from the sent message
    auto sent_msgs = harness.getMockRadio().getSentMessages();
    TEST_ASSERT(sent_msgs.size() == 1);
    
    MessageHeader* header = (MessageHeader*)sent_msgs[0].data();
    uint8_t seq_num = header->seq_num;
    
    // Wait a bit, then simulate ACK
    sleep_ms(500);
    harness.simulateAckResponse(0x0002, seq_num);
    
    // Continue running updates
    harness.runUpdates(3000, 50);
    
    // Should not have retried after ACK
    TEST_ASSERT_EQUAL(1, harness.getSentMessageCount());
    TEST_ASSERT_EQUAL(0, harness.getRetryCount());
    
    return true;
}

bool test_retry_backoff_timing() {
    printf("  Testing retry backoff timing\n");
    
    ReliabilityTestHarness harness(0x0001);
    harness.setPacketLossRate(1.0f);  // Force retries
    
    // Send RELIABLE message
    uint8_t test_data[] = {0x77, 0x88};
    bool sent = harness.sendReliableMessage(0x0002, test_data, sizeof(test_data));
    
    TEST_ASSERT(sent);
    
    uint32_t start_time = to_ms_since_boot(get_absolute_time());
    
    // Wait for first retry (should be ~1 second)
    harness.runUpdates(1500, 50);
    TEST_ASSERT(harness.getSentMessageCount() >= 2);  // Initial + first retry
    
    // Wait for second retry (should be ~2 seconds after first)
    harness.runUpdates(2500, 50);
    TEST_ASSERT(harness.getSentMessageCount() >= 3);  // + second retry
    
    uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_time;
    printf("    Total elapsed time: %d ms\n", (int)elapsed);
    
    // Should have taken at least 3 seconds (1s + 2s for exponential backoff)
    TEST_ASSERT(elapsed >= 3000);
    
    return true;
}

bool test_message_timeout() {
    printf("  Testing message timeout after max retries\n");
    
    ReliabilityTestHarness harness(0x0001);
    harness.setPacketLossRate(1.0f);  // Force timeout
    
    // Send RELIABLE message
    uint8_t test_data[] = {0x99, 0xAA};
    bool sent = harness.sendReliableMessage(0x0002, test_data, sizeof(test_data));
    
    TEST_ASSERT(sent);
    
    uint32_t initial_count = harness.getSentMessageCount();
    
    // Wait long enough for all retries to complete
    harness.runUpdates(10000, 100);  // 10 seconds
    
    uint32_t final_count = harness.getSentMessageCount();
    printf("    Final attempt count: %d\n", (int)final_count);
    
    // Should have stopped retrying after timeout
    TEST_ASSERT(final_count > initial_count);  // Had some retries
    
    // Wait more time - should not send additional attempts
    harness.runUpdates(3000, 100);
    TEST_ASSERT_EQUAL(final_count, harness.getSentMessageCount());
    
    return true;
}

bool test_concurrent_reliability_levels() {
    printf("  Testing concurrent messages with different reliability levels\n");
    
    ReliabilityTestHarness harness(0x0001);
    harness.setPacketLossRate(1.0f);  // Force retries
    
    // Send messages with different reliability levels
    uint8_t data1[] = {0x01};
    uint8_t data2[] = {0x02};
    uint8_t data3[] = {0x03};
    
    // BEST_EFFORT should not retry
    bool sent1 = harness.sendBestEffortMessage(0x0002, data1, sizeof(data1));
    
    // RELIABLE should retry
    bool sent2 = harness.sendReliableMessage(0x0002, data2, sizeof(data2));
    
    // CRITICAL should retry the most
    bool sent3 = harness.sendCriticalMessage(0x0002, data3, sizeof(data3));
    
    TEST_ASSERT(sent1 && sent2 && sent3);
    
    // Wait for retries
    harness.runUpdates(8000, 100);
    
    uint32_t total_attempts = harness.getSentMessageCount();
    printf("    Total attempts for all messages: %d\n", (int)total_attempts);
    
    // Should have more than 3 attempts (1 BEST_EFFORT + retries for others)
    TEST_ASSERT(total_attempts > 3);
    
    return true;
}

// Test suite definition
const TestCase reliability_test_suite[] = {
    {"BEST_EFFORT no retries", test_best_effort_no_retries},
    {"RELIABLE with retries", test_reliable_with_retries},
    {"CRITICAL max retries", test_critical_max_retries},
    {"ACK response handling", test_ack_response_handling},
    {"Retry backoff timing", test_retry_backoff_timing},
    {"Message timeout", test_message_timeout},
    {"Concurrent reliability levels", test_concurrent_reliability_levels}
};

const size_t reliability_test_suite_size = sizeof(reliability_test_suite) / sizeof(TestCase);