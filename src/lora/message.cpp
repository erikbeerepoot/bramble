#include "message.h"

#include <stdio.h>
#include <string.h>

#include "message_builder.h"
#include "message_validator.h"

const char *MessageHandler::getMessageTypeName(uint8_t type)
{
    switch (type) {
        case MSG_TYPE_SENSOR_DATA:
            return "SENSOR_DATA";
        case MSG_TYPE_ACTUATOR_CMD:
            return "ACTUATOR_CMD";
        case MSG_TYPE_ACK:
            return "ACK";
        case MSG_TYPE_HEARTBEAT:
            return "HEARTBEAT";
        case MSG_TYPE_REGISTRATION:
            return "REGISTRATION";
        case MSG_TYPE_REG_RESPONSE:
            return "REG_RESPONSE";
        case MSG_TYPE_CONFIG:
            return "CONFIG";
        case MSG_TYPE_ROUTE:
            return "ROUTE";
        case MSG_TYPE_CHECK_UPDATES:
            return "CHECK_UPDATES";
        case MSG_TYPE_UPDATE_AVAILABLE:
            return "UPDATE_AVAILABLE";
        case MSG_TYPE_HEARTBEAT_RESPONSE:
            return "HEARTBEAT_RESPONSE";
        case MSG_TYPE_SENSOR_DATA_BATCH:
            return "SENSOR_DATA_BATCH";
        case MSG_TYPE_BATCH_ACK:
            return "BATCH_ACK";
        default:
            return "UNKNOWN";
    }
}

size_t MessageHandler::createSensorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                           uint8_t sensor_type, const uint8_t *data,
                                           uint8_t data_length, uint8_t flags, uint8_t *buffer)
{
    if (!data || data_length > MAX_SENSOR_DATA_LENGTH) {
        return 0;
    }

    return MessageBuilder::createSensorMessage(src_addr, dst_addr, seq_num, sensor_type, data,
                                               data_length, flags, buffer);
}

size_t MessageHandler::createActuatorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                             uint8_t actuator_type, uint8_t command,
                                             const uint8_t *params, uint8_t param_length,
                                             uint8_t flags, uint8_t *buffer)
{
    if (param_length > MAX_ACTUATOR_PARAMS) {
        return 0;
    }

    return MessageBuilder::createActuatorMessage(src_addr, dst_addr, seq_num, actuator_type,
                                                 command, params, param_length, flags, buffer);
}

size_t MessageHandler::createHeartbeatMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                              uint8_t battery_level, uint8_t signal_quality,
                                              uint32_t uptime_seconds, uint16_t status_flags,
                                              uint8_t *buffer)
{
    HeartbeatPayload payload = {.uptime_seconds = uptime_seconds,
                                .battery_level = battery_level,
                                .signal_strength = signal_quality,
                                .active_sensors = 0,  // Default to no sensors active
                                .error_flags = status_flags};

    return MessageBuilder::createMessage<HeartbeatPayload>(MSG_TYPE_HEARTBEAT, 0, src_addr,
                                                           dst_addr, seq_num, payload, buffer);
}

size_t MessageHandler::createHeartbeatResponseMessage(uint16_t src_addr, uint16_t dst_addr,
                                                      uint8_t seq_num, int16_t year, int8_t month,
                                                      int8_t day, int8_t dotw, int8_t hour,
                                                      int8_t min, int8_t sec, uint8_t *buffer)
{
    HeartbeatResponsePayload payload = {.year = year,
                                        .month = month,
                                        .day = day,
                                        .dotw = dotw,
                                        .hour = hour,
                                        .min = min,
                                        .sec = sec};

    return MessageBuilder::createMessage<HeartbeatResponsePayload>(
        MSG_TYPE_HEARTBEAT_RESPONSE, MSG_FLAG_RELIABLE, src_addr, dst_addr, seq_num, payload,
        buffer);
}

size_t MessageHandler::createRegistrationMessage(uint16_t src_addr, uint16_t dst_addr,
                                                 uint8_t seq_num, uint64_t device_id,
                                                 uint8_t node_type, uint8_t capabilities,
                                                 uint32_t firmware_ver, const char *device_name,
                                                 uint8_t *buffer)
{
    if (device_id == 0) {
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
            return 0;  // Name too long or not null-terminated
        }
    }

    RegistrationPayload payload = {};
    payload.device_id = device_id;
    payload.node_type = node_type;
    payload.capabilities = capabilities;
    payload.firmware_ver = firmware_ver;

    // Copy device name, ensuring null termination
    if (device_name) {
        strncpy(payload.device_name, device_name, sizeof(payload.device_name) - 1);
        payload.device_name[sizeof(payload.device_name) - 1] = '\0';
    } else {
        payload.device_name[0] = '\0';
    }

    return MessageBuilder::createMessage<RegistrationPayload>(
        MSG_TYPE_REGISTRATION, MSG_FLAG_RELIABLE, src_addr, dst_addr, seq_num, payload, buffer);
}

size_t MessageHandler::createRegistrationResponse(uint16_t src_addr, uint16_t dst_addr,
                                                  uint8_t seq_num, uint64_t device_id,
                                                  uint16_t assigned_addr, uint8_t status,
                                                  uint8_t retry_interval, uint32_t network_time,
                                                  uint8_t *buffer)
{
    RegistrationResponsePayload payload = {.device_id = device_id,
                                           .assigned_addr = assigned_addr,
                                           .status = status,
                                           .retry_interval = retry_interval,
                                           .network_time = network_time};

    return MessageBuilder::createMessage<RegistrationResponsePayload>(
        MSG_TYPE_REG_RESPONSE, MSG_FLAG_RELIABLE, src_addr, dst_addr, seq_num, payload, buffer);
}

size_t MessageHandler::createAckMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                        uint8_t ack_seq_num, uint8_t status, uint8_t *buffer)
{
    AckPayload payload = {.ack_seq_num = ack_seq_num, .status = status};

    return MessageBuilder::createMessage<AckPayload>(MSG_TYPE_ACK, 0, src_addr, dst_addr, seq_num,
                                                     payload, buffer);
}

bool MessageHandler::parseMessage(const uint8_t *data, size_t length, Message *message)
{
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

bool MessageHandler::validateHeader(const MessageHeader *header)
{
    if (!header) {
        return false;
    }

    // Check magic number
    if (header->magic != MESSAGE_MAGIC) {
        return false;
    }

    // Check message type
    if (header->type < MSG_TYPE_SENSOR_DATA || header->type > MSG_TYPE_HEARTBEAT_RESPONSE) {
        return false;
    }

    // Check addresses using MessageValidator
    if (!MessageValidator::isValidAddress(header->src_addr)) {
        return false;
    }

    if (!MessageValidator::isValidAddress(header->dst_addr)) {
        return false;
    }

    return true;
}

bool MessageHandler::validatePayload(const MessageHeader *header, const uint8_t *payload,
                                     size_t payload_length)
{
    if (!header) {
        return false;
    }

    // Validate payload size and content based on message type
    switch (header->type) {
        case MSG_TYPE_SENSOR_DATA: {
            if (payload_length < 2) {  // Need at least sensor_type and data_length
                return false;
            }
            const SensorPayload *sensor = (const SensorPayload *)payload;
            // Validate sensor data length doesn't exceed maximum
            if (sensor->data_length > MAX_SENSOR_DATA_LENGTH) {
                return false;
            }
            // Validate total payload size matches expected size
            // payload should be: sensor_type (1) + data_length (1) + actual data bytes
            if (payload_length != 2 + sensor->data_length) {
                return false;
            }
            break;
        }

        case MSG_TYPE_ACTUATOR_CMD: {
            if (payload_length < 3) {  // Need at least actuator_type, command, and param_length
                return false;
            }
            const ActuatorPayload *actuator = (const ActuatorPayload *)payload;
            // Validate parameter length doesn't exceed maximum
            if (actuator->param_length > MAX_ACTUATOR_PARAMS) {
                return false;
            }
            // Validate total payload size matches expected size
            // payload should be: actuator_type (1) + command (1) + param_length (1) + actual param
            // bytes
            if (payload_length != 3 + actuator->param_length) {
                return false;
            }
            break;
        }

        case MSG_TYPE_HEARTBEAT: {
            if (payload_length != sizeof(HeartbeatPayload)) {
                return false;
            }
            const HeartbeatPayload *heartbeat = (const HeartbeatPayload *)payload;
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
            const RegistrationPayload *reg = (const RegistrationPayload *)payload;
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
            const RegistrationResponsePayload *response =
                (const RegistrationResponsePayload *)payload;
            // Validate status code is within valid range
            if (response->status > REG_REREGISTER_REQUIRED) {
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

const SensorPayload *MessageHandler::getSensorPayload(const Message *message)
{
    if (!message || message->header.type != MSG_TYPE_SENSOR_DATA) {
        return nullptr;
    }
    return (const SensorPayload *)message->payload;
}

const ActuatorPayload *MessageHandler::getActuatorPayload(const Message *message)
{
    if (!message || message->header.type != MSG_TYPE_ACTUATOR_CMD) {
        return nullptr;
    }
    return (const ActuatorPayload *)message->payload;
}

const HeartbeatPayload *MessageHandler::getHeartbeatPayload(const Message *message)
{
    if (!message || message->header.type != MSG_TYPE_HEARTBEAT) {
        return nullptr;
    }
    return (const HeartbeatPayload *)message->payload;
}

const HeartbeatResponsePayload *MessageHandler::getHeartbeatResponsePayload(const Message *message)
{
    if (!message || message->header.type != MSG_TYPE_HEARTBEAT_RESPONSE) {
        return nullptr;
    }
    return (const HeartbeatResponsePayload *)message->payload;
}

const RegistrationPayload *MessageHandler::getRegistrationPayload(const Message *message)
{
    if (!message || message->header.type != MSG_TYPE_REGISTRATION) {
        return nullptr;
    }
    return (const RegistrationPayload *)message->payload;
}

const RegistrationResponsePayload *MessageHandler::getRegistrationResponsePayload(
    const Message *message)
{
    if (!message || message->header.type != MSG_TYPE_REG_RESPONSE) {
        return nullptr;
    }
    return (const RegistrationResponsePayload *)message->payload;
}

const AckPayload *MessageHandler::getAckPayload(const Message *message)
{
    if (!message || message->header.type != MSG_TYPE_ACK) {
        return nullptr;
    }
    return (const AckPayload *)message->payload;
}

void MessageHandler::createHeader(uint8_t type, uint8_t flags, uint16_t src_addr, uint16_t dst_addr,
                                  uint8_t seq_num, MessageHeader *header)
{
    header->magic = MESSAGE_MAGIC;
    header->type = type;
    header->flags = flags;
    header->src_addr = src_addr;
    header->dst_addr = dst_addr;
    header->seq_num = seq_num;
}

bool MessageHandler::requiresAck(const Message *message)
{
    if (!message)
        return false;
    return (message->header.flags & MSG_FLAG_RELIABLE) != 0;
}

bool MessageHandler::isCritical(const Message *message)
{
    if (!message)
        return false;
    return (message->header.flags & MSG_FLAG_CRITICAL) != 0;
}
