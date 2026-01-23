#pragma once

#include <cstdio>
#include <cstring>

#include "message.h"

/**
 * @brief Consolidated message validation logic
 */
class MessageValidator {
public:
    /**
     * @brief Check if an address is valid
     */
    static bool isValidAddress(uint16_t addr)
    {
        return addr == ADDRESS_HUB || addr == ADDRESS_BROADCAST || addr == ADDRESS_UNREGISTERED ||
               (addr >= ADDRESS_MIN_NODE && addr <= ADDRESS_MAX_NODE);
    }

    /**
     * @brief Validate message header
     */
    static bool validateHeader(const MessageHeader *header)
    {
        if (!header || header->magic != MESSAGE_MAGIC)
            return false;
        if (header->type < MSG_TYPE_SENSOR_DATA || header->type > MSG_TYPE_BATCH_ACK)
            return false;
        return isValidAddress(header->src_addr) && isValidAddress(header->dst_addr);
    }

    /**
     * @brief Template for validating fixed-size payloads
     */
    template <typename PayloadType>
    static bool validateFixedPayload(const uint8_t *payload, size_t payload_length)
    {
        return payload_length == sizeof(PayloadType);
    }

    /**
     * @brief Validate variable-length payload
     */
    static bool validateVariablePayload(const uint8_t *payload, size_t payload_length,
                                        size_t header_size, size_t max_data_length,
                                        size_t length_offset = 1)
    {
        if (payload_length < header_size)
            return false;

        // Extract data length from payload at specified offset
        uint8_t data_length = payload[length_offset];
        if (data_length > max_data_length)
            return false;

        return payload_length == header_size + data_length;
    }

    /**
     * @brief Validate complete message
     */
    static bool validateMessage(const uint8_t *data, size_t length)
    {
        if (!data || length < MESSAGE_HEADER_SIZE || length > MESSAGE_MAX_SIZE) {
            return false;
        }

        const MessageHeader *header = reinterpret_cast<const MessageHeader *>(data);
        if (!validateHeader(header))
            return false;

        size_t payload_length = length - MESSAGE_HEADER_SIZE;
        const uint8_t *payload = data + MESSAGE_HEADER_SIZE;

        // Validate payload based on message type
        switch (header->type) {
            case MSG_TYPE_SENSOR_DATA:
                // For sensor: sensor_type, data_length, data[]
                // data_length is at offset 1
                return validateVariablePayload(payload, payload_length, 2, MAX_SENSOR_DATA_LENGTH,
                                               1);

            case MSG_TYPE_ACTUATOR_CMD:
                // For actuator: actuator_type, command, param_length, params[]
                // param_length is at offset 2
                return validateVariablePayload(payload, payload_length, 3, MAX_ACTUATOR_PARAMS, 2);

            case MSG_TYPE_HEARTBEAT:
                return validateFixedPayload<HeartbeatPayload>(payload, payload_length);

            case MSG_TYPE_REGISTRATION:
                return validateFixedPayload<RegistrationPayload>(payload, payload_length) &&
                       validateRegistrationPayload(
                           reinterpret_cast<const RegistrationPayload *>(payload));

            case MSG_TYPE_REG_RESPONSE:
                return validateFixedPayload<RegistrationResponsePayload>(payload, payload_length) &&
                       validateRegistrationResponse(
                           reinterpret_cast<const RegistrationResponsePayload *>(payload));

            case MSG_TYPE_ACK:
                return validateFixedPayload<AckPayload>(payload, payload_length);

            case MSG_TYPE_ROUTE:
                return payload_length <= MESSAGE_MAX_PAYLOAD;

            case MSG_TYPE_CHECK_UPDATES:
                return validateFixedPayload<CheckUpdatesPayload>(payload, payload_length);

            case MSG_TYPE_UPDATE_AVAILABLE:
                return validateFixedPayload<UpdateAvailablePayload>(payload, payload_length);

            case MSG_TYPE_HEARTBEAT_RESPONSE:
                return validateFixedPayload<HeartbeatResponsePayload>(payload, payload_length);

            case MSG_TYPE_SENSOR_DATA_BATCH:
                return validateBatchPayload(payload, payload_length);

            case MSG_TYPE_BATCH_ACK:
                return validateFixedPayload<BatchAckPayload>(payload, payload_length);

            default:
                return false;
        }
    }

private:
    static bool validateRegistrationPayload(const RegistrationPayload *reg)
    {
        // Check device name is null-terminated
        for (size_t i = 0; i < sizeof(reg->device_name); i++) {
            if (reg->device_name[i] == '\0')
                return true;
        }
        return false;
    }

    static bool validateRegistrationResponse(const RegistrationResponsePayload *resp)
    {
        return resp->status <= REG_REREGISTER_REQUIRED;
    }

    static bool validateBatchPayload(const uint8_t *payload, size_t payload_length)
    {
        // Minimum: header (7 bytes) + at least 1 record (12 bytes) = 19 bytes
        constexpr size_t MIN_BATCH_SIZE = 7 + BATCH_RECORD_SIZE;
        if (payload_length < MIN_BATCH_SIZE)
            return false;

        // Extract record_count from payload
        const SensorDataBatchPayload *batch =
            reinterpret_cast<const SensorDataBatchPayload *>(payload);

        // Validate record count
        if (batch->record_count == 0 || batch->record_count > MAX_BATCH_RECORDS) {
            return false;
        }

        // Calculate expected payload size
        size_t expected_size = 7 + (batch->record_count * BATCH_RECORD_SIZE);
        return payload_length == expected_size;
    }
};