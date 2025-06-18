#include "reliable_messenger.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

ReliableMessenger::ReliableMessenger(SX1276* lora, uint16_t node_addr)
    : lora_(lora), node_addr_(node_addr), next_seq_num_(1) {
}

bool ReliableMessenger::sendActuatorCommand(uint16_t dst_addr, uint8_t actuator_type, uint8_t command,
                                           const uint8_t* params, uint8_t param_length, 
                                           DeliveryCriticality criticality) {
    if (!lora_) return false;
    
    // Convert criticality to flags
    uint8_t flags = 0;
    if (criticality == RELIABLE) flags |= MSG_FLAG_RELIABLE;
    if (criticality == CRITICAL) flags |= (MSG_FLAG_RELIABLE | MSG_FLAG_CRITICAL);
    
    uint8_t seq_num = next_seq_num_++;
    
    // Create the actuator message with appropriate flags
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length = MessageHandler::createActuatorMessage(
        node_addr_, dst_addr, seq_num,
        actuator_type, command, params, param_length, flags,
        buffer
    );
    
    if (length == 0) {
        printf("Failed to create actuator message\n");
        return false;
    }
    
    return send(buffer, length, criticality);
}

bool ReliableMessenger::sendSensorData(uint16_t dst_addr, uint8_t sensor_type, 
                                      const uint8_t* data, uint8_t data_length,
                                      DeliveryCriticality criticality) {
    if (!lora_) return false;
    
    // Convert criticality to flags
    uint8_t flags = 0;
    if (criticality == RELIABLE) flags |= MSG_FLAG_RELIABLE;
    if (criticality == CRITICAL) flags |= (MSG_FLAG_RELIABLE | MSG_FLAG_CRITICAL);
    
    uint8_t seq_num = next_seq_num_++;
    
    // Create the sensor message with appropriate flags
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length = MessageHandler::createSensorMessage(
        node_addr_, dst_addr, seq_num,
        sensor_type, data, data_length, flags,
        buffer
    );
    
    if (length == 0) {
        printf("Failed to create sensor message\n");
        return false;
    }
    
    return send(buffer, length, criticality);
}

bool ReliableMessenger::send(const uint8_t* buffer, size_t length, 
                            DeliveryCriticality criticality) {
    if (!lora_ || !buffer || length == 0) return false;
    
    // Send immediately
    if (!sendMessage(buffer, length)) {
        printf("Failed to send message\n");
        return false;
    }
    
    // For BEST_EFFORT, we're done - fire and forget
    if (criticality == BEST_EFFORT) {
        printf("Sent message (best effort)\n");
        return true;
    }
    
    // For RELIABLE and CRITICAL, add to pending messages for ACK tracking
    Message msg;
    if (!MessageHandler::parseMessage(buffer, length, &msg)) {
        printf("Failed to parse sent message for tracking\n");
        return false;
    }
    
    PendingMessage pending;
    memcpy(pending.buffer, buffer, length);
    pending.length = length;
    pending.seq_num = msg.header.seq_num;
    pending.dst_addr = msg.header.dst_addr;
    pending.last_send_time = getCurrentTime();
    pending.retry_count = 0;
    pending.next_retry_time = getCurrentTime() + ACK_TIMEOUT_MS;
    pending.ack_received = false;
    
    pending_messages_[msg.header.seq_num] = pending;
    
    const char* level = (criticality == CRITICAL) ? "CRITICAL" : "RELIABLE";
    printf("Sent %s message (seq=%d) to 0x%04X, awaiting ACK\n", 
           level, msg.header.seq_num, msg.header.dst_addr);
    
    return true;
}

bool ReliableMessenger::processIncomingMessage(const uint8_t* buffer, size_t length) {
    if (!buffer || length == 0) return false;
    
    Message message;
    if (!MessageHandler::parseMessage(buffer, length, &message)) {
        printf("Failed to parse incoming message\n");
        return false;
    }
    
    printf("Received message type %d from 0x%04X\n", message.header.type, message.header.src_addr);
    
    // Handle ACK messages
    if (message.header.type == MSG_TYPE_ACK) {
        const AckPayload* ack = MessageHandler::getAckPayload(&message);
        if (ack) {
            handleAck(ack);
            return true;
        }
    }
    
    // Handle any message that requires ACK based on flags
    if (MessageHandler::requiresAck(&message)) {
        printf("Received %s message requiring ACK\n", 
               MessageHandler::isCritical(&message) ? "CRITICAL" : "RELIABLE");
        
        // Send ACK back to sender
        sendAck(message.header.src_addr, message.header.seq_num, 0);
    }
    
    // Handle specific message types
    if (message.header.type == MSG_TYPE_ACTUATOR_CMD) {
        const ActuatorPayload* actuator = MessageHandler::getActuatorPayload(&message);
        if (actuator) {
            printf("Received actuator command: type=%d, cmd=%d\n", 
                   actuator->actuator_type, actuator->command);
            
            // TODO: Execute the actuator command here
            // For now, just log receipt
            
            return true;
        }
    }
    
    if (message.header.type == MSG_TYPE_SENSOR_DATA) {
        const SensorPayload* sensor = MessageHandler::getSensorPayload(&message);
        if (sensor) {
            printf("Received sensor data: type=%d, len=%d\n", 
                   sensor->sensor_type, sensor->data_length);
            return true;
        }
    }
    
    return false;
}

void ReliableMessenger::update() {
    uint32_t current_time = getCurrentTime();
    
    // Check for messages that need retry
    auto it = pending_messages_.begin();
    while (it != pending_messages_.end()) {
        PendingMessage& pending = it->second;
        
        // Remove acknowledged messages
        if (pending.ack_received) {
            printf("Message seq=%d acknowledged, removing from pending\n", pending.seq_num);
            it = pending_messages_.erase(it);
            continue;
        }
        
        // Check if retry time reached
        if (current_time >= pending.next_retry_time) {
            // Parse message to check criticality
            Message msg;
            bool is_critical = false;
            if (MessageHandler::parseMessage(pending.buffer, pending.length, &msg)) {
                is_critical = MessageHandler::isCritical(&msg);
            }
            
            // For non-critical messages, give up after MAX_RETRIES
            if (!is_critical && pending.retry_count >= MAX_RETRIES) {
                printf("Message seq=%d failed after %d retries, giving up\n", 
                       pending.seq_num, MAX_RETRIES);
                it = pending_messages_.erase(it);
                continue;
            }
            
            // For critical messages, keep trying but with longer delays
            if (is_critical && pending.retry_count >= MAX_RETRIES) {
                printf("CRITICAL message seq=%d still failing (attempt %d), continuing...\n", 
                       pending.seq_num, pending.retry_count + 1);
            }
            
            // Retry the message
            pending.retry_count++;
            pending.last_send_time = current_time;
            pending.next_retry_time = current_time + calculateRetryDelay(pending.retry_count);
            
            printf("Retrying message seq=%d (attempt %d/%d)\n", 
                   pending.seq_num, pending.retry_count, MAX_RETRIES);
            
            if (!sendMessage(pending.buffer, pending.length)) {
                printf("Failed to resend message seq=%d\n", pending.seq_num);
            }
        }
        
        ++it;
    }
}

bool ReliableMessenger::wasAcknowledged(uint8_t seq_num) {
    auto it = pending_messages_.find(seq_num);
    if (it == pending_messages_.end()) {
        return false; // Message not found (might have been removed after ACK)
    }
    return it->second.ack_received;
}

bool ReliableMessenger::sendMessage(const uint8_t* buffer, size_t length) {
    if (!lora_ || !buffer || length == 0) return false;
    
    if (!lora_->send(buffer, length)) {
        return false;
    }
    
    // Wait for transmission to complete
    while (!lora_->isTxDone()) {
        sleep_ms(10);
    }
    
    // Return to receive mode
    lora_->startReceive();
    
    return true;
}

void ReliableMessenger::handleAck(const AckPayload* ack_payload) {
    if (!ack_payload) return;
    
    auto it = pending_messages_.find(ack_payload->ack_seq_num);
    if (it != pending_messages_.end()) {
        it->second.ack_received = true;
        printf("Received ACK for seq=%d, status=%d\n", 
               ack_payload->ack_seq_num, ack_payload->status);
    } else {
        printf("Received ACK for unknown seq=%d\n", ack_payload->ack_seq_num);
    }
}

void ReliableMessenger::sendAck(uint16_t src_addr, uint8_t seq_num, uint8_t status) {
    uint8_t my_seq = next_seq_num_++;
    
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length = MessageHandler::createAckMessage(
        node_addr_, src_addr, my_seq,
        seq_num, status,
        buffer
    );
    
    if (length > 0) {
        sendMessage(buffer, length);
        printf("Sent ACK for seq=%d to 0x%04X\n", seq_num, src_addr);
    } else {
        printf("Failed to create ACK message\n");
    }
}

uint32_t ReliableMessenger::calculateRetryDelay(uint8_t retry_count) {
    // Exponential backoff: base_delay * 2^retry_count
    uint32_t delay = RETRY_BASE_DELAY_MS;
    for (uint8_t i = 0; i < retry_count && i < 4; i++) {
        delay *= 2;
    }
    return delay;
}

uint32_t ReliableMessenger::getCurrentTime() {
    return to_ms_since_boot(get_absolute_time());
}