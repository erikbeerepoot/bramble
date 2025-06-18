#pragma once

#include "address_manager.h"
#include "reliable_messenger.h"
#include "message.h"
#include <queue>
#include <map>

/**
 * @brief Routing table entry for node-to-node communication
 */
struct RouteEntry {
    uint16_t destination_address;    // Final destination
    uint16_t next_hop_address;       // Next hop (for multi-hop routing - future)
    uint32_t last_used_time;         // When route was last used
    uint8_t hop_count;               // Number of hops to destination
    bool is_direct;                  // True if direct connection to destination
};

/**
 * @brief Queued message for forwarding
 */
struct QueuedMessage {
    uint8_t buffer[MESSAGE_MAX_SIZE];    // Message data
    size_t length;                       // Message length
    uint16_t destination_address;        // Final destination
    uint32_t queued_time;                // When message was queued
    uint8_t retry_count;                 // Number of forward attempts
    bool requires_ack;                   // Whether message needs ACK
};

/**
 * @brief Hub router for node-to-node message forwarding
 * 
 * Provides routing services for nodes to communicate with each other
 * via the hub. Supports both direct forwarding and queuing for
 * offline nodes.
 */
class HubRouter {
public:
    explicit HubRouter(AddressManager& address_manager, ReliableMessenger& messenger);
    
    /**
     * @brief Process incoming message and route if necessary
     * @param buffer Message buffer
     * @param length Message length
     * @param source_address Address of sender
     * @return true if message was processed/routed
     */
    bool processMessage(const uint8_t* buffer, size_t length, uint16_t source_address);
    
    /**
     * @brief Forward message to destination node
     * @param buffer Message buffer
     * @param length Message length
     * @param destination_address Final destination
     * @param source_address Original sender
     * @return true if forwarding successful or queued
     */
    bool forwardMessage(const uint8_t* buffer, size_t length, 
                       uint16_t destination_address, uint16_t source_address);
    
    /**
     * @brief Update routing table when node comes online
     * @param node_address Address of node that came online
     */
    void updateRouteOnline(uint16_t node_address);
    
    /**
     * @brief Update routing table when node goes offline
     * @param node_address Address of node that went offline
     */
    void updateRouteOffline(uint16_t node_address);
    
    /**
     * @brief Process queued messages for delivery
     * Should be called periodically to retry message delivery
     */
    void processQueuedMessages();
    
    /**
     * @brief Get routing statistics
     * @param total_routed Total messages routed
     * @param total_queued Total messages currently queued
     * @param total_dropped Total messages dropped
     */
    void getRoutingStats(uint32_t& total_routed, uint32_t& total_queued, uint32_t& total_dropped);
    
    /**
     * @brief Print routing table for debugging
     */
    void printRoutingTable();
    
    /**
     * @brief Clear old routes that haven't been used
     * @param current_time Current time in milliseconds
     * @param max_age_ms Maximum age for routes in milliseconds
     * @return Number of routes cleared
     */
    uint32_t clearOldRoutes(uint32_t current_time, uint32_t max_age_ms = 3600000); // 1 hour default
    
private:
    AddressManager& address_manager_;    // Reference to address manager
    ReliableMessenger& messenger_;       // Reference to reliable messenger
    
    std::map<uint16_t, RouteEntry> routing_table_;      // Destination -> Route mapping
    std::queue<QueuedMessage> message_queue_;           // Messages waiting for delivery
    
    // Statistics
    uint32_t total_messages_routed_;     // Total messages successfully routed
    uint32_t total_messages_queued_;     // Total messages currently queued
    uint32_t total_messages_dropped_;    // Total messages dropped (queue full, etc.)
    
    // Configuration
    static constexpr uint32_t MAX_QUEUE_SIZE = 50;          // Maximum queued messages
    static constexpr uint32_t MESSAGE_TIMEOUT_MS = 300000;  // 5 minutes message timeout
    static constexpr uint32_t MAX_RETRY_COUNT = 3;          // Maximum forward attempts
    
    /**
     * @brief Check if destination is reachable
     * @param destination_address Destination to check
     * @return true if destination is reachable
     */
    bool isDestinationReachable(uint16_t destination_address);
    
    /**
     * @brief Queue message for later delivery
     * @param buffer Message buffer
     * @param length Message length
     * @param destination_address Destination address
     * @param requires_ack Whether message requires ACK
     * @return true if queued successfully
     */
    bool queueMessage(const uint8_t* buffer, size_t length, 
                     uint16_t destination_address, bool requires_ack);
    
    /**
     * @brief Remove expired messages from queue
     * @param current_time Current time in milliseconds
     * @return Number of messages removed
     */
    uint32_t removeExpiredMessages(uint32_t current_time);
    
    /**
     * @brief Get current time in milliseconds
     */
    uint32_t getCurrentTime();
};