#include "hub_router.h"
#include "../utils/time_utils.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <cstring>

HubRouter::HubRouter(AddressManager& address_manager, ReliableMessenger& messenger)
    : address_manager_(address_manager)
    , messenger_(messenger)
    , total_messages_routed_(0)
    , total_messages_queued_(0)
    , total_messages_dropped_(0) {
}

bool HubRouter::processMessage(const uint8_t* buffer, size_t length, uint16_t source_address) {
    if (!buffer || length < sizeof(MessageHeader)) {
        return false;
    }
    
    const MessageHeader* header = reinterpret_cast<const MessageHeader*>(buffer);
    
    // Check if this message needs to be routed to another node
    if (header->dst_addr != ADDRESS_HUB && 
        header->dst_addr != ADDRESS_BROADCAST) {
        
        // This is a node-to-node message that needs routing
        printf("Routing message from 0x%04X to 0x%04X\n", 
               source_address, header->dst_addr);
        
        return forwardMessage(buffer, length, header->dst_addr, source_address);
    }
    
    // Message is for the hub itself - no routing needed
    return false;
}

bool HubRouter::forwardMessage(const uint8_t* buffer, size_t length, 
                              uint16_t destination_address, uint16_t source_address) {
    
    // Check if destination is reachable
    if (!isDestinationReachable(destination_address)) {
        printf("Destination 0x%04X not reachable, queueing message\n", destination_address);
        
        // Determine if message requires ACK based on flags
        const MessageHeader* header = reinterpret_cast<const MessageHeader*>(buffer);
        bool requires_ack = (header->flags & (MSG_FLAG_RELIABLE | MSG_FLAG_CRITICAL)) != 0;
        
        return queueMessage(buffer, length, destination_address, requires_ack);
    }
    
    // Update routing table with successful path
    RouteEntry& route = routing_table_[destination_address];
    route.destination_address = destination_address;
    route.next_hop_address = destination_address;  // Direct connection for now
    route.last_used_time = TimeUtils::getCurrentTimeMs();
    route.hop_count = 1;
    route.is_direct = true;
    
    // Forward the message directly using the send method
    bool success = messenger_.send(buffer, length, BEST_EFFORT);
    
    if (success) {
        total_messages_routed_++;
        printf("Successfully routed message to 0x%04X\n", destination_address);
    } else {
        printf("Failed to route message to 0x%04X, queueing\n", destination_address);
        
        // If direct send failed, queue for retry
        const MessageHeader* header = reinterpret_cast<const MessageHeader*>(buffer);
        bool requires_ack = (header->flags & (MSG_FLAG_RELIABLE | MSG_FLAG_CRITICAL)) != 0;
        return queueMessage(buffer, length, destination_address, requires_ack);
    }
    
    return success;
}

void HubRouter::updateRouteOnline(uint16_t node_address) {
    uint32_t current_time = TimeUtils::getCurrentTimeMs();
    
    // Get or create routing table entry
    RouteEntry& route = routing_table_[node_address];
    
    // Check if this is a transition from offline to online
    bool was_offline = !route.is_online || 
                      (current_time - route.last_online_time > NODE_OFFLINE_TIMEOUT_MS);
    
    // Only log "came online" if node was previously offline or unknown
    if (was_offline || route.last_online_time == 0) {
        printf("Node 0x%04X came online\n", node_address);
        
        // Process any queued messages for this node since it's now available
        processQueuedMessages();
    }
    
    // Update routing table
    route.destination_address = node_address;
    route.next_hop_address = node_address;  // Direct connection
    route.last_used_time = current_time;
    route.last_online_time = current_time;
    route.hop_count = 1;
    route.is_direct = true;
    route.is_online = true;
}

void HubRouter::updateRouteOffline(uint16_t node_address) {
    printf("Node 0x%04X went offline\n", node_address);
    
    // Remove from routing table
    routing_table_.erase(node_address);
}

void HubRouter::processQueuedMessages() {
    if (message_queue_.empty()) {
        return;
    }
    
    uint32_t current_time = TimeUtils::getCurrentTimeMs();
    std::queue<QueuedMessage> retry_queue;
    
    // Process all queued messages
    while (!message_queue_.empty()) {
        QueuedMessage msg = message_queue_.front();
        message_queue_.pop();
        
        // Check if message has expired
        if (current_time - msg.queued_time > MESSAGE_TIMEOUT_MS) {
            printf("Message to 0x%04X expired, dropping\n", msg.destination_address);
            total_messages_dropped_++;
            continue;
        }
        
        // Check if destination is now reachable
        if (isDestinationReachable(msg.destination_address)) {
            printf("Retrying queued message to 0x%04X\n", msg.destination_address);
            
            bool success = messenger_.send(msg.buffer, msg.length, BEST_EFFORT);
            
            if (success) {
                total_messages_routed_++;
                printf("Successfully sent queued message to 0x%04X\n", msg.destination_address);
                
                // Update routing table
                RouteEntry& route = routing_table_[msg.destination_address];
                route.last_used_time = current_time;
            } else {
                // Increment retry count and re-queue if under limit
                msg.retry_count++;
                if (msg.retry_count < MAX_RETRY_COUNT) {
                    retry_queue.push(msg);
                } else {
                    printf("Max retries reached for message to 0x%04X, dropping\n", 
                           msg.destination_address);
                    total_messages_dropped_++;
                }
            }
        } else {
            // Destination still not reachable, re-queue
            retry_queue.push(msg);
        }
    }
    
    // Restore messages that need more retries
    message_queue_ = retry_queue;
    total_messages_queued_ = message_queue_.size();
}

void HubRouter::getRoutingStats(uint32_t& total_routed, uint32_t& total_queued, uint32_t& total_dropped) {
    total_routed = total_messages_routed_;
    total_queued = total_messages_queued_;
    total_dropped = total_messages_dropped_;
}

void HubRouter::printRoutingTable() {
    printf("\n=== Routing Table ===\n");
    printf("Destination | Next Hop | Hops | Direct | Last Used\n");
    printf("------------|----------|------|--------|----------\n");
    
    for (const auto& entry : routing_table_) {
        const RouteEntry& route = entry.second;
        printf("0x%04X      | 0x%04X   | %d    | %s     | %lu\n",
               route.destination_address,
               route.next_hop_address,
               route.hop_count,
               route.is_direct ? "Yes" : "No",
               route.last_used_time);
    }
    
    printf("=====================\n");
    printf("Messages routed: %lu, queued: %lu, dropped: %lu\n",
           total_messages_routed_, total_messages_queued_, total_messages_dropped_);
}

uint32_t HubRouter::clearOldRoutes(uint32_t current_time, uint32_t max_age_ms) {
    uint32_t cleared_count = 0;
    
    auto it = routing_table_.begin();
    while (it != routing_table_.end()) {
        // Check if node has been inactive for the offline timeout period
        bool should_mark_offline = (current_time - it->second.last_online_time > NODE_OFFLINE_TIMEOUT_MS);
        
        if (should_mark_offline && it->second.is_online) {
            printf("Node 0x%04X went offline (inactive for %d minutes)\n", 
                   it->first, NODE_OFFLINE_TIMEOUT_MS / (60 * 1000));
            it->second.is_online = false;
        }
        
        // Only remove very old routes (much older than offline timeout)
        if (current_time - it->second.last_used_time > max_age_ms) {
            printf("Clearing old route to 0x%04X\n", it->first);
            it = routing_table_.erase(it);
            cleared_count++;
        } else {
            ++it;
        }
    }
    
    return cleared_count;
}

bool HubRouter::isDestinationReachable(uint16_t destination_address) {
    // Check if we have a valid node registration for this address
    const NodeInfo* node_info = address_manager_.getNodeInfo(destination_address);
    if (!node_info) {
        return false;  // Node not registered
    }
    
    // For now, assume all registered nodes are reachable
    // In future versions, this could check last heartbeat time
    return true;
}

bool HubRouter::queueMessage(const uint8_t* buffer, size_t length, 
                            uint16_t destination_address, bool requires_ack) {
    
    // Check if queue is full
    if (message_queue_.size() >= MAX_QUEUE_SIZE) {
        printf("Message queue full, dropping oldest message\n");
        
        // Remove oldest message
        message_queue_.pop();
        total_messages_dropped_++;
    }
    
    // Create queued message
    QueuedMessage msg;
    memcpy(msg.buffer, buffer, length);
    msg.length = length;
    msg.destination_address = destination_address;
    msg.queued_time = TimeUtils::getCurrentTimeMs();
    msg.retry_count = 0;
    msg.requires_ack = requires_ack;
    
    // Add to queue
    message_queue_.push(msg);
    total_messages_queued_++;
    
    printf("Queued message for 0x%04X (queue size: %zu)\n", 
           destination_address, message_queue_.size());
    
    return true;
}

uint32_t HubRouter::removeExpiredMessages(uint32_t current_time) {
    uint32_t removed_count = 0;
    std::queue<QueuedMessage> new_queue;
    
    // Filter out expired messages
    while (!message_queue_.empty()) {
        QueuedMessage msg = message_queue_.front();
        message_queue_.pop();
        
        if (current_time - msg.queued_time <= MESSAGE_TIMEOUT_MS) {
            new_queue.push(msg);
        } else {
            removed_count++;
            total_messages_dropped_++;
        }
    }
    
    message_queue_ = new_queue;
    total_messages_queued_ = message_queue_.size();
    
    if (removed_count > 0) {
        printf("Removed %lu expired messages from queue\n", removed_count);
    }
    
    return removed_count;
}

