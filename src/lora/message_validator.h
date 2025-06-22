#pragma once

#include "message.h"
#include <cstring>

/**
 * @brief Consolidated message validation logic
 */
class MessageValidator {
public:
    /**
     * @brief Check if an address is valid
     */
    static bool isValidAddress(uint16_t addr) {
        return addr == ADDRESS_HUB || 
               addr == ADDRESS_BROADCAST ||
               addr == ADDRESS_UNREGISTERED ||
               (addr >= ADDRESS_MIN_NODE && addr <= ADDRESS_MAX_NODE);
    }
    
    /**
     * @brief Validate message header
     */
    static bool validateHeader(const MessageHeader* header) {
        if (!header || header->magic != MESSAGE_MAGIC) return false;
        if (header->type < MSG_TYPE_SENSOR_DATA || header->type > MSG_TYPE_ROUTE) return false;
        return isValidAddress(header->src_addr) && isValidAddress(header->dst_addr);
    }
    
    /**
     * @brief Template for validating fixed-size payloads
     */
    template<typename PayloadType>
    static bool validateFixedPayload(const uint8_t* payload, size_t payload_length) {
        return payload_length == sizeof(PayloadType);
    }
    
    /**
     * @brief Validate variable-length payload
     */
    static bool validateVariablePayload(const uint8_t* payload, size_t payload_length,
                                       size_t header_size, size_t max_data_length) {
        if (payload_length < header_size) return false;
        
        // Extract data length from payload (assuming it's the second byte)
        uint8_t data_length = payload[1];
        if (data_length > max_data_length) return false;
        
        return payload_length == header_size + data_length;
    }
    
    /**
     * @brief Validate complete message
     */
    static bool validateMessage(const uint8_t* data, size_t length) {
        if (!data || length < MESSAGE_HEADER_SIZE || length > MESSAGE_MAX_SIZE) {
            return false;
        }
        
        const MessageHeader* header = reinterpret_cast<const MessageHeader*>(data);
        if (!validateHeader(header)) return false;
        
        size_t payload_length = length - MESSAGE_HEADER_SIZE;
        const uint8_t* payload = data + MESSAGE_HEADER_SIZE;
        
        // Validate payload based on message type
        switch (header->type) {
            case MSG_TYPE_SENSOR_DATA:
                return validateVariablePayload(payload, payload_length, 2, MAX_SENSOR_DATA_LENGTH);
                
            case MSG_TYPE_ACTUATOR_CMD:
                return validateVariablePayload(payload, payload_length, 3, MAX_ACTUATOR_PARAMS);
                
            case MSG_TYPE_HEARTBEAT:
                return validateFixedPayload<HeartbeatPayload>(payload, payload_length);
                
            case MSG_TYPE_REGISTRATION:
                return validateFixedPayload<RegistrationPayload>(payload, payload_length) &&
                       validateRegistrationPayload(reinterpret_cast<const RegistrationPayload*>(payload));
                
            case MSG_TYPE_REG_RESPONSE:
                return validateFixedPayload<RegistrationResponsePayload>(payload, payload_length) &&
                       validateRegistrationResponse(reinterpret_cast<const RegistrationResponsePayload*>(payload));
                
            case MSG_TYPE_ACK:
                return validateFixedPayload<AckPayload>(payload, payload_length);
                
            case MSG_TYPE_ROUTE:
                return payload_length <= MESSAGE_MAX_PAYLOAD;
                
            default:
                return false;
        }
    }
    
private:
    static bool validateRegistrationPayload(const RegistrationPayload* reg) {
        // Check device name is null-terminated
        for (size_t i = 0; i < sizeof(reg->device_name); i++) {
            if (reg->device_name[i] == '\0') return true;
        }
        return false;
    }
    
    static bool validateRegistrationResponse(const RegistrationResponsePayload* resp) {
        return resp->status <= REG_ERROR_INTERNAL;
    }
};