#pragma once

#include "message.h"
#include "message_builder.h"
#include "message_validator.h"
#include "retry_policy.h"
#include "sx1276.h"
#include "network_stats.h"
#include "../hal/logger.h"
#include <map>
#include <memory>
#include <functional>

/**
 * @brief Pending message for retry tracking
 */
struct PendingMessage {
    std::unique_ptr<uint8_t[]> buffer;
    size_t length;
    uint16_t dst_addr;
    uint32_t send_time;
    uint8_t attempts;
    DeliveryCriticality criticality;
    RetryPolicy::RetryConfig retry_config;
};

/**
 * @brief Reliable message delivery with criticality-based ACK/retry mechanism
 */
class ReliableMessenger {
public:
    // Callback type for actuator commands
    using ActuatorCallback = std::function<void(const ActuatorPayload*)>;

    // Callback type for update available messages
    using UpdateCallback = std::function<void(const UpdateAvailablePayload*)>;

    ReliableMessenger(SX1276* lora, uint16_t node_addr, NetworkStats* stats = nullptr);
    
    /**
     * @brief Send an actuator command with specified criticality
     * @param dst_addr Destination address
     * @param actuator_type Type of actuator
     * @param command Command to execute
     * @param params Command parameters
     * @param param_length Length of parameters
     * @param criticality Delivery criticality (BEST_EFFORT, RELIABLE, CRITICAL)
     * @return true if message sent/queued successfully
     */
    bool sendActuatorCommand(uint16_t dst_addr, uint8_t actuator_type, uint8_t command,
                            const uint8_t* params, uint8_t param_length, 
                            DeliveryCriticality criticality = RELIABLE);
    
    /**
     * @brief Send sensor data with specified criticality
     * @param dst_addr Destination address
     * @param sensor_type Type of sensor
     * @param data Sensor data
     * @param data_length Length of sensor data
     * @param criticality Delivery criticality (BEST_EFFORT, RELIABLE, CRITICAL)
     * @return true if message sent/queued successfully
     */
    bool sendSensorData(uint16_t dst_addr, uint8_t sensor_type, 
                       const uint8_t* data, uint8_t data_length,
                       DeliveryCriticality criticality = BEST_EFFORT);
    
    /**
     * @brief Send heartbeat message with node status
     * @param dst_addr Destination address (usually hub)
     * @param uptime_seconds Node uptime in seconds
     * @param battery_level Battery percentage (0-100, 255=external power)
     * @param signal_strength Last received RSSI (absolute value)
     * @param active_sensors Bitmask of active sensors
     * @param error_flags Error status flags
     * @return true if heartbeat sent successfully
     */
    bool sendHeartbeat(uint16_t dst_addr, uint32_t uptime_seconds, 
                      uint8_t battery_level, uint8_t signal_strength,
                      uint8_t active_sensors, uint8_t error_flags);
    
    /**
     * @brief Send registration request to hub
     * @param dst_addr Destination address (usually hub)
     * @param device_id Unique device identifier
     * @param node_type Type of node (sensor, actuator, etc.)
     * @param capabilities Bitmask of node capabilities
     * @param firmware_ver Firmware version
     * @param device_name Human-readable device name
     * @return true if registration request sent successfully
     */
    bool sendRegistrationRequest(uint16_t dst_addr, uint64_t device_id,
                               uint8_t node_type, uint8_t capabilities,
                               uint16_t firmware_ver, const char* device_name);
    
    /**
     * @brief Send registration response to node
     * @param dst_addr Destination address (requesting node)
     * @param device_id Device ID from registration request
     * @param assigned_addr Assigned address for the node
     * @param status Registration status (success/error)
     * @param retry_interval Retry interval for failed registrations
     * @param network_time Current network time
     * @return true if registration response sent successfully
     */
    bool sendRegistrationResponse(uint16_t dst_addr, uint64_t device_id,
                                uint16_t assigned_addr, uint8_t status,
                                uint8_t retry_interval, uint32_t network_time);

    /**
     * @brief Send CHECK_UPDATES message to hub
     * @param dst_addr Destination address (usually hub)
     * @param node_sequence Current sequence number known by node
     * @return true if message sent successfully
     */
    bool sendCheckUpdates(uint16_t dst_addr, uint8_t node_sequence);

    /**
     * @brief Generic send method for any message type
     * @param buffer Pre-built message buffer
     * @param length Message length
     * @param criticality Delivery criticality
     * @return true if message sent/queued successfully
     */
    bool send(const uint8_t* buffer, size_t length,
              DeliveryCriticality criticality = BEST_EFFORT);
    
    /**
     * @brief Process incoming messages and handle ACKs
     * @param buffer Received message buffer
     * @param length Length of received message
     * @return true if message processed (may be ACK or new message)
     */
    bool processIncomingMessage(const uint8_t* buffer, size_t length);
    
    /**
     * @brief Update retry timers and resend messages if needed
     * Call this regularly in main loop
     */
    void update();
    
    /**
     * @brief Get number of pending messages awaiting ACK
     */
    size_t getPendingCount() const { return pending_messages_.size(); }
    
    /**
     * @brief Check if a specific message was acknowledged
     * @param seq_num Sequence number to check
     * @return true if ACK received for this sequence number
     */
    bool wasAcknowledged(uint8_t seq_num);
    
    /**
     * @brief Update the node address (used after successful registration)
     * @param new_addr New address to use for sending messages
     */
    void updateNodeAddress(uint16_t new_addr);
    
    /**
     * @brief Get current node address
     * @return Current node address
     */
    uint16_t getNodeAddress() const { return node_addr_; }
    
    /**
     * @brief Set callback for actuator commands
     * @param callback Function to call when actuator command is received
     */
    void setActuatorCallback(ActuatorCallback callback) { actuator_callback_ = callback; }

    /**
     * @brief Set callback for update available messages
     * @param callback Function to call when UPDATE_AVAILABLE is received
     */
    void setUpdateCallback(UpdateCallback callback) { update_callback_ = callback; }

private:
    SX1276* lora_;
    uint16_t node_addr_;
    uint8_t next_seq_num_;
    std::map<uint8_t, PendingMessage> pending_messages_;
    Logger logger_;
    NetworkStats* network_stats_;
    ActuatorCallback actuator_callback_;
    UpdateCallback update_callback_;
    
    /**
     * @brief Send a message immediately
     * @param buffer Message buffer
     * @param length Message length
     * @return true if sent successfully
     */
    bool sendMessage(const uint8_t* buffer, size_t length);
    
    /**
     * @brief Handle received ACK message
     * @param ack_payload ACK payload
     */
    void handleAck(const AckPayload* ack_payload);
    
    /**
     * @brief Send ACK for received actuator command
     * @param src_addr Original sender address
     * @param seq_num Sequence number to acknowledge
     * @param status ACK status (0=success)
     */
    void sendAck(uint16_t src_addr, uint8_t seq_num, uint8_t status);
    
    
    /**
     * @brief Get current time in milliseconds
     */
    uint32_t getCurrentTime();
    
    /**
     * @brief Get next sequence number within appropriate range
     * Hub uses 1-127, nodes use 128-255 to prevent collisions
     */
    uint8_t getNextSequenceNumber();
    
    /**
     * @brief Template method for sending messages with common pattern
     * @tparam CreateFunc Function/lambda that creates the message
     * @param create Function that takes buffer and returns message length
     * @param criticality Delivery criticality
     * @param msg_type Message type name for logging
     * @return true if message sent/queued successfully
     */
    template<typename CreateFunc>
    bool sendWithBuilder(CreateFunc create, DeliveryCriticality criticality, const char* msg_type) {
        if (!lora_) return false;
        
        uint8_t buffer[MESSAGE_MAX_SIZE];
        size_t length = create(buffer);
        
        if (length == 0) {
            logger_.error("Failed to create %s message", msg_type);
            return false;
        }
        
        return send(buffer, length, criticality);
    }
};