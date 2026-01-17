#include "hub_router.h"
#include "../utils/time_utils.h"
#include "../hal/logger.h"
#include "pico/stdlib.h"
#include <cstring>

static Logger logger("HubRouter");

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
        logger.info("Routing message from 0x%04X to 0x%04X",
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
        logger.debug("Destination 0x%04X not reachable, queueing message", destination_address);
        
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
        logger.debug("Successfully routed message to 0x%04X", destination_address);
    } else {
        logger.warn("Failed to route message to 0x%04X, queueing", destination_address);
        
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
        logger.info("Node 0x%04X came online", node_address);
        
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
    logger.info("Node 0x%04X went offline", node_address);
    
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
            logger.debug("Message to 0x%04X expired, dropping", msg.destination_address);
            total_messages_dropped_++;
            continue;
        }

        // Check if destination is now reachable
        if (isDestinationReachable(msg.destination_address)) {
            logger.debug("Retrying queued message to 0x%04X", msg.destination_address);

            bool success = messenger_.send(msg.buffer, msg.length, BEST_EFFORT);

            if (success) {
                total_messages_routed_++;
                logger.debug("Successfully sent queued message to 0x%04X", msg.destination_address);

                // Update routing table
                RouteEntry& route = routing_table_[msg.destination_address];
                route.last_used_time = current_time;
            } else {
                // Increment retry count and re-queue if under limit
                msg.retry_count++;
                if (msg.retry_count < MAX_RETRY_COUNT) {
                    retry_queue.push(msg);
                } else {
                    logger.warn("Max retries reached for message to 0x%04X, dropping",
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
    logger.info("=== Routing Table ===");
    logger.info("Destination | Next Hop | Hops | Direct | Last Used");
    logger.info("------------|----------|------|--------|----------");

    for (const auto& entry : routing_table_) {
        const RouteEntry& route = entry.second;
        logger.info("0x%04X      | 0x%04X   | %d    | %s     | %lu",
               route.destination_address,
               route.next_hop_address,
               route.hop_count,
               route.is_direct ? "Yes" : "No",
               route.last_used_time);
    }

    logger.info("=====================");
    logger.info("Messages routed: %lu, queued: %lu, dropped: %lu",
           total_messages_routed_, total_messages_queued_, total_messages_dropped_);
}

uint32_t HubRouter::clearOldRoutes(uint32_t current_time, uint32_t max_age_ms) {
    uint32_t cleared_count = 0;
    
    auto it = routing_table_.begin();
    while (it != routing_table_.end()) {
        // Check if node has been inactive for the offline timeout period
        bool should_mark_offline = (current_time - it->second.last_online_time > NODE_OFFLINE_TIMEOUT_MS);

        if (should_mark_offline && it->second.is_online) {
            logger.info("Node 0x%04X went offline (inactive for %d minutes)",
                   it->first, NODE_OFFLINE_TIMEOUT_MS / (60 * 1000));
            it->second.is_online = false;
        }

        // Only remove very old routes (much older than offline timeout)
        if (current_time - it->second.last_used_time > max_age_ms) {
            logger.debug("Clearing old route to 0x%04X", it->first);
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
        logger.warn("Message queue full, dropping oldest message");
        
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
    
    logger.debug("Queued message for 0x%04X (queue size: %zu)",
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
        logger.debug("Removed %lu expired messages from queue", removed_count);
    }

    return removed_count;
}

// ===== Update Queue Management =====

bool HubRouter::queueScheduleUpdate(uint16_t node_addr, uint8_t index,
                                   const PMU::ScheduleEntry& entry) {
    auto& state = node_updates_[node_addr];

    // Check queue size limit
    if (state.pending_updates.size() >= MAX_UPDATES_PER_NODE) {
        logger.warn("Update queue full for node 0x%04X", node_addr);
        return false;
    }

    // Create update
    PendingUpdate update;
    update.type = UpdateType::SET_SCHEDULE;
    update.queued_at_ms = TimeUtils::getCurrentTimeMs();
    update.sequence = state.next_sequence++;

    // Pack schedule data (8 bytes)
    update.data[0] = index;
    update.data[1] = entry.hour;
    update.data[2] = entry.minute;
    update.data[3] = entry.duration & 0xFF;
    update.data[4] = (entry.duration >> 8) & 0xFF;
    update.data[5] = static_cast<uint8_t>(entry.daysMask);
    update.data[6] = entry.valveId;
    update.data[7] = entry.enabled ? 1 : 0;
    update.data_length = 8;

    // Add to queue
    state.pending_updates.push(update);

    logger.debug("Queued schedule update for node 0x%04X (seq=%d, pos=%zu)",
           node_addr, update.sequence, state.pending_updates.size());

    return true;
}

bool HubRouter::queueRemoveSchedule(uint16_t node_addr, uint8_t index) {
    auto& state = node_updates_[node_addr];

    if (state.pending_updates.size() >= MAX_UPDATES_PER_NODE) {
        logger.warn("Update queue full for node 0x%04X", node_addr);
        return false;
    }

    PendingUpdate update;
    update.type = UpdateType::REMOVE_SCHEDULE;
    update.queued_at_ms = TimeUtils::getCurrentTimeMs();
    update.sequence = state.next_sequence++;
    update.data[0] = index;
    update.data_length = 1;

    state.pending_updates.push(update);

    logger.debug("Queued schedule removal for node 0x%04X (seq=%d, index=%d)",
           node_addr, update.sequence, index);

    return true;
}

bool HubRouter::queueDateTimeUpdate(uint16_t node_addr, const PMU::DateTime& datetime) {
    auto& state = node_updates_[node_addr];

    if (state.pending_updates.size() >= MAX_UPDATES_PER_NODE) {
        logger.warn("Update queue full for node 0x%04X", node_addr);
        return false;
    }

    PendingUpdate update;
    update.type = UpdateType::SET_DATETIME;
    update.queued_at_ms = TimeUtils::getCurrentTimeMs();
    update.sequence = state.next_sequence++;

    // Pack datetime data (7 bytes)
    update.data[0] = datetime.year;
    update.data[1] = datetime.month;
    update.data[2] = datetime.day;
    update.data[3] = datetime.weekday;
    update.data[4] = datetime.hour;
    update.data[5] = datetime.minute;
    update.data[6] = datetime.second;
    update.data_length = 7;

    state.pending_updates.push(update);

    logger.debug("Queued datetime update for node 0x%04X (seq=%d)",
           node_addr, update.sequence);

    return true;
}

bool HubRouter::queueWakeIntervalUpdate(uint16_t node_addr, uint16_t interval_seconds) {
    auto& state = node_updates_[node_addr];

    if (state.pending_updates.size() >= MAX_UPDATES_PER_NODE) {
        logger.warn("Update queue full for node 0x%04X", node_addr);
        return false;
    }

    PendingUpdate update;
    update.type = UpdateType::SET_WAKE_INTERVAL;
    update.queued_at_ms = TimeUtils::getCurrentTimeMs();
    update.sequence = state.next_sequence++;

    // Pack interval data (2 bytes)
    update.data[0] = interval_seconds & 0xFF;
    update.data[1] = (interval_seconds >> 8) & 0xFF;
    update.data_length = 2;

    state.pending_updates.push(update);

    logger.debug("Queued wake interval update for node 0x%04X (seq=%d, interval=%d)",
           node_addr, update.sequence, interval_seconds);

    return true;
}

void HubRouter::handleCheckUpdates(uint16_t node_addr, uint8_t node_sequence) {
    auto& state = node_updates_[node_addr];
    state.last_check_time = TimeUtils::getCurrentTimeMs();

    // Remove updates that the node has already processed
    // Node sequence indicates the last update the node successfully applied
    while (!state.pending_updates.empty()) {
        const PendingUpdate& front = state.pending_updates.front();
        if (front.sequence <= node_sequence) {
            logger.debug("Node 0x%04X: Already processed seq=%d, removing from queue",
                   node_addr, front.sequence);
            state.pending_updates.pop();
        } else {
            break;  // Found an update the node hasn't seen yet
        }
    }

    // Check if queue is empty after cleanup
    if (state.pending_updates.empty()) {
        logger.debug("Node 0x%04X: No updates available", node_addr);

        // Send empty response (has_update=0)
        uint8_t buffer[MESSAGE_MAX_SIZE];
        Message* msg = reinterpret_cast<Message*>(buffer);

        // Create header
        msg->header.magic = MESSAGE_MAGIC;
        msg->header.type = MSG_TYPE_UPDATE_AVAILABLE;
        msg->header.flags = MSG_FLAG_RELIABLE;
        msg->header.src_addr = ADDRESS_HUB;
        msg->header.dst_addr = node_addr;
        msg->header.seq_num = 0;  // Messenger will set proper sequence number

        // Set payload
        UpdateAvailablePayload* payload = reinterpret_cast<UpdateAvailablePayload*>(msg->payload);
        payload->has_update = 0;
        payload->update_type = 0;
        payload->sequence = 0;
        memset(payload->payload_data, 0, sizeof(payload->payload_data));

        size_t length = MESSAGE_HEADER_SIZE + sizeof(UpdateAvailablePayload);
        messenger_.send(buffer, length, RELIABLE);
        return;
    }

    // Get next update (peek, don't remove - will be removed on next CHECK_UPDATES)
    const PendingUpdate& update = state.pending_updates.front();

    logger.debug("Node 0x%04X: Sending update seq=%d type=%d (node at seq=%d)",
           node_addr, update.sequence, static_cast<int>(update.type), node_sequence);

    // Send update
    uint8_t buffer[MESSAGE_MAX_SIZE];
    Message* msg = reinterpret_cast<Message*>(buffer);

    // Create header
    msg->header.magic = MESSAGE_MAGIC;
    msg->header.type = MSG_TYPE_UPDATE_AVAILABLE;
    msg->header.flags = MSG_FLAG_RELIABLE;
    msg->header.src_addr = ADDRESS_HUB;
    msg->header.dst_addr = node_addr;
    msg->header.seq_num = 0;  // Messenger will set proper sequence number

    // Set payload
    UpdateAvailablePayload* payload = reinterpret_cast<UpdateAvailablePayload*>(msg->payload);
    payload->has_update = 1;
    payload->update_type = static_cast<uint8_t>(update.type);
    payload->sequence = update.sequence;
    memcpy(payload->payload_data, update.data, update.data_length);
    // Zero out remaining bytes
    if (update.data_length < sizeof(payload->payload_data)) {
        memset(payload->payload_data + update.data_length, 0,
               sizeof(payload->payload_data) - update.data_length);
    }

    size_t length = MESSAGE_HEADER_SIZE + sizeof(UpdateAvailablePayload);
    messenger_.send(buffer, length, RELIABLE);
}

void HubRouter::handleUpdateAck(uint16_t node_addr, uint8_t sequence,
                               bool success, uint8_t error_code) {
    auto it = node_updates_.find(node_addr);
    if (it == node_updates_.end() || it->second.pending_updates.empty()) {
        logger.warn("Node 0x%04X: ACK for unknown update seq=%d", node_addr, sequence);
        return;
    }

    auto& state = it->second;
    const PendingUpdate& update = state.pending_updates.front();

    // Verify sequence matches
    if (update.sequence != sequence) {
        logger.warn("Node 0x%04X: ACK sequence mismatch (expected %d, got %d)",
               node_addr, update.sequence, sequence);
        return;
    }

    if (success) {
        logger.info("Node 0x%04X: Update seq=%d applied successfully",
               node_addr, sequence);

        // Remove from queue
        state.pending_updates.pop();
    } else {
        logger.error("Node 0x%04X: Update seq=%d failed (error=%d)",
               node_addr, sequence, error_code);

        // TODO: Retry logic or remove and report failure
        state.pending_updates.pop();
    }
}

size_t HubRouter::getPendingUpdateCount(uint16_t node_addr) const {
    auto it = node_updates_.find(node_addr);
    return (it != node_updates_.end()) ? it->second.pending_updates.size() : 0;
}

bool HubRouter::getPendingUpdate(uint16_t node_addr, size_t index, PendingUpdate& out_update) const {
    auto it = node_updates_.find(node_addr);
    if (it == node_updates_.end()) {
        return false;
    }

    // std::queue doesn't support indexing, so we need to make a copy and iterate
    auto queue_copy = it->second.pending_updates;

    // Skip to the requested index
    for (size_t i = 0; i < index && !queue_copy.empty(); i++) {
        queue_copy.pop();
    }

    if (queue_copy.empty()) {
        return false;
    }

    out_update = queue_copy.front();
    return true;
}

void HubRouter::clearPendingUpdates(uint16_t node_addr) {
    auto it = node_updates_.find(node_addr);
    if (it != node_updates_.end()) {
        size_t count = it->second.pending_updates.size();
        while (!it->second.pending_updates.empty()) {
            it->second.pending_updates.pop();
        }
        logger.debug("Cleared %zu pending updates for node 0x%04X", count, node_addr);
    }
}

void HubRouter::printQueueStats() const {
    logger.info("=== Update Queue Stats ===");

    size_t total_updates = 0;
    for (const auto& entry : node_updates_) {
        total_updates += entry.second.pending_updates.size();
    }

    logger.info("Total nodes with updates: %zu", node_updates_.size());
    logger.info("Total pending updates: %zu", total_updates);

    for (const auto& entry : node_updates_) {
        if (!entry.second.pending_updates.empty()) {
            logger.info("  Node 0x%04X: %zu updates (next_seq=%d)",
                   entry.first, entry.second.pending_updates.size(),
                   entry.second.next_sequence);
        }
    }

    logger.info("=========================");
}

