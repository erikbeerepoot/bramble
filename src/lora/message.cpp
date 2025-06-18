#include "message.h"
#include <string.h>
#include <stdio.h>

size_t MessageHandler::createSensorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                          uint8_t sensor_type, const uint8_t* data, uint8_t data_length,
                                          uint8_t flags, uint8_t* buffer) {
    if (!buffer || !data || data_length > 32) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_SENSOR_DATA, flags, src_addr, dst_addr, seq_num, &msg->header);
    
    SensorPayload* payload = (SensorPayload*)msg->payload;
    payload->sensor_type = sensor_type;
    payload->data_length = data_length;
    memcpy(payload->data, data, data_length);
    
    return MESSAGE_HEADER_SIZE + sizeof(uint8_t) + sizeof(uint8_t) + data_length;
}

size_t MessageHandler::createActuatorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                            uint8_t actuator_type, uint8_t command,
                                            const uint8_t* params, uint8_t param_length,
                                            uint8_t flags, uint8_t* buffer) {
    if (!buffer || param_length > 16) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_ACTUATOR_CMD, flags, src_addr, dst_addr, seq_num, &msg->header);
    
    ActuatorPayload* payload = (ActuatorPayload*)msg->payload;
    payload->actuator_type = actuator_type;
    payload->command = command;
    payload->param_length = param_length;
    
    if (params && param_length > 0) {
        memcpy(payload->params, params, param_length);
    }
    
    return MESSAGE_HEADER_SIZE + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) + param_length;
}

size_t MessageHandler::createHeartbeatMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                             uint8_t battery_level, uint8_t signal_quality,
                                             uint32_t uptime_seconds, uint8_t status_flags,
                                             uint8_t* buffer) {
    if (!buffer) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_HEARTBEAT, 0, src_addr, dst_addr, seq_num, &msg->header);
    
    HeartbeatPayload* payload = (HeartbeatPayload*)msg->payload;
    payload->uptime_seconds = uptime_seconds;
    payload->battery_level = battery_level;
    payload->signal_strength = signal_quality;  // Using signal_quality param as signal_strength
    payload->active_sensors = 0;  // Default to no sensors active
    payload->error_flags = status_flags;
    
    return MESSAGE_HEADER_SIZE + sizeof(HeartbeatPayload);
}

size_t MessageHandler::createRegistrationMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                                 uint64_t device_id, uint8_t node_type, uint8_t capabilities,
                                                 uint16_t firmware_ver, const char* device_name,
                                                 uint8_t* buffer) {
    if (!buffer) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_REGISTRATION, MSG_FLAG_RELIABLE, src_addr, dst_addr, seq_num, &msg->header);
    
    RegistrationPayload* payload = (RegistrationPayload*)msg->payload;
    payload->device_id = device_id;
    payload->node_type = node_type;
    payload->capabilities = capabilities;
    payload->firmware_ver = firmware_ver;
    
    // Copy device name, ensuring null termination
    if (device_name) {
        strncpy(payload->device_name, device_name, sizeof(payload->device_name) - 1);
        payload->device_name[sizeof(payload->device_name) - 1] = '\0';
    } else {
        payload->device_name[0] = '\0';
    }
    
    return MESSAGE_HEADER_SIZE + sizeof(RegistrationPayload);
}

size_t MessageHandler::createRegistrationResponse(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                                  uint64_t device_id, uint16_t assigned_addr, 
                                                  uint8_t status, uint8_t retry_interval,
                                                  uint32_t network_time, uint8_t* buffer) {
    if (!buffer) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_REG_RESPONSE, MSG_FLAG_RELIABLE, src_addr, dst_addr, seq_num, &msg->header);
    
    RegistrationResponsePayload* payload = (RegistrationResponsePayload*)msg->payload;
    payload->device_id = device_id;
    payload->assigned_addr = assigned_addr;
    payload->status = status;
    payload->retry_interval = retry_interval;
    payload->network_time = network_time;
    
    return MESSAGE_HEADER_SIZE + sizeof(RegistrationResponsePayload);
}

size_t MessageHandler::createAckMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                       uint8_t ack_seq_num, uint8_t status,
                                       uint8_t* buffer) {
    if (!buffer) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_ACK, 0, src_addr, dst_addr, seq_num, &msg->header);
    
    AckPayload* payload = (AckPayload*)msg->payload;
    payload->ack_seq_num = ack_seq_num;
    payload->status = status;
    
    return MESSAGE_HEADER_SIZE + sizeof(AckPayload);
}

bool MessageHandler::parseMessage(const uint8_t* data, size_t length, Message* message) {
    if (!data || !message || length < MESSAGE_HEADER_SIZE) {
        return false;
    }
    
    // Copy header
    memcpy(&message->header, data, MESSAGE_HEADER_SIZE);
    
    // Validate header
    if (!validateHeader(&message->header)) {
        return false;
    }
    
    // Copy payload
    size_t payload_length = length - MESSAGE_HEADER_SIZE;
    if (payload_length > MESSAGE_MAX_PAYLOAD) {
        return false;
    }
    
    if (payload_length > 0) {
        memcpy(message->payload, data + MESSAGE_HEADER_SIZE, payload_length);
    }
    
    return true;
}

bool MessageHandler::validateHeader(const MessageHeader* header) {
    if (!header) {
        return false;
    }
    
    // Check magic number
    if (header->magic != MESSAGE_MAGIC) {
        return false;
    }
    
    // Check message type
    if (header->type < MSG_TYPE_SENSOR_DATA || header->type > MSG_TYPE_ROUTE) {
        return false;
    }
    
    // Check addresses (hub address is 0x0000, broadcast is 0xFFFF)
    // All other addresses should be in node range
    if (header->src_addr != ADDRESS_HUB && 
        header->src_addr != ADDRESS_BROADCAST &&
        (header->src_addr < ADDRESS_MIN_NODE || header->src_addr > ADDRESS_MAX_NODE)) {
        return false;
    }
    
    if (header->dst_addr != ADDRESS_HUB && 
        header->dst_addr != ADDRESS_BROADCAST &&
        (header->dst_addr < ADDRESS_MIN_NODE || header->dst_addr > ADDRESS_MAX_NODE)) {
        return false;
    }
    
    return true;
}

const SensorPayload* MessageHandler::getSensorPayload(const Message* message) {
    if (!message || message->header.type != MSG_TYPE_SENSOR_DATA) {
        return nullptr;
    }
    return (const SensorPayload*)message->payload;
}

const ActuatorPayload* MessageHandler::getActuatorPayload(const Message* message) {
    if (!message || message->header.type != MSG_TYPE_ACTUATOR_CMD) {
        return nullptr;
    }
    return (const ActuatorPayload*)message->payload;
}

const HeartbeatPayload* MessageHandler::getHeartbeatPayload(const Message* message) {
    if (!message || message->header.type != MSG_TYPE_HEARTBEAT) {
        return nullptr;
    }
    return (const HeartbeatPayload*)message->payload;
}

const RegistrationPayload* MessageHandler::getRegistrationPayload(const Message* message) {
    if (!message || message->header.type != MSG_TYPE_REGISTRATION) {
        return nullptr;
    }
    return (const RegistrationPayload*)message->payload;
}

const RegistrationResponsePayload* MessageHandler::getRegistrationResponsePayload(const Message* message) {
    if (!message || message->header.type != MSG_TYPE_REG_RESPONSE) {
        return nullptr;
    }
    return (const RegistrationResponsePayload*)message->payload;
}

const AckPayload* MessageHandler::getAckPayload(const Message* message) {
    if (!message || message->header.type != MSG_TYPE_ACK) {
        return nullptr;
    }
    return (const AckPayload*)message->payload;
}

void MessageHandler::createHeader(uint8_t type, uint8_t flags, uint16_t src_addr, uint16_t dst_addr,
                                 uint8_t seq_num, MessageHeader* header) {
    header->magic = MESSAGE_MAGIC;
    header->type = type;
    header->flags = flags;
    header->src_addr = src_addr;
    header->dst_addr = dst_addr;
    header->seq_num = seq_num;
}

bool MessageHandler::requiresAck(const Message* message) {
    if (!message) return false;
    return (message->header.flags & MSG_FLAG_RELIABLE) != 0;
}

bool MessageHandler::isCritical(const Message* message) {
    if (!message) return false;
    return (message->header.flags & MSG_FLAG_CRITICAL) != 0;
}