#pragma once

#include <stdint.h>
#include <map>
#include <cmath>
#include <algorithm>
#include "pico/time.h"
#include "../hal/logger.h"
#include "message.h"

/**
 * @brief Rolling window statistics for signal quality
 */
template<size_t WINDOW_SIZE = 100>
class RollingStats {
public:
    RollingStats() : head_(0), count_(0) {
        // Initialize array to avoid undefined values
        for (size_t i = 0; i < WINDOW_SIZE; i++) {
            values_[i] = 0;
        }
    }
    
    void add(int16_t value) {
        values_[head_] = value;
        head_ = (head_ + 1) % WINDOW_SIZE;
        if (count_ < WINDOW_SIZE) count_++;
    }
    
    float getMean() const {
        if (count_ == 0) return 0;
        float sum = 0;
        for (size_t i = 0; i < count_; i++) {
            sum += values_[i];
        }
        return sum / count_;
    }
    
    float getStdDev() const {
        if (count_ < 2) return 0;
        float mean = getMean();
        float sum_sq = 0;
        for (size_t i = 0; i < count_; i++) {
            float diff = values_[i] - mean;
            sum_sq += diff * diff;
        }
        return std::sqrt(sum_sq / (count_ - 1));
    }
    
    int16_t getMin() const {
        if (count_ == 0) return 0;
        int16_t min_val = values_[0];
        for (size_t i = 1; i < count_; i++) {
            if (values_[i] < min_val) min_val = values_[i];
        }
        return min_val;
    }
    
    int16_t getMax() const {
        if (count_ == 0) return 0;
        int16_t max_val = values_[0];
        for (size_t i = 1; i < count_; i++) {
            if (values_[i] > max_val) max_val = values_[i];
        }
        return max_val;
    }
    
    int16_t getPercentile(uint8_t percentile) const {
        if (count_ == 0 || percentile > 100) return 0;
        
        // Copy values for sorting
        int16_t sorted[WINDOW_SIZE];
        for (size_t i = 0; i < count_; i++) {
            sorted[i] = values_[i];
        }
        std::sort(sorted, sorted + count_);
        
        size_t index = (count_ - 1) * percentile / 100;
        return sorted[index];
    }
    
    bool isTrendingUp() const {
        if (count_ < 10) return false;
        
        // Simple trend: compare first half average to second half
        float first_half = 0, second_half = 0;
        size_t half = count_ / 2;
        
        for (size_t i = 0; i < half; i++) {
            first_half += values_[i];
        }
        for (size_t i = half; i < count_; i++) {
            second_half += values_[i];
        }
        
        first_half /= half;
        second_half /= (count_ - half);
        
        return second_half > first_half + 2; // +2 dB threshold
    }
    
    size_t getSampleCount() const { return count_; }
    
private:
    int16_t values_[WINDOW_SIZE];
    size_t head_;
    size_t count_;
};

/**
 * @brief Link quality categories
 */
enum LinkQuality {
    LINK_EXCELLENT,  // RSSI > -60 dBm
    LINK_GOOD,       // RSSI > -80 dBm  
    LINK_FAIR,       // RSSI > -100 dBm
    LINK_POOR        // RSSI <= -100 dBm
};

/**
 * @brief Statistics for a specific message criticality level
 */
struct MessageTypeStats {
    uint32_t sent = 0;           // Total messages sent
    uint32_t delivered = 0;      // Successfully delivered (ACKed for reliable/critical)
    uint32_t timeouts = 0;       // Timed out waiting for ACK
    uint32_t retries = 0;        // Total retry attempts
    uint32_t max_retries = 0;    // Maximum retries for a single message
    
    float getDeliveryRate() const {
        if (sent == 0) return 100.0f;
        return (float)delivered / sent * 100.0f;
    }
    
    float getAverageRetries() const {
        if (delivered == 0) return 0;
        return (float)retries / delivered;
    }
};

/**
 * @brief Statistics for a single node
 */
struct NodeStatistics {
    // Message counters by criticality level
    MessageTypeStats best_effort_stats;
    MessageTypeStats reliable_stats;
    MessageTypeStats critical_stats;
    
    // Overall counters
    uint32_t messages_received = 0;      // Total messages received from this node
    uint32_t acks_sent = 0;              // ACKs sent to this node
    uint32_t acks_received = 0;          // ACKs received from this node
    
    // Error counters
    uint32_t crc_errors = 0;             // CRC errors from this node
    uint32_t invalid_messages = 0;       // Invalid/malformed messages
    
    // Signal quality tracking
    RollingStats<100> rssi_stats;        // Last 100 RSSI values
    RollingStats<100> snr_stats;         // Last 100 SNR values
    
    // Link quality state
    LinkQuality current_link_quality = LINK_POOR;
    uint32_t link_quality_changes = 0;
    uint32_t time_entered_current_quality = 0;
    
    // Timing
    uint32_t last_seen_time = 0;         // Last communication time
    uint32_t first_seen_time = 0;        // First communication time
    
    // Helper to get stats by criticality
    MessageTypeStats& getStatsByCriticality(DeliveryCriticality criticality) {
        switch (criticality) {
            case CRITICAL: return critical_stats;
            case RELIABLE: return reliable_stats;
            default: return best_effort_stats;
        }
    }
    
    const MessageTypeStats& getStatsByCriticality(DeliveryCriticality criticality) const {
        switch (criticality) {
            case CRITICAL: return critical_stats;
            case RELIABLE: return reliable_stats;
            default: return best_effort_stats;
        }
    }
    
    uint32_t getTotalMessagesSent() const {
        return best_effort_stats.sent + reliable_stats.sent + critical_stats.sent;
    }
    
    uint32_t getTotalDelivered() const {
        return best_effort_stats.delivered + reliable_stats.delivered + critical_stats.delivered;
    }
    
    uint32_t getTotalTimeouts() const {
        return reliable_stats.timeouts + critical_stats.timeouts;
    }
    
    uint32_t getTotalRetries() const {
        return reliable_stats.retries + critical_stats.retries;
    }
    
    LinkQuality calculateLinkQuality(int16_t rssi) const {
        if (rssi > -60) return LINK_EXCELLENT;
        if (rssi > -80) return LINK_GOOD;
        if (rssi > -100) return LINK_FAIR;
        return LINK_POOR;
    }
    
    const char* getLinkQualityString() const {
        switch (current_link_quality) {
            case LINK_EXCELLENT: return "Excellent";
            case LINK_GOOD: return "Good";
            case LINK_FAIR: return "Fair";
            case LINK_POOR: return "Poor";
            default: return "Unknown";
        }
    }
    
    uint32_t getUptimeSeconds() const {
        if (first_seen_time == 0 || last_seen_time == 0) return 0;
        return (last_seen_time - first_seen_time) / 1000;
    }
};

/**
 * @brief Global network statistics
 */
struct GlobalStatistics {
    // Overall counters by criticality
    MessageTypeStats best_effort_total;
    MessageTypeStats reliable_total;
    MessageTypeStats critical_total;
    
    // General counters
    uint32_t total_messages_received = 0;
    uint32_t total_acks_sent = 0;
    uint32_t total_acks_received = 0;
    uint32_t total_broadcasts = 0;
    
    // Error counters
    uint32_t total_crc_errors = 0;
    uint32_t total_invalid_messages = 0;
    
    // Network metrics
    uint32_t nodes_registered = 0;
    uint32_t nodes_active = 0;
    uint32_t nodes_inactive = 0;
    
    // Timing
    uint32_t network_start_time = 0;
    
    uint32_t getNetworkUptimeSeconds() const {
        if (network_start_time == 0) return 0;
        return (to_ms_since_boot(get_absolute_time()) - network_start_time) / 1000;
    }
    
    uint32_t getTotalMessagesSent() const {
        return best_effort_total.sent + reliable_total.sent + critical_total.sent;
    }
    
    uint32_t getTotalDelivered() const {
        return best_effort_total.delivered + reliable_total.delivered + critical_total.delivered;
    }
    
    float getOverallDeliveryRate() const {
        // Only count reliable and critical for overall delivery rate
        uint32_t reliable_sent = reliable_total.sent + critical_total.sent;
        uint32_t reliable_delivered = reliable_total.delivered + critical_total.delivered;
        
        if (reliable_sent == 0) return 100.0f;
        return (float)reliable_delivered / reliable_sent * 100.0f;
    }
};

/**
 * @brief Network statistics tracking and reporting
 */
class NetworkStats {
public:
    NetworkStats();
    
    /**
     * @brief Record a message sent
     * @param dst_addr Destination address
     * @param criticality Message criticality level
     * @param delivered Whether the message was delivered (ACKed)
     * @param retries Number of retries needed
     */
    void recordMessageSent(uint16_t dst_addr, DeliveryCriticality criticality, 
                          bool delivered, uint8_t retries = 0);
    
    /**
     * @brief Record a message received
     * @param src_addr Source address
     * @param rssi RSSI value in dBm
     * @param snr SNR value in dB
     * @param crc_error Whether there was a CRC error
     */
    void recordMessageReceived(uint16_t src_addr, int16_t rssi, float snr, bool crc_error = false);
    
    /**
     * @brief Record an ACK sent
     * @param dst_addr Destination address
     */
    void recordAckSent(uint16_t dst_addr);
    
    /**
     * @brief Record an ACK received
     * @param src_addr Source address
     */
    void recordAckReceived(uint16_t src_addr);
    
    /**
     * @brief Record a timeout
     * @param dst_addr Destination address that timed out
     * @param criticality Message criticality level
     */
    void recordTimeout(uint16_t dst_addr, DeliveryCriticality criticality);
    
    /**
     * @brief Record an invalid message
     */
    void recordInvalidMessage();
    
    /**
     * @brief Record a broadcast message
     */
    void recordBroadcast();
    
    /**
     * @brief Update node counts
     * @param registered Total registered nodes
     * @param active Active nodes
     * @param inactive Inactive nodes
     */
    void updateNodeCounts(uint32_t registered, uint32_t active, uint32_t inactive);
    
    /**
     * @brief Get statistics for a specific node
     * @param address Node address
     * @return Pointer to node statistics (nullptr if not found)
     */
    const NodeStatistics* getNodeStats(uint16_t address) const;
    
    /**
     * @brief Get global statistics
     * @return Reference to global statistics
     */
    const GlobalStatistics& getGlobalStats() const { return global_stats_; }
    
    /**
     * @brief Print summary statistics
     */
    void printSummary() const;
    
    /**
     * @brief Print detailed statistics for a specific node
     * @param address Node address
     */
    void printNodeStats(uint16_t address) const;
    
    /**
     * @brief Reset all statistics
     */
    void reset();

private:
    std::map<uint16_t, NodeStatistics> node_stats_;
    GlobalStatistics global_stats_;
    Logger logger_;
    
    /**
     * @brief Get or create node statistics
     * @param address Node address
     * @return Reference to node statistics
     */
    NodeStatistics& getOrCreateNodeStats(uint16_t address);
    
    /**
     * @brief Update link quality state
     * @param stats Node statistics to update
     * @param rssi New RSSI value
     * @param current_time Current timestamp
     */
    void updateLinkQuality(NodeStatistics& stats, int16_t rssi, uint32_t current_time);
};