#include "reliable_messenger.h"
#include "../utils/time_utils.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

ReliableMessenger::ReliableMessenger(SX1276* lora, uint16_t node_addr, NetworkStats* stats)
    : lora_(lora), node_addr_(node_addr), logger_("ReliableMessenger"), network_stats_(stats) {
    
    // Use different sequence number ranges to prevent collisions
    // Hub uses 1-127, nodes (including unregistered) use 128-255
    if (node_addr == ADDRESS_HUB) {
        next_seq_num_ = 1;   // Hub: 1-127
    } else {
        next_seq_num_ = 128; // Nodes and unregistered: 128-255
    }
}

bool ReliableMessenger::sendActuatorCommand(uint16_t dst_addr, uint8_t actuator_type, uint8_t command,
                                           const uint8_t* params, uint8_t param_length,
                                           DeliveryCriticality criticality) {
    uint8_t flags = MessageBuilder::criticalityToFlags(criticality);
    uint8_t seq_num = getNextSequenceNumber();

    logger_.info("sendActuatorCommand: src=0x%04X dst=0x%04X type=%d cmd=%d params_len=%d",
                node_addr_, dst_addr, actuator_type, command, param_length);

    return sendWithBuilder([=](uint8_t* buffer) {
        size_t msg_size = MessageBuilder::createActuatorMessage(
            node_addr_, dst_addr, seq_num,
            actuator_type, command, params, param_length, flags,
            buffer
        );

        // Debug: Log the created message in detail
        if (msg_size > 0) {
            const Message* msg = reinterpret_cast<const Message*>(buffer);
            logger_.info("Created actuator msg: size=%zu", msg_size);
            logger_.info("  Header: magic=0x%04X type=%d flags=0x%02X src=0x%04X dst=0x%04X seq=%d",
                        msg->header.magic, msg->header.type, msg->header.flags,
                        msg->header.src_addr, msg->header.dst_addr, msg->header.seq_num);
            logger_.info("  Payload[0-7]: %02X %02X %02X %02X %02X %02X %02X %02X",
                        msg->payload[0], msg->payload[1], msg->payload[2], msg->payload[3],
                        msg->payload[4], msg->payload[5], msg->payload[6], msg->payload[7]);

            // Verify actuator payload structure
            const ActuatorPayload* actuator = reinterpret_cast<const ActuatorPayload*>(msg->payload);
            logger_.info("  Actuator: type=%d cmd=%d param_len=%d param[0]=%d",
                        actuator->actuator_type, actuator->command, actuator->param_length,
                        actuator->param_length > 0 ? actuator->params[0] : -1);

            // Check if size matches expected
            size_t expected = MESSAGE_HEADER_SIZE + 3 + param_length; // header + actuator header + params
            if (msg_size != expected) {
                logger_.error("Size mismatch! Created=%zu, expected=%zu", msg_size, expected);
            }
        }

        return msg_size;
    }, criticality, "actuator");
}

bool ReliableMessenger::sendSensorData(uint16_t dst_addr, uint8_t sensor_type, 
                                      const uint8_t* data, uint8_t data_length,
                                      DeliveryCriticality criticality) {
    uint8_t flags = MessageBuilder::criticalityToFlags(criticality);
    uint8_t seq_num = getNextSequenceNumber();
    
    logger_.debug("sendSensorData: src=0x%04X, dst=0x%04X, type=%d, criticality=%d", 
                  node_addr_, dst_addr, sensor_type, criticality);
    
    return sendWithBuilder([=](uint8_t* buffer) {
        return MessageBuilder::createSensorMessage(
            node_addr_, dst_addr, seq_num,
            sensor_type, data, data_length, flags,
            buffer
        );
    }, criticality, "sensor");
}

bool ReliableMessenger::sendHeartbeat(uint16_t dst_addr, uint32_t uptime_seconds, 
                                     uint8_t battery_level, uint8_t signal_strength,
                                     uint8_t active_sensors, uint8_t error_flags) {
    uint8_t seq_num = getNextSequenceNumber();
    
    return sendWithBuilder([=](uint8_t* buffer) {
        return MessageHandler::createHeartbeatMessage(
            node_addr_, dst_addr, seq_num,
            battery_level, signal_strength, uptime_seconds, error_flags,
            buffer
        );
    }, BEST_EFFORT, "heartbeat");
}

bool ReliableMessenger::sendRegistrationRequest(uint16_t dst_addr, uint64_t device_id,
                                              uint8_t node_type, uint8_t capabilities,
                                              uint16_t firmware_ver, const char* device_name) {
    uint8_t seq_num = getNextSequenceNumber();
    
    logger_.info("Sending registration request to hub (device_id=0x%016llX)", device_id);
    
    return sendWithBuilder([=](uint8_t* buffer) {
        return MessageHandler::createRegistrationMessage(
            node_addr_, dst_addr, seq_num,
            device_id, node_type, capabilities, firmware_ver, device_name,
            buffer
        );
    }, RELIABLE, "registration");
}

bool ReliableMessenger::sendRegistrationResponse(uint16_t dst_addr, uint64_t device_id,
                                               uint16_t assigned_addr, uint8_t status,
                                               uint8_t retry_interval, uint32_t network_time) {
    uint8_t seq_num = getNextSequenceNumber();

    logger_.info("Sending registration response to 0x%04X (assigned_addr=0x%04X, status=%d)",
           dst_addr, assigned_addr, status);

    return sendWithBuilder([=](uint8_t* buffer) {
        return MessageHandler::createRegistrationResponse(
            node_addr_, dst_addr, seq_num,
            device_id, assigned_addr, status, retry_interval, network_time,
            buffer
        );
    }, RELIABLE, "registration response");
}

bool ReliableMessenger::sendCheckUpdates(uint16_t dst_addr, uint8_t node_sequence) {
    uint8_t seq_num = getNextSequenceNumber();

    logger_.info("Sending CHECK_UPDATES to 0x%04X (node_seq=%d)", dst_addr, node_sequence);

    return sendWithBuilder([=](uint8_t* buffer) {
        Message* msg = reinterpret_cast<Message*>(buffer);
        msg->header.magic = MESSAGE_MAGIC;
        msg->header.type = MSG_TYPE_CHECK_UPDATES;
        msg->header.src_addr = node_addr_;
        msg->header.dst_addr = dst_addr;
        msg->header.flags = MSG_FLAG_RELIABLE;
        msg->header.seq_num = seq_num;

        CheckUpdatesPayload* payload = reinterpret_cast<CheckUpdatesPayload*>(msg->payload);
        payload->node_sequence = node_sequence;

        return sizeof(MessageHeader) + sizeof(CheckUpdatesPayload);
    }, RELIABLE, "check_updates");
}

bool ReliableMessenger::send(const uint8_t* buffer, size_t length,
                            DeliveryCriticality criticality) {
    if (!lora_ || !buffer || length == 0) return false;
    
    // Send immediately
    if (!sendMessage(buffer, length)) {
        logger_.error("Failed to send message");
        return false;
    }
    
    // For BEST_EFFORT, we're done - fire and forget
    if (criticality == BEST_EFFORT) {
        logger_.info("Sent message (%s)", RetryPolicy::getPolicyName(criticality));
        return true;
    }
    
    // For RELIABLE and CRITICAL, add to pending messages for ACK tracking
    if (!MessageValidator::validateMessage(buffer, length)) {
        logger_.error("Invalid message for tracking");
        return false;
    }
    
    const Message* msg = reinterpret_cast<const Message*>(buffer);
    
    PendingMessage pending;
    pending.buffer = std::make_unique<uint8_t[]>(length);
    memcpy(pending.buffer.get(), buffer, length);
    pending.length = length;
    pending.dst_addr = msg->header.dst_addr;
    pending.send_time = TimeUtils::getCurrentTimeMs();
    pending.attempts = 0;
    pending.criticality = criticality;
    pending.retry_config = RetryPolicy::getConfig(criticality);
    
    pending_messages_[msg->header.seq_num] = std::move(pending);
    
    logger_.info("Sent %s message (seq=%d) to 0x%04X, awaiting ACK", 
                RetryPolicy::getPolicyName(criticality), msg->header.seq_num, msg->header.dst_addr);
    
    return true;
}

bool ReliableMessenger::processIncomingMessage(const uint8_t* buffer, size_t length) {
    if (!MessageValidator::validateMessage(buffer, length)) {
        logger_.error("Invalid incoming message");
        return false;
    }

    const Message* message = reinterpret_cast<const Message*>(buffer);

    // Check if message is addressed to us or is a broadcast
    if (message->header.dst_addr != node_addr_ &&
        message->header.dst_addr != ADDRESS_BROADCAST) {
        // Message not for us - ignore it
        logger_.debug("Ignoring message for 0x%04X (our address: 0x%04X)",
                     message->header.dst_addr, node_addr_);
        return false;
    }

    logger_.info("Received %s message from 0x%04X to 0x%04X",
                MessageHandler::getMessageTypeName(message->header.type),
                message->header.src_addr, message->header.dst_addr);
    
    // Handle ACK messages
    if (message->header.type == MSG_TYPE_ACK) {
        const AckPayload* ack = reinterpret_cast<const AckPayload*>(message->payload);
        handleAck(ack);
        return true;
    }
    
    // Handle any message that requires ACK based on flags
    if (message->header.flags & MSG_FLAG_RELIABLE) {
        bool is_critical = (message->header.flags & MSG_FLAG_CRITICAL) != 0;
        logger_.info("Received %s message requiring ACK", 
                    is_critical ? "CRITICAL" : "RELIABLE");
            
        // Send ACK back to sender
        sendAck(message->header.src_addr, message->header.seq_num, 0);
        
        // Record ACK sent for statistics
        if (network_stats_) {
            network_stats_->recordAckSent(message->header.src_addr);
        }
    }
    
    // Handle specific message types
    if (message->header.type == MSG_TYPE_ACTUATOR_CMD) {
        const ActuatorPayload* actuator = reinterpret_cast<const ActuatorPayload*>(message->payload);
        if (actuator) {
            logger_.info("Received actuator command: type=%d, cmd=%d, param_len=%d",
                   actuator->actuator_type, actuator->command, actuator->param_length);

            // Log first parameter if present
            if (actuator->param_length > 0) {
                logger_.info("  First param (valve_id): %d", actuator->params[0]);
            }

            // Call the actuator callback if set
            if (actuator_callback_) {
                logger_.info("Calling actuator callback");
                actuator_callback_(actuator);
            } else {
                logger_.warn("No actuator callback set!");
            }

            return true;
        }
    }
    
    if (message->header.type == MSG_TYPE_SENSOR_DATA) {
        const SensorPayload* sensor = reinterpret_cast<const SensorPayload*>(message->payload);
        if (sensor) {
            logger_.info("Received sensor data: type=%d, len=%d",
                   sensor->sensor_type, sensor->data_length);
            return true;
        }
    }

    if (message->header.type == MSG_TYPE_UPDATE_AVAILABLE) {
        const UpdateAvailablePayload* update_payload = reinterpret_cast<const UpdateAvailablePayload*>(message->payload);
        if (update_payload) {
            logger_.info("Received UPDATE_AVAILABLE: has_update=%d, type=%d, seq=%d",
                   update_payload->has_update, update_payload->update_type, update_payload->sequence);

            // Call the update callback if set
            if (update_callback_) {
                logger_.info("Calling update callback");
                update_callback_(update_payload);
            } else {
                logger_.warn("No update callback set!");
            }

            return true;
        }
    }

    if (message->header.type == MSG_TYPE_REGISTRATION) {
        const RegistrationPayload* reg_payload = reinterpret_cast<const RegistrationPayload*>(message->payload);
        if (reg_payload) {
            logger_.info("Received registration request: device_id=0x%016llX, type=%d, capabilities=0x%02X, name='%s'",
                   reg_payload->device_id, reg_payload->node_type, reg_payload->capabilities, reg_payload->device_name);
            
            // TODO: Handle registration request - this should be handled by hub logic
            // For now, just acknowledge that we received it
            return true;
        }
    }
    
    if (message->header.type == MSG_TYPE_REG_RESPONSE) {
        const RegistrationResponsePayload* reg_response = reinterpret_cast<const RegistrationResponsePayload*>(message->payload);
        if (reg_response) {
            logger_.info("Received registration response: device_id=0x%016llX, assigned_addr=0x%04X, status=%d",
                   reg_response->device_id, reg_response->assigned_addr, reg_response->status);
            
            // TODO: Handle registration response - save assigned address if successful
            if (reg_response->status == REG_SUCCESS) {
                logger_.info("Registration successful! Assigned address: 0x%04X", reg_response->assigned_addr);
            } else {
                logger_.error("Registration failed with status: %d", reg_response->status);
            }
            return true;
        }
    }
    
    return false;
}

void ReliableMessenger::update() {
    uint32_t current_time = TimeUtils::getCurrentTimeMs();
    
    // Check for messages that need retry
    auto it = pending_messages_.begin();
    while (it != pending_messages_.end()) {
        auto& [seq_num, pending] = *it;
        
        // Calculate time for next retry
        uint32_t retry_delay = RetryPolicy::calculateDelay(pending.retry_config, pending.attempts);
        uint32_t next_retry_time = pending.send_time + retry_delay;
        
        if (current_time >= next_retry_time) {
            if (RetryPolicy::shouldRetry(pending.retry_config, pending.attempts + 1)) {
                // Retry the message
                pending.attempts++;
                pending.send_time = current_time;
                
                logger_.info("Retry %d for seq=%d (%s)", pending.attempts, seq_num,
                            RetryPolicy::getPolicyName(pending.criticality));
                
                if (!sendMessage(pending.buffer.get(), pending.length)) {
                    logger_.error("Failed to resend message seq=%d", seq_num);
                }
            } else {
                // Give up
                logger_.error("Message seq=%d failed after %d attempts", seq_num, pending.attempts);
                if (network_stats_) {
                    network_stats_->recordTimeout(pending.dst_addr, pending.criticality);
                    network_stats_->recordMessageSent(pending.dst_addr, pending.criticality,
                                                    false, pending.attempts);
                }
                it = pending_messages_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

bool ReliableMessenger::wasAcknowledged(uint8_t seq_num) {
    // If message is still pending, it hasn't been acknowledged
    // If it's not in pending, it was either acknowledged or never sent as reliable
    return pending_messages_.find(seq_num) == pending_messages_.end();
}

bool ReliableMessenger::sendMessage(const uint8_t* buffer, size_t length) {
    if (!lora_ || !buffer || length == 0) return false;

    // Debug: Check for OFF commands
    if (length >= 12) {
        const Message* msg = reinterpret_cast<const Message*>(buffer);
        if (msg->header.type == MSG_TYPE_ACTUATOR_CMD && msg->payload[1] == 0) {
            logger_.info("Sending OFF cmd to radio: len=%zu, payload[1]=%d", length, msg->payload[1]);
        }
    }

    if (!lora_->send(buffer, length)) {
        return false;
    }
    
    // Wait for transmission to complete
    logger_.debug("Waiting for TX to complete...");
    int wait_count = 0;
    while (!lora_->isTxDone()) {
        // Check for interrupts while waiting
        if (lora_->isInterruptPending()) {
            logger_.debug("TX interrupt pending, handling...");
            lora_->handleInterrupt();
        }
        
        
        sleep_ms(1);
        wait_count++;
        if (wait_count > 5000) {  // 5 second timeout
            logger_.error("TX timeout - no interrupt received");
            return false;
        }
    }
    logger_.debug("TX complete after %d ms", wait_count);
    
    // Return to receive mode
    lora_->startReceive();
    
    return true;
}

void ReliableMessenger::handleAck(const AckPayload* ack_payload) {
    if (!ack_payload) return;
    
    auto it = pending_messages_.find(ack_payload->ack_seq_num);
    if (it != pending_messages_.end()) {
        auto& [seq_num, pending] = *it;
        
        logger_.info("ACK received for seq=%d, status=%d", 
                    ack_payload->ack_seq_num, ack_payload->status);
        
        // Record successful transmission for statistics
        if (network_stats_) {
            network_stats_->recordMessageSent(pending.dst_addr, pending.criticality, 
                                            true, pending.attempts);
            network_stats_->recordAckReceived(pending.dst_addr);
        }
        
        // Remove from pending
        pending_messages_.erase(it);
    } else {
        logger_.debug("Received ACK for unknown seq=%d", ack_payload->ack_seq_num);
    }
}

void ReliableMessenger::sendAck(uint16_t src_addr, uint8_t seq_num, uint8_t status) {
    uint8_t my_seq = getNextSequenceNumber();
    
    uint8_t buffer[MESSAGE_MAX_SIZE];
    AckPayload ack = { seq_num, status };
    size_t length = MessageBuilder::createMessage(MSG_TYPE_ACK, 0, node_addr_, src_addr, 
                                                 my_seq, ack, buffer);
    
    if (length > 0) {
        sendMessage(buffer, length);
        logger_.info("Sent ACK for seq=%d to 0x%04X", seq_num, src_addr);
    } else {
        logger_.error("Failed to create ACK message");
    }
}




uint8_t ReliableMessenger::getNextSequenceNumber() {
    uint8_t seq_num = next_seq_num_;
    
    // Handle sequence number wraparound within ranges
    if (node_addr_ == ADDRESS_HUB) {
        next_seq_num_ = (next_seq_num_ >= 127) ? 1 : next_seq_num_ + 1;
    } else {
        // All non-hub addresses (including unregistered nodes) use 128-255
        next_seq_num_ = (next_seq_num_ >= 255) ? 128 : next_seq_num_ + 1;
    }
    
    return seq_num;
}

void ReliableMessenger::updateNodeAddress(uint16_t new_addr) {
    logger_.info("Updating node address from 0x%04X to 0x%04X", node_addr_, new_addr);
    node_addr_ = new_addr;
    
    // Note: We keep the same sequence number range (128-255) for all non-hub nodes
    // This ensures continuity even after address assignment
}