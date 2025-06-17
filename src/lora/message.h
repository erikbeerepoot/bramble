#pragma once

#include <stdint.h>
#include <stddef.h>

// Message protocol constants
#define MESSAGE_MAGIC           0xBEEF
#define MESSAGE_HEADER_SIZE     8
#define MESSAGE_MAX_PAYLOAD     247
#define MESSAGE_MAX_SIZE        (MESSAGE_HEADER_SIZE + MESSAGE_MAX_PAYLOAD)

// Address definitions
#define ADDRESS_HUB             0x0000
#define ADDRESS_BROADCAST       0xFFFF
#define ADDRESS_MIN_NODE        0x0001
#define ADDRESS_MAX_NODE        0xFFFE

// Message types
enum MessageType {
    MSG_TYPE_SENSOR_DATA    = 0x01,  // Fire-and-forget sensor readings
    MSG_TYPE_ACTUATOR_CMD   = 0x02,  // Reliable actuator commands
    MSG_TYPE_ACK            = 0x03,  // Acknowledgment
    MSG_TYPE_HEARTBEAT      = 0x04,  // Node heartbeat/status
    MSG_TYPE_REGISTRATION   = 0x05,  // Node registration with hub
    MSG_TYPE_CONFIG         = 0x06,  // Configuration updates
    MSG_TYPE_ROUTE          = 0x07   // Message routing (future use)
};

// Sensor data subtypes
enum SensorType {
    SENSOR_TEMPERATURE      = 0x01,
    SENSOR_HUMIDITY         = 0x02,
    SENSOR_BATTERY          = 0x03,
    SENSOR_SOIL_MOISTURE    = 0x04
};

// Actuator command subtypes  
enum ActuatorType {
    ACTUATOR_VALVE          = 0x01,
    ACTUATOR_PUMP           = 0x02,
    ACTUATOR_FAN            = 0x03
};

// Actuator commands
enum ActuatorCommand {
    CMD_TURN_OFF            = 0x00,
    CMD_TURN_ON             = 0x01,
    CMD_TOGGLE              = 0x02,
    CMD_SET_LEVEL           = 0x03  // For variable speed/position actuators
};

/**
 * @brief Message header structure
 */
struct __attribute__((packed)) MessageHeader {
    uint16_t magic;         // Magic number (0xBEEF)
    uint8_t  type;          // Message type
    uint16_t src_addr;      // Source address
    uint16_t dst_addr;      // Destination address  
    uint8_t  seq_num;       // Sequence number
};

/**
 * @brief Sensor data payload
 */
struct __attribute__((packed)) SensorPayload {
    uint8_t  sensor_type;   // Sensor type
    uint8_t  data_length;   // Length of sensor data
    uint8_t  data[32];      // Sensor data (flexible format)
};

/**
 * @brief Actuator command payload
 */
struct __attribute__((packed)) ActuatorPayload {
    uint8_t  actuator_type; // Actuator type
    uint8_t  command;       // Command to execute
    uint8_t  param_length;  // Length of parameters
    uint8_t  params[16];    // Command parameters
};

/**
 * @brief Heartbeat payload
 */
struct __attribute__((packed)) HeartbeatPayload {
    uint8_t  battery_level; // Battery level (0-100%)
    uint8_t  signal_quality;// Signal quality (0-100%)
    uint32_t uptime_seconds;// Uptime in seconds
    uint8_t  status_flags;  // Status flags
};

/**
 * @brief Registration payload
 */
struct __attribute__((packed)) RegistrationPayload {
    uint8_t  node_type;     // Node type identifier
    uint8_t  capabilities;  // Capability flags
    uint16_t firmware_ver;  // Firmware version
    char     device_name[16]; // Human readable device name
};

/**
 * @brief ACK payload
 */
struct __attribute__((packed)) AckPayload {
    uint8_t  ack_seq_num;   // Sequence number being acknowledged
    uint8_t  status;        // ACK status (0=success, non-zero=error)
};

/**
 * @brief Complete message structure
 */
struct __attribute__((packed)) Message {
    MessageHeader header;
    uint8_t payload[MESSAGE_MAX_PAYLOAD];
};

/**
 * @brief Message handling class
 */
class MessageHandler {
public:
    /**
     * @brief Create a sensor data message
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param sensor_type Type of sensor
     * @param data Sensor data
     * @param data_length Length of sensor data
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createSensorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                     uint8_t sensor_type, const uint8_t* data, uint8_t data_length,
                                     uint8_t* buffer);
    
    /**
     * @brief Create an actuator command message
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param actuator_type Type of actuator
     * @param command Command to execute
     * @param params Command parameters
     * @param param_length Length of parameters
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createActuatorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                       uint8_t actuator_type, uint8_t command, 
                                       const uint8_t* params, uint8_t param_length,
                                       uint8_t* buffer);
    
    /**
     * @brief Create a heartbeat message
     * @param src_addr Source address
     * @param dst_addr Destination address  
     * @param seq_num Sequence number
     * @param battery_level Battery level (0-100%)
     * @param signal_quality Signal quality (0-100%)
     * @param uptime_seconds Uptime in seconds
     * @param status_flags Status flags
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createHeartbeatMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                        uint8_t battery_level, uint8_t signal_quality,
                                        uint32_t uptime_seconds, uint8_t status_flags,
                                        uint8_t* buffer);
    
    /**
     * @brief Create a registration message
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param node_type Node type identifier
     * @param capabilities Capability flags
     * @param firmware_ver Firmware version
     * @param device_name Device name (max 15 chars + null terminator)
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createRegistrationMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                           uint8_t node_type, uint8_t capabilities,
                                           uint16_t firmware_ver, const char* device_name,
                                           uint8_t* buffer);
    
    /**
     * @brief Create an ACK message
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param ack_seq_num Sequence number being acknowledged
     * @param status ACK status (0=success)
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createAckMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                  uint8_t ack_seq_num, uint8_t status,
                                  uint8_t* buffer);
    
    /**
     * @brief Parse a received message
     * @param data Raw message data
     * @param length Length of message data
     * @param message Output parsed message structure
     * @return true if message parsed successfully, false on error
     */
    static bool parseMessage(const uint8_t* data, size_t length, Message* message);
    
    /**
     * @brief Validate message header
     * @param header Message header to validate
     * @return true if header is valid
     */
    static bool validateHeader(const MessageHeader* header);
    
    /**
     * @brief Get sensor payload from message
     * @param message Message to extract from
     * @return Pointer to sensor payload, NULL if not a sensor message
     */
    static const SensorPayload* getSensorPayload(const Message* message);
    
    /**
     * @brief Get actuator payload from message
     * @param message Message to extract from
     * @return Pointer to actuator payload, NULL if not an actuator message
     */
    static const ActuatorPayload* getActuatorPayload(const Message* message);
    
    /**
     * @brief Get heartbeat payload from message
     * @param message Message to extract from
     * @return Pointer to heartbeat payload, NULL if not a heartbeat message
     */
    static const HeartbeatPayload* getHeartbeatPayload(const Message* message);
    
    /**
     * @brief Get registration payload from message
     * @param message Message to extract from
     * @return Pointer to registration payload, NULL if not a registration message
     */
    static const RegistrationPayload* getRegistrationPayload(const Message* message);
    
    /**
     * @brief Get ACK payload from message
     * @param message Message to extract from
     * @return Pointer to ACK payload, NULL if not an ACK message
     */
    static const AckPayload* getAckPayload(const Message* message);

private:
    /**
     * @brief Create message header
     * @param type Message type
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param header Output header structure
     */
    static void createHeader(uint8_t type, uint16_t src_addr, uint16_t dst_addr, 
                            uint8_t seq_num, MessageHeader* header);
};