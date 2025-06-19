#include "message.h"
#include <string.h>
#include <stdio.h>

const char* MessageHandler::getMessageTypeName(uint8_t type) {
    switch (type) {
        case MSG_TYPE_SENSOR_DATA:   return "SENSOR_DATA";
        case MSG_TYPE_ACTUATOR_CMD:  return "ACTUATOR_CMD";
        case MSG_TYPE_ACK:           return "ACK";
        case MSG_TYPE_HEARTBEAT:     return "HEARTBEAT";
        case MSG_TYPE_REGISTRATION:  return "REGISTRATION";
        case MSG_TYPE_REG_RESPONSE:  return "REG_RESPONSE";
        case MSG_TYPE_CONFIG:        return "CONFIG";
        case MSG_TYPE_ROUTE:         return "ROUTE";
        default:                     return "UNKNOWN";
    }
}

size_t MessageHandler::createSensorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                          uint8_t sensor_type, const uint8_t* data, uint8_t data_length,
                                          uint8_t flags, uint8_t* buffer) {
    if (!buffer || !data || data_length > MAX_SENSOR_DATA_LENGTH) {
        return 0;
    }
    
    // Validate total message size
    size_t total_size = MESSAGE_HEADER_SIZE + sizeof(SensorPayload) + data_length;
    if (total_size > MESSAGE_MAX_SIZE) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_SENSOR_DATA, flags, src_addr, dst_addr, seq_num, &msg->header);
    
    SensorPayload* payload = (SensorPayload*)msg->payload;
    payload->sensor_type = sensor_type;
    payload->data_length = data_length;
    
    // Ensure we don't write beyond the data array bounds
    if (data_length > 0) {
        memcpy(payload->data, data, data_length);
    }
    
    return MESSAGE_HEADER_SIZE + sizeof(uint8_t) + sizeof(uint8_t) + data_length;
}

size_t MessageHandler::createActuatorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                            uint8_t actuator_type, uint8_t command,
                                            const uint8_t* params, uint8_t param_length,
                                            uint8_t flags, uint8_t* buffer) {
    if (!buffer || param_length > MAX_ACTUATOR_PARAMS) {
        return 0;
    }
    
    // Validate total message size
    size_t total_size = MESSAGE_HEADER_SIZE + sizeof(ActuatorPayload) + param_length;
    if (total_size > MESSAGE_MAX_SIZE) {
        return 0;
    }
    
    Message* msg = (Message*)buffer;
    createHeader(MSG_TYPE_ACTUATOR_CMD, flags, src_addr, dst_addr, seq_num, &msg->header);
    
    ActuatorPayload* payload = (ActuatorPayload*)msg->payload;
    payload->actuator_type = actuator_type;
    payload->command = command;
    payload->param_length = param_length;
    
    // Only copy if we have valid parameters and they fit
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
    if (!buffer || device_id == 0) {
        return 0;
    }
    
    // Validate node type
    if (node_type < NODE_TYPE_SENSOR || node_type > NODE_TYPE_REPEATER) {
        return 0;
    }
    
    // Validate device name if provided
    if (device_name) {
        size_t name_len = strnlen(device_name, 32);
        if (name_len >= 32) {
            return 0; // Name too long or not null-terminated
        }
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
    
    // Validate total message size
    if (length > MESSAGE_MAX_SIZE) {
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
    
    // Validate payload content based on message type
    if (!validatePayload(&message->header, message->payload, payload_length)) {
        return false;
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
    
    // Check addresses - allow hub, broadcast, unregistered, and valid node addresses
    if (header->src_addr != ADDRESS_HUB && 
        header->src_addr != ADDRESS_BROADCAST &&
        header->src_addr != ADDRESS_UNREGISTERED &&
        (header->src_addr < ADDRESS_MIN_NODE || header->src_addr > ADDRESS_MAX_NODE)) {
        return false;
    }
    
    if (header->dst_addr != ADDRESS_HUB && 
        header->dst_addr != ADDRESS_BROADCAST &&
        header->dst_addr != ADDRESS_UNREGISTERED &&
        (header->dst_addr < ADDRESS_MIN_NODE || header->dst_addr > ADDRESS_MAX_NODE)) {
        return false;
    }
    
    return true;
}

bool MessageHandler::validatePayload(const MessageHeader* header, const uint8_t* payload, size_t payload_length) {
    if (!header) {
        return false;
    }
    
    // Validate payload size and content based on message type
    switch (header->type) {
        case MSG_TYPE_SENSOR_DATA: {
            if (payload_length < sizeof(SensorPayload)) {
                return false;
            }
            const SensorPayload* sensor = (const SensorPayload*)payload;
            // Validate sensor data length doesn't exceed maximum
            if (sensor->data_length > MAX_SENSOR_DATA_LENGTH) {
                return false;
            }
            // Validate total payload size matches expected size
            if (payload_length != sizeof(SensorPayload) + sensor->data_length) {
                return false;
            }
            break;
        }
        
        case MSG_TYPE_ACTUATOR_CMD: {
            if (payload_length < sizeof(ActuatorPayload)) {
                return false;
            }
            const ActuatorPayload* actuator = (const ActuatorPayload*)payload;
            // Validate parameter length doesn't exceed maximum
            if (actuator->param_length > MAX_ACTUATOR_PARAMS) {
                return false;
            }
            // Validate total payload size matches expected size
            if (payload_length != sizeof(ActuatorPayload) + actuator->param_length) {
                return false;
            }
            break;
        }
        
        case MSG_TYPE_HEARTBEAT: {
            if (payload_length != sizeof(HeartbeatPayload)) {
                return false;
            }
            const HeartbeatPayload* heartbeat = (const HeartbeatPayload*)payload;
            // Validate battery level is within range
            if (heartbeat->battery_level > 100) {
                return false;
            }
            // Validate signal strength is within range
            if (heartbeat->signal_strength > 100) {
                return false;
            }
            break;
        }
        
        case MSG_TYPE_REGISTRATION: {
            if (payload_length != sizeof(RegistrationPayload)) {
                return false;
            }
            const RegistrationPayload* reg = (const RegistrationPayload*)payload;
            // Validate device name is null-terminated
            bool null_terminated = false;
            for (size_t i = 0; i < sizeof(reg->device_name); i++) {
                if (reg->device_name[i] == '\0') {
                    null_terminated = true;
                    break;
                }
            }
            if (!null_terminated) {
                return false;
            }
            break;
        }
        
        case MSG_TYPE_REG_RESPONSE: {
            if (payload_length != sizeof(RegistrationResponsePayload)) {
                return false;
            }
            const RegistrationResponsePayload* response = (const RegistrationResponsePayload*)payload;
            // Validate status code is within valid range
            if (response->status > REG_ERROR_INTERNAL) {
                return false;
            }
            break;
        }
        
        case MSG_TYPE_ACK: {
            if (payload_length != sizeof(AckPayload)) {
                return false;
            }
            break;
        }
        
        case MSG_TYPE_ROUTE: {
            // Route messages can have variable length payloads
            // Just validate it doesn't exceed maximum
            if (payload_length > MESSAGE_MAX_PAYLOAD) {
                return false;
            }
            break;
        }
        
        default:
            // Unknown message type should have been caught in header validation
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