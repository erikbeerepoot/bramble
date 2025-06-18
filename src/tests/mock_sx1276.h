#pragma once

#include <vector>
#include <queue>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Mock SX1276 for controlled testing
 * Simulates packet loss, delays, and other failure modes
 */
class MockSX1276 {
public:
    MockSX1276();
    
    /**
     * @brief Set packet loss rate (0.0 = no loss, 1.0 = all packets lost)
     */
    void setPacketLossRate(float loss_rate) { packet_loss_rate_ = loss_rate; }
    
    /**
     * @brief Set transmission delay in milliseconds
     */
    void setTransmissionDelay(uint32_t delay_ms) { tx_delay_ms_ = delay_ms; }
    
    /**
     * @brief Enable/disable TX done simulation
     */
    void setTxDoneDelay(uint32_t delay_ms) { tx_done_delay_ms_ = delay_ms; }
    
    /**
     * @brief Queue a message to be "received"
     */
    void queueIncomingMessage(const uint8_t* data, size_t length);
    
    /**
     * @brief Get messages that were "sent"
     */
    std::vector<std::vector<uint8_t>> getSentMessages() const { return sent_messages_; }
    
    /**
     * @brief Clear sent message history
     */
    void clearSentMessages() { sent_messages_.clear(); }
    
    /**
     * @brief Reset all state
     */
    void reset();
    
    // SX1276-compatible interface
    bool begin() { return true; }
    bool send(const uint8_t* data, size_t length);
    bool isTxDone();
    int receive(uint8_t* buffer, size_t max_length);
    void startReceive() { /* no-op for mock */ }
    void setTxPower(int power_db) { /* no-op for mock */ }
    void setFrequency(uint32_t frequency_hz) { /* no-op for mock */ }
    int getRssi() { return -80; } // Mock RSSI
    float getSnr() { return 8.5f; } // Mock SNR
    
private:
    float packet_loss_rate_;
    uint32_t tx_delay_ms_;
    uint32_t tx_done_delay_ms_;
    uint32_t last_tx_time_;
    bool tx_in_progress_;
    
    std::vector<std::vector<uint8_t>> sent_messages_;
    std::queue<std::vector<uint8_t>> incoming_messages_;
    
    bool shouldDropPacket();
    uint32_t getCurrentTime();
};