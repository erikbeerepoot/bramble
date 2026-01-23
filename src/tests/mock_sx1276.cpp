#include "mock_sx1276.h"

#include <stdio.h>

#include <cstring>

#include "pico/stdlib.h"

MockSX1276::MockSX1276()
{
    reset();
}

void MockSX1276::reset()
{
    packet_loss_rate_ = 0.0f;
    tx_delay_ms_ = 0;
    tx_done_delay_ms_ = 10;  // Default 10ms TX time
    last_tx_time_ = 0;
    tx_in_progress_ = false;
    sent_messages_.clear();
    while (!incoming_messages_.empty()) {
        incoming_messages_.pop();
    }
}

bool MockSX1276::send(const uint8_t *data, size_t length)
{
    if (!data || length == 0)
        return false;

    printf("[MOCK] TX attempt (%d bytes)\n", (int)length);

    // Check if we should drop this packet
    if (shouldDropPacket()) {
        printf("[MOCK] Packet dropped (simulated loss)\n");
        return false;
    }

    // Store the sent message
    std::vector<uint8_t> message(data, data + length);
    sent_messages_.push_back(message);

    // Start transmission
    tx_in_progress_ = true;
    last_tx_time_ = getCurrentTime();

    printf("[MOCK] TX started\n");
    return true;
}

bool MockSX1276::isTxDone()
{
    if (!tx_in_progress_)
        return true;

    uint32_t elapsed = getCurrentTime() - last_tx_time_;
    if (elapsed >= tx_done_delay_ms_) {
        tx_in_progress_ = false;
        printf("[MOCK] TX completed\n");
        return true;
    }

    return false;
}

int MockSX1276::receive(uint8_t *buffer, size_t max_length)
{
    if (incoming_messages_.empty()) {
        return 0;  // No messages available
    }

    std::vector<uint8_t> message = incoming_messages_.front();
    incoming_messages_.pop();

    if (message.size() > max_length) {
        printf("[MOCK] RX buffer too small\n");
        return -1;
    }

    memcpy(buffer, message.data(), message.size());
    printf("[MOCK] RX message (%d bytes)\n", (int)message.size());

    return message.size();
}

void MockSX1276::queueIncomingMessage(const uint8_t *data, size_t length)
{
    if (!data || length == 0)
        return;

    std::vector<uint8_t> message(data, data + length);
    incoming_messages_.push(message);

    printf("[MOCK] Queued incoming message (%d bytes)\n", (int)length);
}

bool MockSX1276::shouldDropPacket()
{
    if (packet_loss_rate_ <= 0.0f)
        return false;
    if (packet_loss_rate_ >= 1.0f)
        return true;

    // Simple pseudo-random based on time
    uint32_t time = getCurrentTime();
    float random_value = ((time * 1103515245 + 12345) % 1000) / 1000.0f;

    return random_value < packet_loss_rate_;
}

uint32_t MockSX1276::getCurrentTime()
{
    return to_ms_since_boot(get_absolute_time());
}