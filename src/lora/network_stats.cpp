#include "network_stats.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

NetworkStats::NetworkStats() : logger_("NetworkStats") {
    global_stats_.network_start_time = to_ms_since_boot(get_absolute_time());
}

void NetworkStats::recordMessageSent(uint16_t dst_addr, DeliveryCriticality criticality, 
                                   bool delivered, uint8_t retries) {
    // Skip hub and broadcast addresses for per-node stats
    if (dst_addr == ADDRESS_HUB || dst_addr == ADDRESS_BROADCAST) {
        if (dst_addr == ADDRESS_BROADCAST) {
            global_stats_.total_broadcasts++;
        }
    } else {
        NodeStatistics& node = getOrCreateNodeStats(dst_addr);
        MessageTypeStats& stats = node.getStatsByCriticality(criticality);
        
        stats.sent++;
        if (delivered) {
            stats.delivered++;
            stats.retries += retries;
            if (retries > stats.max_retries) {
                stats.max_retries = retries;
            }
        }
    }
    
    // Update global stats
    MessageTypeStats& global = (criticality == CRITICAL) ? global_stats_.critical_total :
                              (criticality == RELIABLE) ? global_stats_.reliable_total :
                              global_stats_.best_effort_total;
    
    global.sent++;
    if (delivered) {
        global.delivered++;
        global.retries += retries;
        if (retries > global.max_retries) {
            global.max_retries = retries;
        }
    }
}

void NetworkStats::recordMessageReceived(uint16_t src_addr, int16_t rssi, float snr, bool crc_error) {
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    
    // Update global counters
    if (crc_error) {
        global_stats_.total_crc_errors++;
    } else {
        global_stats_.total_messages_received++;
    }
    
    // Skip hub address for per-node stats
    if (src_addr == ADDRESS_HUB) {
        return;
    }
    
    NodeStatistics& node = getOrCreateNodeStats(src_addr);
    
    if (crc_error) {
        node.crc_errors++;
    } else {
        node.messages_received++;
        
        // Update signal statistics
        node.rssi_stats.add(rssi);
        node.snr_stats.add((int16_t)(snr * 10)); // Store SNR * 10 for precision
        
        // Update link quality
        updateLinkQuality(node, rssi, current_time);
    }
    
    node.last_seen_time = current_time;
    if (node.first_seen_time == 0) {
        node.first_seen_time = current_time;
    }
}

void NetworkStats::recordAckSent(uint16_t dst_addr) {
    global_stats_.total_acks_sent++;
    
    if (dst_addr != ADDRESS_HUB && dst_addr != ADDRESS_BROADCAST) {
        NodeStatistics& node = getOrCreateNodeStats(dst_addr);
        node.acks_sent++;
    }
}

void NetworkStats::recordAckReceived(uint16_t src_addr) {
    global_stats_.total_acks_received++;
    
    if (src_addr != ADDRESS_HUB && src_addr != ADDRESS_BROADCAST) {
        NodeStatistics& node = getOrCreateNodeStats(src_addr);
        node.acks_received++;
    }
}

void NetworkStats::recordTimeout(uint16_t dst_addr, DeliveryCriticality criticality) {
    // Update node stats
    if (dst_addr != ADDRESS_HUB && dst_addr != ADDRESS_BROADCAST) {
        NodeStatistics& node = getOrCreateNodeStats(dst_addr);
        MessageTypeStats& stats = node.getStatsByCriticality(criticality);
        stats.timeouts++;
    }
    
    // Update global stats
    MessageTypeStats& global = (criticality == CRITICAL) ? global_stats_.critical_total :
                              (criticality == RELIABLE) ? global_stats_.reliable_total :
                              global_stats_.best_effort_total;
    global.timeouts++;
}

void NetworkStats::recordInvalidMessage() {
    global_stats_.total_invalid_messages++;
}

void NetworkStats::recordBroadcast() {
    global_stats_.total_broadcasts++;
}

void NetworkStats::updateNodeCounts(uint32_t registered, uint32_t active, uint32_t inactive) {
    global_stats_.nodes_registered = registered;
    global_stats_.nodes_active = active;
    global_stats_.nodes_inactive = inactive;
}

const NodeStatistics* NetworkStats::getNodeStats(uint16_t address) const {
    auto it = node_stats_.find(address);
    if (it == node_stats_.end()) {
        return nullptr;
    }
    return &it->second;
}

void NetworkStats::printSummary() const {
    logger_.info("========== Network Statistics Summary ==========");
    logger_.info("Network Uptime: %lu seconds", global_stats_.getNetworkUptimeSeconds());
    logger_.info("Nodes: %u registered, %u active, %u inactive",
                 global_stats_.nodes_registered, global_stats_.nodes_active, global_stats_.nodes_inactive);
    
    logger_.info("Message Statistics:");
    logger_.info("  Total Sent: %lu (BE: %lu, R: %lu, C: %lu)",
                 global_stats_.getTotalMessagesSent(),
                 global_stats_.best_effort_total.sent,
                 global_stats_.reliable_total.sent,
                 global_stats_.critical_total.sent);
    
    logger_.info("  Total Delivered: %lu", global_stats_.getTotalDelivered());
    logger_.info("  Total Received: %lu", global_stats_.total_messages_received);
    logger_.info("  Broadcasts: %lu", global_stats_.total_broadcasts);
    
    logger_.info("Reliability Metrics:");
    logger_.info("  Overall Delivery Rate: %.1f%%", global_stats_.getOverallDeliveryRate());
    logger_.info("  Best Effort: %.1f%% delivery", global_stats_.best_effort_total.getDeliveryRate());
    logger_.info("  Reliable: %.1f%% delivery (%.2f avg retries)", 
                 global_stats_.reliable_total.getDeliveryRate(),
                 global_stats_.reliable_total.getAverageRetries());
    logger_.info("  Critical: %.1f%% delivery (%.2f avg retries, max %u)",
                 global_stats_.critical_total.getDeliveryRate(),
                 global_stats_.critical_total.getAverageRetries(),
                 global_stats_.critical_total.max_retries);
    
    logger_.info("Error Statistics:");
    logger_.info("  CRC Errors: %lu", global_stats_.total_crc_errors);
    logger_.info("  Invalid Messages: %lu", global_stats_.total_invalid_messages);
    logger_.info("  Timeouts: %lu (R: %lu, C: %lu)",
                 global_stats_.reliable_total.timeouts + global_stats_.critical_total.timeouts,
                 global_stats_.reliable_total.timeouts,
                 global_stats_.critical_total.timeouts);
    
    logger_.info("ACK Statistics:");
    logger_.info("  ACKs Sent: %lu", global_stats_.total_acks_sent);
    logger_.info("  ACKs Received: %lu", global_stats_.total_acks_received);
    logger_.info("===============================================");
}

void NetworkStats::printNodeStats(uint16_t address) const {
    const NodeStatistics* node = getNodeStats(address);
    if (!node) {
        logger_.warn("No statistics for node 0x%04X", address);
        return;
    }
    
    logger_.info("========== Node 0x%04X Statistics ==========", address);
    logger_.info("Uptime: %lu seconds", node->getUptimeSeconds());
    logger_.info("Last Seen: %lu ms ago", 
                 node->last_seen_time ? to_ms_since_boot(get_absolute_time()) - node->last_seen_time : 0);
    
    logger_.info("Message Statistics:");
    logger_.info("  Sent: %lu total", node->getTotalMessagesSent());
    logger_.info("    Best Effort: %lu sent, %.1f%% delivered",
                 node->best_effort_stats.sent, node->best_effort_stats.getDeliveryRate());
    logger_.info("    Reliable: %lu sent, %.1f%% delivered, %.2f avg retries",
                 node->reliable_stats.sent, node->reliable_stats.getDeliveryRate(),
                 node->reliable_stats.getAverageRetries());
    logger_.info("    Critical: %lu sent, %.1f%% delivered, %.2f avg retries, max %u",
                 node->critical_stats.sent, node->critical_stats.getDeliveryRate(),
                 node->critical_stats.getAverageRetries(), node->critical_stats.max_retries);
    
    logger_.info("  Received: %lu", node->messages_received);
    logger_.info("  Timeouts: %lu", node->getTotalTimeouts());
    logger_.info("  Retries: %lu", node->getTotalRetries());
    
    logger_.info("Link Quality: %s", node->getLinkQualityString());
    logger_.info("  Quality Changes: %lu", node->link_quality_changes);
    
    if (node->rssi_stats.getSampleCount() > 0) {
        logger_.info("RSSI Statistics (%zu samples):", node->rssi_stats.getSampleCount());
        logger_.info("  Current: %d dBm", node->rssi_stats.getMax()); // Most recent
        logger_.info("  Mean: %.1f dBm (σ=%.1f)", node->rssi_stats.getMean(), node->rssi_stats.getStdDev());
        logger_.info("  Range: %d to %d dBm", node->rssi_stats.getMin(), node->rssi_stats.getMax());
        logger_.info("  Percentiles - 10th: %d, 50th: %d, 90th: %d dBm",
                     node->rssi_stats.getPercentile(10),
                     node->rssi_stats.getPercentile(50),
                     node->rssi_stats.getPercentile(90));
        logger_.info("  Trend: %s", node->rssi_stats.isTrendingUp() ? "Improving" : "Stable/Declining");
    }
    
    if (node->snr_stats.getSampleCount() > 0) {
        logger_.info("SNR Statistics (%zu samples):", node->snr_stats.getSampleCount());
        logger_.info("  Mean: %.1f dB", node->snr_stats.getMean() / 10.0f);
        logger_.info("  Range: %.1f to %.1f dB", 
                     node->snr_stats.getMin() / 10.0f, node->snr_stats.getMax() / 10.0f);
    }
    
    logger_.info("Error Statistics:");
    logger_.info("  CRC Errors: %lu", node->crc_errors);
    logger_.info("  Invalid Messages: %lu", node->invalid_messages);
    
    logger_.info("ACK Statistics:");
    logger_.info("  ACKs Sent: %lu", node->acks_sent);
    logger_.info("  ACKs Received: %lu", node->acks_received);
    logger_.info("=========================================");
}

void NetworkStats::reset() {
    node_stats_.clear();
    global_stats_ = GlobalStatistics();
    global_stats_.network_start_time = to_ms_since_boot(get_absolute_time());
    logger_.info("Network statistics reset");
}

NodeStatistics& NetworkStats::getOrCreateNodeStats(uint16_t address) {
    auto it = node_stats_.find(address);
    if (it == node_stats_.end()) {
        NodeStatistics new_stats;
        new_stats.first_seen_time = to_ms_since_boot(get_absolute_time());
        node_stats_[address] = new_stats;
        return node_stats_[address];
    }
    return it->second;
}

void NetworkStats::updateLinkQuality(NodeStatistics& stats, int16_t rssi, uint32_t current_time) {
    LinkQuality new_quality = stats.calculateLinkQuality(rssi);
    
    if (new_quality != stats.current_link_quality) {
        stats.link_quality_changes++;
        stats.current_link_quality = new_quality;
        stats.time_entered_current_quality = current_time;
        
        logger_.debug("Node link quality changed to %s (RSSI: %d dBm)", 
                     stats.getLinkQualityString(), rssi);
    }
}