#pragma once

#include <functional>
#include <map>
#include <memory>
#include <queue>

#include "../hal/logger.h"
#include "message.h"
#include "message_builder.h"
#include "message_validator.h"
#include "network_stats.h"
#include "radio_interface.h"
#include "retry_policy.h"

/**
 * @brief Recently seen message for deduplication
 *
 * Tracks (src_addr, seq_num) pairs to avoid processing the same
 * reliable message multiple times when retransmissions occur.
 */
struct SeenMessage {
    uint16_t src_addr;
    uint8_t seq_num;
    uint32_t timestamp;  // When this message was first seen
};

// Number of recent messages to track for deduplication
constexpr size_t SEEN_MESSAGE_BUFFER_SIZE = 16;

// How long to remember seen messages (in milliseconds)
constexpr uint32_t SEEN_MESSAGE_EXPIRY_MS = 30000;  // 30 seconds

/**
 * @brief Callback type for ACK received notification
 *
 * Called when an ACK is received for a message. Provides the sequence number,
 * ACK status, and optional user context (can be used for flash record index).
 */
using AckCallback = std::function<void(uint8_t seq_num, uint8_t ack_status, uint64_t user_context)>;

/**
 * @brief Outgoing message waiting to be transmitted
 */
struct OutgoingMessage {
    std::unique_ptr<uint8_t[]> buffer;
    size_t length;
    DeliveryCriticality criticality;
    uint8_t seq_num;
    uint8_t attempts;          // Number of previous transmission attempts
    AckCallback ack_callback;  // Carried through retries so callback survives re-queuing
    uint64_t user_context;     // Carried through retries alongside ack_callback
};

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
    AckCallback ack_callback;  // Optional callback when ACK received
    uint64_t user_context;     // User-defined context (e.g., flash record index)
};

/**
 * @brief Reliable message delivery with criticality-based ACK/retry mechanism
 */
class ReliableMessenger {
public:
    // Callback type for actuator commands
    using ActuatorCallback = std::function<void(const ActuatorPayload *)>;

    // Callback type for update available messages
    using UpdateCallback = std::function<void(const UpdateAvailablePayload *)>;

    // Callback type for heartbeat response messages
    using HeartbeatResponseCallback = std::function<void(const HeartbeatResponsePayload *)>;

    // Callback type for registration success (provides new assigned address)
    using RegistrationSuccessCallback = std::function<void(uint16_t new_address)>;

    ReliableMessenger(RadioInterface *lora, uint16_t node_addr, NetworkStats *stats = nullptr);

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
                             const uint8_t *params, uint8_t param_length,
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
    bool sendSensorData(uint16_t dst_addr, uint8_t sensor_type, const uint8_t *data,
                        uint8_t data_length, DeliveryCriticality criticality = BEST_EFFORT);

    /**
     * @brief Send sensor data with ACK callback
     * @param dst_addr Destination address
     * @param sensor_type Type of sensor
     * @param data Sensor data
     * @param data_length Length of sensor data
     * @param criticality Delivery criticality (BEST_EFFORT, RELIABLE, CRITICAL)
     * @param ack_callback Callback invoked when ACK is received
     * @param user_context User-defined context passed to callback
     * @return Sequence number of sent message (0 on failure)
     */
    uint8_t sendSensorDataWithCallback(uint16_t dst_addr, uint8_t sensor_type, const uint8_t *data,
                                       uint8_t data_length, DeliveryCriticality criticality,
                                       AckCallback ack_callback, uint64_t user_context = 0);

    /**
     * @brief Send heartbeat message with node status
     * @param dst_addr Destination address (usually hub)
     * @param uptime_seconds Node uptime in seconds
     * @param battery_level Battery percentage (0-100, 255=external power)
     * @param signal_strength Last received RSSI (absolute value)
     * @param active_sensors Bitmask of active sensors
     * @param error_flags Error status flags
     * @param pending_records Untransmitted sensor records in flash backlog
     * @param criticality Delivery criticality (default BEST_EFFORT, use RELIABLE for time sync)
     * @return true if heartbeat sent successfully
     */
    bool sendHeartbeat(uint16_t dst_addr, uint32_t uptime_seconds, uint8_t battery_level,
                       uint8_t signal_strength, uint8_t active_sensors, uint16_t error_flags,
                       uint16_t pending_records = 0, uint64_t device_id = 0,
                       DeliveryCriticality criticality = BEST_EFFORT,
                       AckCallback ack_callback = nullptr, uint64_t user_context = 0);

    /**
     * @brief Send heartbeat response with current datetime (hub to node)
     * @param dst_addr Destination address (node)
     * @param year Current year (0-4095)
     * @param month Current month (1-12)
     * @param day Current day (1-31)
     * @param dotw Day of week (0-6, 0=Sunday)
     * @param hour Current hour (0-23)
     * @param min Current minute (0-59)
     * @param sec Current second (0-59)
     * @param pending_update_flags PENDING_FLAG_* bitmask of queued updates
     * @return true if heartbeat response sent successfully
     */
    bool sendHeartbeatResponse(uint16_t dst_addr, int16_t year, int8_t month, int8_t day,
                               int8_t dotw, int8_t hour, int8_t min, int8_t sec,
                               uint8_t pending_update_flags = 0);

    /**
     * @brief Send registration request to hub
     * @param dst_addr Destination address (usually hub)
     * @param device_id Unique device identifier
     * @param node_type Type of node (sensor, actuator, etc.)
     * @param capabilities Bitmask of node capabilities
     * @param firmware_ver Firmware version
     * @param device_name Human-readable device name
     * @return Sequence number of sent message (0 on failure)
     */
    uint8_t sendRegistrationRequest(uint16_t dst_addr, uint64_t device_id, uint8_t node_type,
                                    uint8_t capabilities, uint32_t firmware_ver,
                                    const char *device_name);

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
    bool sendRegistrationResponse(uint16_t dst_addr, uint64_t device_id, uint16_t assigned_addr,
                                  uint8_t status, uint8_t retry_interval, uint32_t network_time);

    /**
     * @brief Send CHECK_UPDATES message to hub
     * @param dst_addr Destination address (usually hub)
     * @param node_sequence Current sequence number known by node
     * @return Sequence number of sent message (0 on failure)
     */
    uint8_t sendCheckUpdates(uint16_t dst_addr, uint8_t node_sequence);

    /**
     * @brief Send batch of sensor data records from flash backlog
     * @param dst_addr Destination address (hub)
     * @param start_index Flash buffer start index (for tracking)
     * @param records Array of batch sensor records
     * @param record_count Number of records (1-20)
     * @param criticality Delivery criticality
     * @return Sequence number of sent message (0 on failure)
     */
    uint8_t sendSensorDataBatch(uint16_t dst_addr, uint32_t start_index,
                                const BatchSensorRecord *records, uint8_t record_count,
                                DeliveryCriticality criticality = RELIABLE);

    /**
     * @brief Send batch of sensor data records with ACK callback
     * @param dst_addr Destination address (hub)
     * @param start_index Flash buffer start index (for tracking)
     * @param records Array of batch sensor records
     * @param record_count Number of records (1-20)
     * @param criticality Delivery criticality
     * @param ack_callback Callback invoked when ACK is received
     * @param user_context User-defined context passed to callback
     * @return Sequence number of sent message (0 on failure)
     */
    uint8_t sendSensorDataBatchWithCallback(uint16_t dst_addr, uint32_t start_index,
                                            const BatchSensorRecord *records, uint8_t record_count,
                                            DeliveryCriticality criticality,
                                            AckCallback ack_callback, uint64_t user_context = 0);

    /**
     * @brief Generic send method for any message type
     * @param buffer Pre-built message buffer
     * @param length Message length
     * @param criticality Delivery criticality
     * @return true if message sent/queued successfully
     */
    bool send(const uint8_t *buffer, size_t length, DeliveryCriticality criticality = BEST_EFFORT);

    /**
     * @brief Generic send method with ACK callback
     * @param buffer Pre-built message buffer
     * @param length Message length
     * @param criticality Delivery criticality
     * @param ack_callback Callback invoked when ACK is received
     * @param user_context User-defined context passed to callback
     * @return Sequence number of sent message (0 on failure)
     */
    uint8_t sendWithCallback(const uint8_t *buffer, size_t length, DeliveryCriticality criticality,
                             AckCallback ack_callback, uint64_t user_context = 0);

    /**
     * @brief Process incoming messages and handle ACKs
     * @param buffer Received message buffer
     * @param length Length of received message
     * @return true if message processed (may be ACK or new message)
     */
    bool processIncomingMessage(const uint8_t *buffer, size_t length);

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
     * @brief Get the next sequence number (for persistence across sleep)
     * @return Current next sequence number
     */
    uint8_t getNextSeqNum() const { return next_seq_num_; }

    /**
     * @brief Set the next sequence number (restore from persistent storage)
     * @param seq_num Sequence number to restore
     */
    void setNextSeqNum(uint8_t seq_num) { next_seq_num_ = seq_num; }

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

    /**
     * @brief Set callback for heartbeat response messages
     * @param callback Function to call when HEARTBEAT_RESPONSE is received
     */
    void setHeartbeatResponseCallback(HeartbeatResponseCallback callback)
    {
        heartbeat_response_callback_ = callback;
    }

    /**
     * @brief Set callback for registration success
     * @param callback Function to call when registration succeeds (receives new address)
     */
    void setRegistrationSuccessCallback(RegistrationSuccessCallback callback)
    {
        registration_success_callback_ = callback;
    }

    /**
     * @brief Update node address (used after re-registration)
     * @param new_addr New assigned address
     */
    void setNodeAddress(uint16_t new_addr) { node_addr_ = new_addr; }

    /**
     * @brief Cancel a pending message by sequence number
     * @param seq_num Sequence number of message to cancel
     * @return true if message was found and cancelled
     */
    bool cancelPendingMessage(uint8_t seq_num);

private:
    RadioInterface *lora_;
    uint16_t node_addr_;
    uint8_t next_seq_num_;
    std::map<uint8_t, PendingMessage> pending_messages_;
    std::queue<OutgoingMessage> message_queue_;
    bool is_transmitting_;
    Logger logger_;
    NetworkStats *network_stats_;
    ActuatorCallback actuator_callback_;
    UpdateCallback update_callback_;
    HeartbeatResponseCallback heartbeat_response_callback_;
    RegistrationSuccessCallback registration_success_callback_;

    // Deduplication: ring buffer of recently seen messages
    SeenMessage seen_messages_[SEEN_MESSAGE_BUFFER_SIZE] = {};
    size_t seen_messages_index_ = 0;

    /**
     * @brief Check if a message was recently seen (for deduplication)
     * @param src_addr Source address of the message
     * @param seq_num Sequence number of the message
     * @return true if this (src_addr, seq_num) was recently processed
     */
    bool wasRecentlySeen(uint16_t src_addr, uint8_t seq_num);

    /**
     * @brief Mark a message as seen (add to deduplication buffer)
     * @param src_addr Source address of the message
     * @param seq_num Sequence number of the message
     */
    void markAsSeen(uint16_t src_addr, uint8_t seq_num);

    /**
     * @brief Send a message immediately
     * @param buffer Message buffer
     * @param length Message length
     * @return true if sent successfully
     */
    bool sendMessage(const uint8_t *buffer, size_t length);

    /**
     * @brief Handle received ACK message
     * @param ack_payload ACK payload
     */
    void handleAck(const AckPayload *ack_payload);

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
    template <typename CreateFunc>
    bool sendWithBuilder(CreateFunc create, DeliveryCriticality criticality, const char *msg_type)
    {
        if (!lora_)
            return false;

        uint8_t buffer[MESSAGE_MAX_SIZE];
        size_t length = create(buffer);

        if (length == 0) {
            logger_.error("Failed to create %s message", msg_type);
            return false;
        }

        return send(buffer, length, criticality);
    }
};