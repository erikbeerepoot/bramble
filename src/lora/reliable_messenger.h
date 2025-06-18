#pragma once

#include "message.h"
#include "sx1276.h"
#include <map>

// Retry configuration
#define MAX_RETRIES 3
#define ACK_TIMEOUT_MS 5000
#define RETRY_BASE_DELAY_MS 1000

/**
 * @brief Pending message for retry tracking
 */
struct PendingMessage {
    uint8_t buffer[MESSAGE_MAX_SIZE];
    size_t length;
    uint8_t seq_num;
    uint16_t dst_addr;
    uint32_t last_send_time;
    uint8_t retry_count;
    uint32_t next_retry_time;
    bool ack_received;
};

/**
 * @brief Reliable message delivery with criticality-based ACK/retry mechanism
 */
class ReliableMessenger {
public:
    ReliableMessenger(SX1276* lora, uint16_t node_addr);
    
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

private:
    SX1276* lora_;
    uint16_t node_addr_;
    uint8_t next_seq_num_;
    std::map<uint8_t, PendingMessage> pending_messages_;
    
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
     * @brief Calculate next retry delay with exponential backoff
     * @param retry_count Current retry attempt (0-based)
     * @return Delay in milliseconds
     */
    uint32_t calculateRetryDelay(uint8_t retry_count);
    
    /**
     * @brief Get current time in milliseconds
     */
    uint32_t getCurrentTime();
};