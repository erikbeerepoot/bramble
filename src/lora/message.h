#pragma once

#include <stddef.h>
#include <stdint.h>

// Message protocol constants
constexpr uint16_t MESSAGE_MAGIC = 0xBEEF;
constexpr size_t MESSAGE_HEADER_SIZE = 9;
constexpr size_t MESSAGE_MAX_PAYLOAD = 247;
constexpr size_t MESSAGE_MAX_SIZE = MESSAGE_HEADER_SIZE + MESSAGE_MAX_PAYLOAD;

// Payload data limits
constexpr size_t MAX_SENSOR_DATA_LENGTH = 32;  // Maximum sensor data array size
constexpr size_t MAX_ACTUATOR_PARAMS = 16;     // Maximum actuator parameters array size

// Message flags
constexpr uint8_t MSG_FLAG_RELIABLE = 0x01;   // Requires ACK
constexpr uint8_t MSG_FLAG_CRITICAL = 0x02;   // Critical message (persistent retry)
constexpr uint8_t MSG_FLAG_PRIORITY = 0x04;   // High priority message
constexpr uint8_t MSG_FLAG_BROADCAST = 0x08;  // Broadcast message (no ACK expected)

// Address definitions
constexpr uint16_t ADDRESS_HUB = 0x0000;           // Hub/gateway address
constexpr uint16_t ADDRESS_UNREGISTERED = 0xFFFF;  // Temporary address for unregistered nodes
constexpr uint16_t ADDRESS_BROADCAST = 0xFFFE;     // Broadcast to all registered nodes
constexpr uint16_t ADDRESS_MIN_NODE = 0x0001;      // First assignable node address
constexpr uint16_t ADDRESS_MAX_NODE = 0xFFFD;      // Last assignable node address

// Message types
enum MessageType {
    MSG_TYPE_SENSOR_DATA = 0x01,         // Fire-and-forget sensor readings
    MSG_TYPE_ACTUATOR_CMD = 0x02,        // Reliable actuator commands
    MSG_TYPE_ACK = 0x03,                 // Acknowledgment
    MSG_TYPE_HEARTBEAT = 0x04,           // Node heartbeat/status
    MSG_TYPE_REGISTRATION = 0x05,        // Node registration request
    MSG_TYPE_REG_RESPONSE = 0x06,        // Registration response with address assignment
    MSG_TYPE_CONFIG = 0x07,              // Configuration updates
    MSG_TYPE_ROUTE = 0x08,               // Message routing (future use)
    MSG_TYPE_CHECK_UPDATES = 0x09,       // Node → Hub: Check for pending updates
    MSG_TYPE_UPDATE_AVAILABLE = 0x0A,    // Hub → Node: Update response (ACK'd with MSG_TYPE_ACK)
    MSG_TYPE_HEARTBEAT_RESPONSE = 0x0B,  // Hub → Node: Heartbeat response with current time
    MSG_TYPE_SENSOR_DATA_BATCH = 0x0C,   // Batch transmission of sensor records from flash
    MSG_TYPE_BATCH_ACK = 0x0D            // Batch acknowledgment from hub
};

// Sensor data subtypes
enum SensorType {
    SENSOR_TEMPERATURE = 0x01,
    SENSOR_HUMIDITY = 0x02,
    SENSOR_BATTERY = 0x03,
    SENSOR_SOIL_MOISTURE = 0x04
};

// Actuator command subtypes
enum ActuatorType {
    ACTUATOR_VALVE = 0x01,
    ACTUATOR_PUMP = 0x02,
    ACTUATOR_FAN = 0x03
};

// Actuator commands
enum ActuatorCommand {
    CMD_TURN_OFF = 0x00,
    CMD_TURN_ON = 0x01,
    CMD_TOGGLE = 0x02,
    CMD_SET_LEVEL = 0x03  // For variable speed/position actuators
};

// Node types for registration
enum NodeType {
    NODE_TYPE_SENSOR = 0x01,     // Sensor-only node
    NODE_TYPE_ACTUATOR = 0x02,   // Actuator-only node
    NODE_TYPE_HYBRID = 0x03,     // Both sensor and actuator
    NODE_TYPE_REPEATER = 0x04,   // Range extender (future)
    NODE_TYPE_CONTROLLER = 0x05  // Controller/hub node
};

// Node capability flags
// Sensor capabilities (bits 0-3)
constexpr uint8_t CAP_TEMPERATURE = 0x01;      // Temperature sensor
constexpr uint8_t CAP_HUMIDITY = 0x02;         // Humidity sensor
constexpr uint8_t CAP_SOIL_MOISTURE = 0x04;    // Soil moisture sensor
constexpr uint8_t CAP_BATTERY_MONITOR = 0x08;  // Battery level monitoring

// Actuator capabilities (bits 4-6)
constexpr uint8_t CAP_VALVE_CONTROL = 0x10;  // Valve actuator
constexpr uint8_t CAP_PUMP_CONTROL = 0x20;   // Pump actuator
constexpr uint8_t CAP_FAN_CONTROL = 0x40;    // Fan actuator

// System capabilities (bit 7 and extended)
constexpr uint8_t CAP_SOLAR_POWERED = 0x80;  // Solar powered node

// Extended capabilities (for controller nodes, use multiple flags)
constexpr uint8_t CAP_CONTROLLER =
    CAP_VALVE_CONTROL | CAP_PUMP_CONTROL | CAP_FAN_CONTROL;  // Full actuator control
constexpr uint8_t CAP_SCHEDULING = CAP_BATTERY_MONITOR;  // Scheduling requires timing/monitoring

// Error flags for HeartbeatPayload
// These indicate various node health conditions
constexpr uint8_t ERR_FLAG_NONE              = 0x00;  // No errors
constexpr uint8_t ERR_FLAG_SENSOR_FAILURE    = 0x01;  // Temp/humidity sensor not responding
constexpr uint8_t ERR_FLAG_FLASH_FAILURE     = 0x02;  // External flash not responding
constexpr uint8_t ERR_FLAG_FLASH_FULL        = 0x04;  // Flash storage >90% full
constexpr uint8_t ERR_FLAG_PMU_FAILURE       = 0x08;  // PMU communication failure
constexpr uint8_t ERR_FLAG_BATTERY_LOW       = 0x10;  // Battery <20% (warning)
constexpr uint8_t ERR_FLAG_BATTERY_CRITICAL  = 0x20;  // Battery <10% (critical)
constexpr uint8_t ERR_FLAG_RTC_NOT_SYNCED    = 0x40;  // RTC never synchronized
constexpr uint8_t ERR_FLAG_RADIO_ISSUE       = 0x80;  // LoRa transmission issues

// Registration status codes
enum RegistrationStatus {
    REG_SUCCESS = 0x00,             // Registration successful
    REG_ERROR_FULL = 0x01,          // Network full (no addresses available)
    REG_ERROR_DUPLICATE = 0x02,     // Device ID already registered
    REG_ERROR_INVALID = 0x03,       // Invalid registration data
    REG_ERROR_HUB_BUSY = 0x04,      // Hub temporarily unavailable
    REG_ERROR_INTERNAL = 0x05,      // Internal hub error
    REG_REREGISTER_REQUIRED = 0x06  // Node unknown, must re-register with device_id
};

// Delivery criticality levels
enum DeliveryCriticality {
    BEST_EFFORT = 0,  // Fire and forget (default)
    RELIABLE = 1,     // ACK required, limited retries
    CRITICAL = 2      // ACK required, persistent retry
};

// Update types for MSG_TYPE_UPDATE_AVAILABLE
enum class UpdateType : uint8_t {
    SET_SCHEDULE = 0x01,      // Add/modify schedule entry
    REMOVE_SCHEDULE = 0x02,   // Remove schedule entry
    SET_DATETIME = 0x03,      // Sync RTC date/time
    SET_WAKE_INTERVAL = 0x04  // Change periodic wake interval
};

/**
 * @brief Message header structure
 */
struct __attribute__((packed)) MessageHeader {
    uint16_t magic;     // Magic number (0xBEEF)
    uint8_t type;       // Message type
    uint8_t flags;      // Message flags (reliability, priority, etc.)
    uint16_t src_addr;  // Source address
    uint16_t dst_addr;  // Destination address
    uint8_t seq_num;    // Sequence number
};

/**
 * @brief Sensor data payload
 */
struct __attribute__((packed)) SensorPayload {
    uint8_t sensor_type;  // Sensor type
    uint8_t data_length;  // Length of sensor data
    uint8_t data[32];     // Sensor data (flexible format)
};

/**
 * @brief Actuator command payload
 */
struct __attribute__((packed)) ActuatorPayload {
    uint8_t actuator_type;  // Actuator type
    uint8_t command;        // Command to execute
    uint8_t param_length;   // Length of parameters
    uint8_t params[16];     // Command parameters
};

/**
 * @brief Heartbeat payload
 */
struct __attribute__((packed)) HeartbeatPayload {
    uint32_t uptime_seconds;  // How long node has been running
    uint8_t battery_level;    // Battery percentage (0-100, 255=external power)
    uint8_t signal_strength;  // Last received RSSI (absolute value)
    uint8_t active_sensors;   // Bitmask of active sensors
    uint8_t error_flags;      // Error status flags
};

/**
 * @brief Heartbeat response payload (hub sends current datetime to node)
 */
struct __attribute__((packed)) HeartbeatResponsePayload {
    int16_t year;  // 0..4095 (e.g., 2025)
    int8_t month;  // 1..12 (1=January)
    int8_t day;    // 1..28,29,30,31
    int8_t dotw;   // 0..6 (0=Sunday, 1=Monday, ..., 6=Saturday)
    int8_t hour;   // 0..23
    int8_t min;    // 0..59
    int8_t sec;    // 0..59
};

/**
 * @brief Registration request payload
 */
struct __attribute__((packed)) RegistrationPayload {
    uint64_t device_id;     // Unique device identifier (chip serial/MAC)
    uint8_t node_type;      // Node type (sensor/actuator/hybrid)
    uint8_t capabilities;   // Capability flags (what sensors/actuators available)
    uint16_t firmware_ver;  // Firmware version
    char device_name[16];   // Human readable device name
};

/**
 * @brief Registration response payload
 */
struct __attribute__((packed)) RegistrationResponsePayload {
    uint64_t device_id;      // Echo back the device ID being registered
    uint16_t assigned_addr;  // Assigned node address (0x0000 if registration failed)
    uint8_t status;          // Registration status (RegistrationStatus enum)
    uint8_t retry_interval;  // Seconds to wait before retry if failed
    uint32_t network_time;   // Network time for synchronization
};

/**
 * @brief ACK payload
 */
struct __attribute__((packed)) AckPayload {
    uint8_t ack_seq_num;  // Sequence number being acknowledged
    uint8_t status;       // ACK status (0=success, non-zero=error)
};

/**
 * @brief Check updates payload (Node → Hub)
 */
struct __attribute__((packed)) CheckUpdatesPayload {
    uint8_t node_sequence;  // Node's current update sequence number
};

/**
 * @brief Update available payload (Hub → Node)
 */
struct __attribute__((packed)) UpdateAvailablePayload {
    uint8_t has_update;        // 0=no updates, 1=update follows
    uint8_t update_type;       // UpdateType enum
    uint8_t sequence;          // Hub's sequence for this update
    uint8_t payload_data[24];  // Type-specific data
};

/**
 * @brief Compact sensor data record for batch transmission
 *
 * Matches SensorDataRecord from storage but packed for wire transmission.
 * Each record is 12 bytes.
 */
struct __attribute__((packed)) BatchSensorRecord {
    uint32_t timestamp;   // Unix timestamp (seconds since epoch)
    int16_t temperature;  // Temperature in 0.01°C units
    uint16_t humidity;    // Humidity in 0.01% units
    uint8_t flags;        // Status flags
    uint8_t reserved;     // Reserved for future use
    uint16_t crc16;       // CRC16 of record
};

// Batch size constants
// Max records limited by LoRa packet size: 255 - 9 (header) - 7 (batch header) = 239 bytes
// 239 / 12 bytes per record = 19.9, so max 19 records
constexpr size_t MAX_BATCH_RECORDS = 19;
constexpr size_t BATCH_RECORD_SIZE = sizeof(BatchSensorRecord);                         // 12 bytes
constexpr size_t MAX_BATCH_PAYLOAD_SIZE = 7 + (MAX_BATCH_RECORDS * BATCH_RECORD_SIZE);  // 235 bytes

/**
 * @brief Sensor data batch payload (Node → Hub)
 *
 * Transmits multiple sensor records in a single message for efficient
 * catch-up after network outages. Max 19 records per batch (LoRa packet limit).
 */
struct __attribute__((packed)) SensorDataBatchPayload {
    uint16_t node_addr;                            // Source node address (for routing/validation)
    uint32_t start_index;                          // Flash buffer start index (for debugging)
    uint8_t record_count;                          // Number of records in this batch (1-19)
    BatchSensorRecord records[MAX_BATCH_RECORDS];  // Sensor records
};

/**
 * @brief Batch acknowledgment payload (Hub → Node)
 *
 * Acknowledges receipt of a batch. Node can mark records as transmitted.
 */
struct __attribute__((packed)) BatchAckPayload {
    uint8_t ack_seq_num;       // Sequence number being acknowledged
    uint8_t status;            // ACK status (0=success)
    uint8_t records_received;  // Number of records successfully stored
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
     * @brief Convert message type to human-readable string
     * @param type Message type enum value
     * @return String representation of message type
     */
    static const char *getMessageTypeName(uint8_t type);
    /**
     * @brief Create a sensor data message
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param sensor_type Type of sensor
     * @param data Sensor data
     * @param data_length Length of sensor data
     * @param flags Message flags (reliability, priority, etc.)
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createSensorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                      uint8_t sensor_type, const uint8_t *data, uint8_t data_length,
                                      uint8_t flags, uint8_t *buffer);

    /**
     * @brief Create an actuator command message
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param actuator_type Type of actuator
     * @param command Command to execute
     * @param params Command parameters
     * @param param_length Length of parameters
     * @param flags Message flags (reliability, priority, etc.)
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createActuatorMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                        uint8_t actuator_type, uint8_t command,
                                        const uint8_t *params, uint8_t param_length, uint8_t flags,
                                        uint8_t *buffer);

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
                                         uint8_t *buffer);

    /**
     * @brief Create a heartbeat response message with current datetime
     * @param src_addr Source address (hub)
     * @param dst_addr Destination address (node)
     * @param seq_num Sequence number
     * @param year Current year (0-4095)
     * @param month Current month (1-12)
     * @param day Current day (1-31)
     * @param dotw Current day of week (0-6, 0=Sunday)
     * @param hour Current hour (0-23)
     * @param min Current minute (0-59)
     * @param sec Current second (0-59)
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createHeartbeatResponseMessage(uint16_t src_addr, uint16_t dst_addr,
                                                 uint8_t seq_num, int16_t year, int8_t month,
                                                 int8_t day, int8_t dotw, int8_t hour, int8_t min,
                                                 int8_t sec, uint8_t *buffer);

    /**
     * @brief Create a registration request message
     * @param src_addr Source address (ADDRESS_UNREGISTERED for new nodes)
     * @param dst_addr Destination address (ADDRESS_HUB)
     * @param seq_num Sequence number
     * @param device_id Unique device identifier
     * @param node_type Node type identifier
     * @param capabilities Capability flags
     * @param firmware_ver Firmware version
     * @param device_name Device name (max 15 chars + null terminator)
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createRegistrationMessage(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                            uint64_t device_id, uint8_t node_type,
                                            uint8_t capabilities, uint16_t firmware_ver,
                                            const char *device_name, uint8_t *buffer);

    /**
     * @brief Create a registration response message
     * @param src_addr Source address (ADDRESS_HUB)
     * @param dst_addr Destination address (ADDRESS_UNREGISTERED or assigned address)
     * @param seq_num Sequence number
     * @param device_id Device ID being responded to
     * @param assigned_addr Assigned address (0x0000 if registration failed)
     * @param status Registration status
     * @param retry_interval Retry interval in seconds (if failed)
     * @param network_time Current network time
     * @param buffer Output buffer (must be at least MESSAGE_MAX_SIZE bytes)
     * @return Length of created message, 0 on error
     */
    static size_t createRegistrationResponse(uint16_t src_addr, uint16_t dst_addr, uint8_t seq_num,
                                             uint64_t device_id, uint16_t assigned_addr,
                                             uint8_t status, uint8_t retry_interval,
                                             uint32_t network_time, uint8_t *buffer);

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
                                   uint8_t ack_seq_num, uint8_t status, uint8_t *buffer);

    /**
     * @brief Parse a received message
     * @param data Raw message data
     * @param length Length of message data
     * @param message Output parsed message structure
     * @return true if message parsed successfully, false on error
     */
    static bool parseMessage(const uint8_t *data, size_t length, Message *message);

    /**
     * @brief Validate message header
     * @param header Message header to validate
     * @return true if header is valid
     */
    static bool validateHeader(const MessageHeader *header);

    /**
     * @brief Validate message payload content
     * @param header Message header (for type information)
     * @param payload Payload data to validate
     * @param payload_length Length of payload data
     * @return true if payload is valid for the message type
     */
    static bool validatePayload(const MessageHeader *header, const uint8_t *payload,
                                size_t payload_length);

    /**
     * @brief Get sensor payload from message
     * @param message Message to extract from
     * @return Pointer to sensor payload, NULL if not a sensor message
     */
    static const SensorPayload *getSensorPayload(const Message *message);

    /**
     * @brief Get actuator payload from message
     * @param message Message to extract from
     * @return Pointer to actuator payload, NULL if not an actuator message
     */
    static const ActuatorPayload *getActuatorPayload(const Message *message);

    /**
     * @brief Get heartbeat payload from message
     * @param message Message to extract from
     * @return Pointer to heartbeat payload, NULL if not a heartbeat message
     */
    static const HeartbeatPayload *getHeartbeatPayload(const Message *message);

    /**
     * @brief Get heartbeat response payload from message
     * @param message Message to extract from
     * @return Pointer to heartbeat response payload, NULL if not a heartbeat response message
     */
    static const HeartbeatResponsePayload *getHeartbeatResponsePayload(const Message *message);

    /**
     * @brief Get registration request payload from message
     * @param message Message to extract from
     * @return Pointer to registration payload, NULL if not a registration message
     */
    static const RegistrationPayload *getRegistrationPayload(const Message *message);

    /**
     * @brief Get registration response payload from message
     * @param message Message to extract from
     * @return Pointer to registration response payload, NULL if not a registration response
     */
    static const RegistrationResponsePayload *getRegistrationResponsePayload(
        const Message *message);

    /**
     * @brief Get ACK payload from message
     * @param message Message to extract from
     * @return Pointer to ACK payload, NULL if not an ACK message
     */
    static const AckPayload *getAckPayload(const Message *message);

    /**
     * @brief Helper: Check if message requires ACK
     * @param message Message to check
     * @return true if ACK required
     */
    static bool requiresAck(const Message *message);

    /**
     * @brief Helper: Check if message is critical
     * @param message Message to check
     * @return true if critical priority
     */
    static bool isCritical(const Message *message);

private:
    /**
     * @brief Create message header
     * @param type Message type
     * @param flags Message flags
     * @param src_addr Source address
     * @param dst_addr Destination address
     * @param seq_num Sequence number
     * @param header Output header structure
     */
    static void createHeader(uint8_t type, uint8_t flags, uint16_t src_addr, uint16_t dst_addr,
                             uint8_t seq_num, MessageHeader *header);
};