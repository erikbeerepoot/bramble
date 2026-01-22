#pragma once

#include <cstdio>
#include <cstring>
#include <type_traits>

#include "message.h"

/**
 * @brief Template-based message builder for simplified message creation
 */
class MessageBuilder {
public:
    /**
     * @brief Template for creating messages with payload types
     * @tparam PayloadType The payload structure type
     * @param type Message type
     * @param flags Message flags
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param payload_data Data to copy into payload
     * @param buffer Output buffer
     * @return Size of created message, 0 on error
     */
    template <typename PayloadType>
    static size_t createMessage(uint8_t type, uint8_t flags, uint16_t src_addr, uint16_t dst_addr,
                                uint8_t seq_num, const PayloadType &payload_data, uint8_t *buffer)
    {
        static_assert(std::is_trivially_copyable<PayloadType>::value,
                      "Payload type must be trivially copyable");

        if (!buffer)
            return 0;

        // Validate message will fit
        size_t total_size = MESSAGE_HEADER_SIZE + sizeof(PayloadType);
        if (total_size > MESSAGE_MAX_SIZE)
            return 0;

        Message *message = reinterpret_cast<Message *>(buffer);

        // Create header
        message->header.magic = MESSAGE_MAGIC;
        message->header.type = type;
        message->header.flags = flags;
        message->header.src_addr = src_addr;
        message->header.dst_addr = dst_addr;
        message->header.seq_num = seq_num;

        // Copy payload
        std::memcpy(message->payload, &payload_data, sizeof(PayloadType));

        return total_size;
    }

    /**
     * @brief Create a variable-length message (for sensor/actuator data)
     */
    static size_t createVariableMessage(uint8_t type, uint8_t flags, uint16_t src_addr,
                                        uint16_t dst_addr, uint8_t seq_num, const void *header_data,
                                        size_t header_size, const void *variable_data,
                                        size_t variable_size, uint8_t *buffer)
    {
        if (!buffer)
            return 0;

        size_t total_payload = header_size + variable_size;
        size_t total_size = MESSAGE_HEADER_SIZE + total_payload;
        if (total_size > MESSAGE_MAX_SIZE)
            return 0;

        Message *message = reinterpret_cast<Message *>(buffer);

        // Create header
        message->header.magic = MESSAGE_MAGIC;
        message->header.type = type;
        message->header.flags = flags;
        message->header.src_addr = src_addr;
        message->header.dst_addr = dst_addr;
        message->header.seq_num = seq_num;

        // Copy fixed header part
        if (header_data && header_size > 0) {
            std::memcpy(message->payload, header_data, header_size);
        }

        // Copy variable data part
        if (variable_data && variable_size > 0) {
            std::memcpy(message->payload + header_size, variable_data, variable_size);
        }

        return total_size;
    }

    /**
     * @brief Simplified sensor message creation
     */
    static size_t createSensorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                      uint8_t sensor_type, const uint8_t *data, uint8_t data_length,
                                      uint8_t flags, uint8_t *buffer)
    {
        struct SensorHeader {
            uint8_t sensor_type;
            uint8_t data_length;
        } header = {sensor_type, data_length};

        return createVariableMessage(MSG_TYPE_SENSOR_DATA, flags, src_addr, dst_addr, seq_num,
                                     &header, sizeof(header), data, data_length, buffer);
    }

    /**
     * @brief Simplified actuator message creation
     */
    static size_t createActuatorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                        uint8_t actuator_type, uint8_t command,
                                        const uint8_t *params, uint8_t param_length, uint8_t flags,
                                        uint8_t *buffer)
    {
        struct ActuatorHeader {
            uint8_t actuator_type;
            uint8_t command;
            uint8_t param_length;
        } header = {actuator_type, command, param_length};

        return createVariableMessage(MSG_TYPE_ACTUATOR_CMD, flags, src_addr, dst_addr, seq_num,
                                     &header, sizeof(header), params, param_length, buffer);
    }

    /**
     * @brief Create a sensor data batch message for backlog transmission
     * @param src_addr Source node address
     * @param dst_addr Destination address (hub)
     * @param seq_num Sequence number
     * @param start_index Flash buffer start index (for debugging/tracking)
     * @param records Array of sensor data records
     * @param record_count Number of records (1-20)
     * @param flags Message flags
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Size of created message, 0 on error
     */
    static size_t createSensorDataBatchMessage(uint16_t src_addr, uint16_t dst_addr,
                                               uint8_t seq_num, uint32_t start_index,
                                               const BatchSensorRecord *records,
                                               uint8_t record_count, uint8_t flags, uint8_t *buffer)
    {
        if (!buffer || !records || record_count == 0 || record_count > MAX_BATCH_RECORDS) {
            return 0;
        }

        // Calculate payload size: header (7 bytes) + records
        size_t payload_size = 7 + (record_count * BATCH_RECORD_SIZE);
        size_t total_size = MESSAGE_HEADER_SIZE + payload_size;

        if (total_size > MESSAGE_MAX_SIZE) {
            return 0;
        }

        Message *message = reinterpret_cast<Message *>(buffer);

        // Create header
        message->header.magic = MESSAGE_MAGIC;
        message->header.type = MSG_TYPE_SENSOR_DATA_BATCH;
        message->header.flags = flags;
        message->header.src_addr = src_addr;
        message->header.dst_addr = dst_addr;
        message->header.seq_num = seq_num;

        // Create batch payload
        SensorDataBatchPayload *batch =
            reinterpret_cast<SensorDataBatchPayload *>(message->payload);
        batch->node_addr = src_addr;
        batch->start_index = start_index;
        batch->record_count = record_count;

        // Copy records
        std::memcpy(batch->records, records, record_count * BATCH_RECORD_SIZE);

        return total_size;
    }

    /**
     * @brief Convert criticality to message flags
     */
    static uint8_t criticalityToFlags(DeliveryCriticality criticality)
    {
        switch (criticality) {
            case BEST_EFFORT:
                return 0;
            case RELIABLE:
                return MSG_FLAG_RELIABLE;
            case CRITICAL:
                return MSG_FLAG_RELIABLE | MSG_FLAG_CRITICAL;
            default:
                return 0;
        }
    }
};