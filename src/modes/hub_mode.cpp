#include "hub_mode.h"

#include <cstring>

#include "pico/stdlib.h"

#include "hardware/uart.h"

#include "hal/flash.h"
#include "hal/logger.h"
#include "hal/pmu_protocol.h"
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"
#include "lora/sx1276.h"

static Logger logger("HubMode");

// UART configuration for Raspberry Pi communication
// Uses UART1 on Feather RP2040 RFM9x module header (D24/D25)
#define API_UART_ID uart1
#define API_UART_TX_PIN 24  // D24 - TX1 on RFM module header
#define API_UART_RX_PIN 25  // D25 - RX1 on RFM module header
#define API_UART_BAUD 115200

constexpr uint32_t STATS_INTERVAL_MS = 30000;             // 30 seconds
constexpr uint32_t MAINTENANCE_INTERVAL_MS = 300000;      // 5 minutes
constexpr uint32_t DATETIME_QUERY_INTERVAL_MS = 3600000;  // 1 hour

void HubMode::onStart()
{
    logger.info("=== HUB MODE ACTIVE ===");
    logger.info("- Managing node registrations");
    logger.info("- Routing node-to-node messages");
    logger.info("- Blue LED indicates hub status");
    logger.info("- API UART on pins %d/%d @ %d baud", API_UART_TX_PIN, API_UART_RX_PIN,
                API_UART_BAUD);

    // Initialize serial input buffer
    serial_input_pos_ = 0;
    last_datetime_sync_ms_ = 0;
    memset(serial_input_buffer_, 0, sizeof(serial_input_buffer_));

    // Initialize UART for Raspberry Pi communication
    uart_init(API_UART_ID, API_UART_BAUD);
    gpio_set_function(API_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(API_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(API_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(API_UART_ID, true);

    logger.info("API UART initialized successfully");
    uart_puts(API_UART_ID, "HUB_READY\n");  // Send ready signal to RasPi

    // Initialize RTC
    rtc_init();
    logger.info("RTC initialized");

    // Request initial time from RasPi
    syncTimeFromRaspberryPi();

    // Hub always uses blue breathing pattern
    led_pattern_ = std::make_unique<BreathingPattern>(led_, 0, 0, 255);

    // Add periodic stats reporting
    task_manager_.addTask(
        [this](uint32_t time) {
            uint32_t routed, queued, dropped;
            hub_router_->getRoutingStats(routed, queued, dropped);
            logger.info("Hub stats - Routed: %lu, Queued: %lu, Dropped: %lu", routed, queued,
                        dropped);

            logger.info("Registered nodes: %u", address_manager_->getRegisteredNodeCount());

            // Print network statistics if available
            if (network_stats_) {
                // Update node counts
                network_stats_->updateNodeCounts(address_manager_->getRegisteredNodeCount(),
                                                 address_manager_->getActiveNodeCount(),
                                                 address_manager_->getRegisteredNodeCount() -
                                                     address_manager_->getActiveNodeCount());
                network_stats_->printSummary();
            }
        },
        STATS_INTERVAL_MS, "Stats Reporting");

    // Add periodic maintenance
    task_manager_.addTask(
        [this](uint32_t time) {
            logger.debug("Performing hub maintenance...");
            hub_router_->clearOldRoutes(time);
            hub_router_->processQueuedMessages();

            // Check for inactive nodes and update network status
            uint32_t inactive_count = address_manager_->checkForInactiveNodes(time);
            if (inactive_count > 0) {
                logger.info("Marked %lu nodes as inactive", inactive_count);
            }

            // Deregister nodes that have been inactive for extended period
            uint32_t deregistered_count = address_manager_->deregisterInactiveNodes(time);
            if (deregistered_count > 0) {
                logger.info("Deregistered %lu nodes (inactive > %lu hours)", deregistered_count,
                            86400000UL / 3600000UL);
                // Persist the updated registry to flash
                Flash flash_hal;
                address_manager_->persist(flash_hal);
            }
        },
        MAINTENANCE_INTERVAL_MS, "Hub Maintenance");

    // Add hourly time sync task
    task_manager_.addTask(
        [this](uint32_t current_time) {
            (void)current_time;  // Unused
            syncTimeFromRaspberryPi();
        },
        DATETIME_QUERY_INTERVAL_MS, "Time Sync");
}

void HubMode::onLoop()
{
    // Process serial input from Raspberry Pi
    processSerialInput();
}

void HubMode::processIncomingMessage(uint8_t *rx_buffer, int rx_len, uint32_t current_time)
{
    logger.debug("Hub received message (len=%d, RSSI=%d dBm)", rx_len, lora_.getRssi());

    // Handle special message types
    if (rx_len >= static_cast<int>(sizeof(MessageHeader))) {
        const MessageHeader *header = reinterpret_cast<const MessageHeader *>(rx_buffer);

        // Check if message is from an unregistered node (not a registration request)
        // and request re-registration if needed
        if (header->src_addr >= ADDRESS_MIN_NODE && header->src_addr <= ADDRESS_MAX_NODE &&
            header->type != MSG_TYPE_REGISTRATION) {
            const NodeInfo *node = address_manager_->getNodeInfo(header->src_addr);
            if (!node) {
                // Node is not registered - check if we should send reregister request
                auto it = last_reregister_request_time_.find(header->src_addr);
                bool should_send = (it == last_reregister_request_time_.end()) ||
                                   (current_time - it->second >= REREGISTER_REQUEST_INTERVAL_MS);

                if (should_send) {
                    printf("Unknown node 0x%04X - sending reregister request\n", header->src_addr);

                    // Send REG_RESPONSE with REG_REREGISTER_REQUIRED status
                    // device_id=0 since we don't know it, assigned_addr=0 to indicate no valid
                    // address
                    messenger_.sendRegistrationResponse(
                        header->src_addr,  // Send to the unknown node
                        0,                 // device_id unknown
                        0,                 // No assigned address
                        REG_REREGISTER_REQUIRED,
                        30,           // Retry interval in seconds
                        current_time  // Network time
                    );

                    last_reregister_request_time_[header->src_addr] = current_time;
                }

                // Still process the message (sensor data is useful even from unknown nodes)
                // But don't call updateLastSeen since the node isn't in the registry
            }
        }

        if (header->type == MSG_TYPE_HEARTBEAT) {
            const HeartbeatPayload *heartbeat =
                reinterpret_cast<const HeartbeatPayload *>(rx_buffer + sizeof(MessageHeader));
            handleHeartbeat(header->src_addr, heartbeat);
            // Fall through to base class for common processing
        } else if (header->type == MSG_TYPE_CHECK_UPDATES) {
            const CheckUpdatesPayload *check =
                reinterpret_cast<const CheckUpdatesPayload *>(rx_buffer + sizeof(MessageHeader));

            // IMPORTANT: Must call messenger to handle ACK before hub_router
            // The messenger will send ACK for this RELIABLE message
            messenger_.processIncomingMessage(rx_buffer, rx_len);

            // Now handle the CHECK_UPDATES in hub_router
            // This sends UPDATE_AVAILABLE response
            hub_router_->handleCheckUpdates(header->src_addr, check->node_sequence);

            // Update node activity tracking (normally done in base class)
            address_manager_->updateLastSeen(header->src_addr, current_time);
            hub_router_->updateRouteOnline(header->src_addr);

            // Don't call base class - would cause double handling of CHECK_UPDATES
            // (base class calls global processIncomingMessage which also calls hub_router)
            return;
        } else if (header->type == MSG_TYPE_SENSOR_DATA) {
            // Forward individual sensor readings to Raspberry Pi
            const SensorPayload *sensor =
                reinterpret_cast<const SensorPayload *>(rx_buffer + sizeof(MessageHeader));
            handleSensorData(header->src_addr, sensor);
            // Fall through to base class for ACK handling if needed
        } else if (header->type == MSG_TYPE_SENSOR_DATA_BATCH) {
            // Forward batch sensor data to Raspberry Pi
            const SensorDataBatchPayload *batch =
                reinterpret_cast<const SensorDataBatchPayload *>(rx_buffer + sizeof(MessageHeader));

            // Process via messenger first (handles ACK for RELIABLE messages)
            messenger_.processIncomingMessage(rx_buffer, rx_len);

            // Store seq_num for later BATCH_ACK response (after RasPi confirms storage)
            pending_batch_seq_num_[header->src_addr] = header->seq_num;

            // Forward to Raspberry Pi
            handleSensorDataBatch(header->src_addr, batch);

            // Update node activity tracking (normally done in base class)
            address_manager_->updateLastSeen(header->src_addr, current_time);
            hub_router_->updateRouteOnline(header->src_addr);

            // Don't call base class - we've handled the message
            return;
        }
    }

    // Call base class implementation which handles common processing
    ApplicationMode::processIncomingMessage(rx_buffer, rx_len, current_time);
}

// ===== Serial Command Processing =====

void HubMode::processSerialInput()
{
    // Read available characters from UART
    while (uart_is_readable(API_UART_ID)) {
        char c = uart_getc(API_UART_ID);

        if (c == '\n' || c == '\r') {
            if (serial_input_pos_ > 0) {
                serial_input_buffer_[serial_input_pos_] = '\0';
                handleSerialCommand(serial_input_buffer_);
                serial_input_pos_ = 0;
            }
        } else if (serial_input_pos_ < sizeof(serial_input_buffer_) - 1) {
            serial_input_buffer_[serial_input_pos_++] = c;
        }
    }
}

void HubMode::handleSerialCommand(const char *cmd)
{
    // Skip empty lines
    if (!cmd || cmd[0] == '\0') {
        return;
    }

    // Skip leading non-printable characters (e.g., 0xFF garbage from UART)
    while (*cmd != '\0' && ((unsigned char)*cmd < 0x20 || (unsigned char)*cmd >= 0x7F)) {
        cmd++;
    }

    // Check again after skipping garbage
    if (cmd[0] == '\0') {
        return;
    }

    // Parse command
    if (strcmp(cmd, "LIST_NODES") == 0) {
        handleListNodes();
    } else if (strncmp(cmd, "GET_QUEUE ", 10) == 0) {
        handleGetQueue(cmd + 10);
    } else if (strncmp(cmd, "SET_SCHEDULE ", 13) == 0) {
        handleSetSchedule(cmd + 13);
    } else if (strncmp(cmd, "REMOVE_SCHEDULE ", 16) == 0) {
        handleRemoveSchedule(cmd + 16);
    } else if (strncmp(cmd, "SET_WAKE_INTERVAL ", 18) == 0) {
        handleSetWakeInterval(cmd + 18);
    } else if (strncmp(cmd, "SET_CONFIG ", 11) == 0) {
        handleSetConfig(cmd + 11);
    } else if (strncmp(cmd, "SET_DATETIME ", 13) == 0) {
        handleSetDateTime(cmd + 13);
    } else if (strcmp(cmd, "GET_DATETIME") == 0) {
        handleGetDateTime();
    } else if (strncmp(cmd, "DATETIME ", 9) == 0) {
        handleDateTimeResponse(cmd + 9);
    } else if (strncmp(cmd, "BATCH_ACK ", 10) == 0) {
        handleBatchAckResponse(cmd + 10);
    } else {
        uart_puts(API_UART_ID, "ERROR Unknown command\n");
    }
}

void HubMode::handleListNodes()
{
    // Get node count
    uint32_t count = address_manager_->getRegisteredNodeCount();

    // Send header
    char response[128];
    snprintf(response, sizeof(response), "NODE_LIST %lu\n", count);
    logger.debug("Hub TX: %s", response);  // Debug: confirm we're sending
    uart_puts(API_UART_ID, response);

    // Iterate all registered nodes
    for (uint16_t addr = ADDRESS_MIN_NODE; addr <= ADDRESS_MAX_NODE; addr++) {
        const NodeInfo *node = address_manager_->getNodeInfo(addr);
        if (node) {
            // Determine node type
            const char *type = "UNKNOWN";
            if (node->capabilities & CAP_VALVE_CONTROL) {
                type = "IRRIGATION";
            } else if (node->capabilities & CAP_TEMPERATURE) {
                type = "SENSOR";
            }

            // Calculate last seen
            uint32_t now = to_ms_since_boot(get_absolute_time());
            uint32_t last_seen_sec = (now - node->last_seen_time) / 1000;

            // Send node info (include device_id for hardware identification)
            snprintf(response, sizeof(response), "NODE %u %llu %s %d %lu\n", addr, node->device_id,
                     type, node->is_active ? 1 : 0, last_seen_sec);
            uart_puts(API_UART_ID, response);
        }
    }
}

void HubMode::handleGetQueue(const char *args)
{
    // Parse node address
    uint16_t node_addr = atoi(args);

    if (node_addr < ADDRESS_MIN_NODE || node_addr > ADDRESS_MAX_NODE) {
        uart_puts(API_UART_ID, "ERROR Invalid node address\n");
        return;
    }

    // Get queue size
    size_t count = hub_router_->getPendingUpdateCount(node_addr);

    // Send header
    char response[128];
    snprintf(response, sizeof(response), "QUEUE %u %zu\n", node_addr, count);
    uart_puts(API_UART_ID, response);

    // Send individual update entries
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    for (size_t i = 0; i < count; i++) {
        PendingUpdate update;
        if (hub_router_->getPendingUpdate(node_addr, i, update)) {
            // Calculate age in seconds
            uint32_t age_sec = (current_time - update.queued_at_ms) / 1000;

            // Map UpdateType enum to string
            const char *type_str;
            switch (update.type) {
                case UpdateType::SET_SCHEDULE:
                    type_str = "SET_SCHEDULE";
                    break;
                case UpdateType::REMOVE_SCHEDULE:
                    type_str = "REMOVE_SCHEDULE";
                    break;
                case UpdateType::SET_DATETIME:
                    type_str = "SET_DATETIME";
                    break;
                case UpdateType::SET_WAKE_INTERVAL:
                    type_str = "SET_WAKE_INTERVAL";
                    break;
                case UpdateType::SET_CONFIG:
                    type_str = "SET_CONFIG";
                    break;
                default:
                    type_str = "UNKNOWN";
                    break;
            }

            // Format: UPDATE <seq> <type> <age_sec>
            snprintf(response, sizeof(response), "UPDATE %u %s %lu\n", update.sequence, type_str,
                     age_sec);
            uart_puts(API_UART_ID, response);
        }
    }
}

void HubMode::handleSetSchedule(const char *args)
{
    // Parse: <addr> <index> <hour>:<min> <duration> <days> <valve>
    uint16_t node_addr, duration;
    uint8_t index, hour, minute, days, valve;

    if (!parseScheduleArgs(args, node_addr, index, hour, minute, duration, days, valve)) {
        uart_puts(API_UART_ID, "ERROR Invalid SET_SCHEDULE syntax\n");
        return;
    }

    // Create schedule entry
    PMU::ScheduleEntry entry;
    entry.hour = hour;
    entry.minute = minute;
    entry.duration = duration;
    entry.daysMask = static_cast<PMU::DayOfWeek>(days);
    entry.valveId = valve;
    entry.enabled = true;

    // Queue update
    if (hub_router_->queueScheduleUpdate(node_addr, index, entry)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED SET_SCHEDULE %u %zu\n", node_addr, position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue update\n");
    }
}

void HubMode::handleRemoveSchedule(const char *args)
{
    // Parse: <addr> <index>
    uint16_t node_addr;
    uint8_t index;

    if (sscanf(args, "%hu %hhu", &node_addr, &index) != 2) {
        uart_puts(API_UART_ID, "ERROR Invalid REMOVE_SCHEDULE syntax\n");
        return;
    }

    // Queue removal
    if (hub_router_->queueRemoveSchedule(node_addr, index)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED REMOVE_SCHEDULE %u %zu\n", node_addr,
                 position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue removal\n");
    }
}

void HubMode::handleSetWakeInterval(const char *args)
{
    // Parse: <addr> <seconds>
    uint16_t node_addr, interval;

    if (sscanf(args, "%hu %hu", &node_addr, &interval) != 2) {
        uart_puts(API_UART_ID, "ERROR Invalid SET_WAKE_INTERVAL syntax\n");
        return;
    }

    // Queue update
    if (hub_router_->queueWakeIntervalUpdate(node_addr, interval)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED SET_WAKE_INTERVAL %u %zu\n", node_addr,
                 position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue update\n");
    }
}

void HubMode::handleSetConfig(const char *args)
{
    // Parse: <addr> <param_id> <value>
    uint16_t node_addr;
    int param_id;
    int32_t value;

    if (sscanf(args, "%hu %d %ld", &node_addr, &param_id, &value) != 3) {
        uart_puts(API_UART_ID, "ERROR Invalid SET_CONFIG syntax\n");
        return;
    }

    // Validate address range
    if (node_addr < ADDRESS_MIN_NODE || node_addr > ADDRESS_MAX_NODE) {
        uart_puts(API_UART_ID, "ERROR Invalid node address\n");
        return;
    }

    // Queue update
    if (hub_router_->queueConfigUpdate(node_addr, static_cast<uint8_t>(param_id), value)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED SET_CONFIG %u %zu\n", node_addr, position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue config update\n");
    }
}

void HubMode::handleSetDateTime(const char *args)
{
    // Parse: <addr> <year> <month> <day> <weekday> <hour> <minute> <second>
    uint16_t node_addr;
    int year, month, day, weekday, hour, minute, second;

    if (sscanf(args, "%hu %d %d %d %d %d %d %d", &node_addr, &year, &month, &day, &weekday, &hour,
               &minute, &second) != 8) {
        uart_puts(API_UART_ID, "ERROR Invalid SET_DATETIME syntax\n");
        return;
    }

    // Create datetime structure
    PMU::DateTime datetime(year, month, day, weekday, hour, minute, second);

    // Queue update
    if (hub_router_->queueDateTimeUpdate(node_addr, datetime)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED SET_DATETIME %u %zu\n", node_addr, position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue update\n");
    }
}

void HubMode::handleDateTimeResponse(const char *args)
{
    // Parse: DATETIME YYYY-MM-DD HH:MM:SS DOW (ISO 8601 + day of week)
    int year, month, day, hour, minute, second, weekday;
    if (sscanf(args, "%d-%d-%d %d:%d:%d %d", &year, &month, &day, &hour, &minute, &second,
               &weekday) != 7) {
        logger.error("Invalid DATETIME response format: %s", args);
        return;
    }

    // Set RTC
    datetime_t dt;
    dt.year = year;
    dt.month = month;
    dt.day = day;
    dt.dotw = weekday;
    dt.hour = hour;
    dt.min = minute;
    dt.sec = second;

    if (rtc_set_datetime(&dt)) {
        logger.info("RTC set to: %04d-%02d-%02d %02d:%02d:%02d (dow=%d)", year, month, day, hour,
                    minute, second, weekday);
    } else {
        logger.error("Failed to set RTC");
    }
}

bool HubMode::parseScheduleArgs(const char *args, uint16_t &node_addr, uint8_t &index,
                                uint8_t &hour, uint8_t &minute, uint16_t &duration, uint8_t &days,
                                uint8_t &valve)
{
    // Parse: <addr> <index> <hour>:<min> <duration> <days> <valve>
    // Example: 42 0 14:30 900 127 0

    int addr, idx, h, m, dur, d, v;
    if (sscanf(args, "%d %d %d:%d %d %d %d", &addr, &idx, &h, &m, &dur, &d, &v) != 7) {
        return false;
    }

    node_addr = addr;
    index = idx;
    hour = h;
    minute = m;
    duration = dur;
    days = d;
    valve = v;

    return true;
}

void HubMode::syncTimeFromRaspberryPi()
{
    // Send request for datetime
    uart_puts(API_UART_ID, "GET_DATETIME\n");
    last_datetime_sync_ms_ = to_ms_since_boot(get_absolute_time());
    logger.debug("Requesting datetime from RasPi");
}

void HubMode::handleGetDateTime()
{
    // Parse response: DATETIME YYYY MM DD DOW HH MM SS
    // This will be called when RasPi responds with datetime
    // The response is handled in handleSerialCommand via handleDateTimeResponse
}

void HubMode::handleHeartbeat(uint16_t source_addr, const HeartbeatPayload *payload)
{
    // Get current datetime from RTC
    datetime_t dt;
    if (!rtc_get_datetime(&dt)) {
        logger.warn("RTC not running, cannot send time to node 0x%04X", source_addr);
        return;
    }

    // Send heartbeat response with current time
    messenger_.sendHeartbeatResponse(source_addr, dt.year, dt.month, dt.day, dt.dotw, dt.hour,
                                     dt.min, dt.sec);

    logger.debug("Sent time to node 0x%04X: %04d-%02d-%02d %02d:%02d:%02d", source_addr, dt.year,
                 dt.month, dt.day, dt.hour, dt.min, dt.sec);
}

// ===== Sensor Data Forwarding =====

void HubMode::handleSensorData(uint16_t source_addr, const SensorPayload *payload)
{
    if (!payload) {
        return;
    }

    // Look up device_id from address manager
    uint64_t device_id = 0;
    const NodeInfo *node = address_manager_->getNodeInfo(source_addr);
    if (node) {
        device_id = node->device_id;
    }

    // Forward sensor data to Raspberry Pi via UART
    // Format: SENSOR_DATA <node_addr> <device_id> <sensor_type> <data_hex>
    char response[128];

    // For temperature/humidity, extract the 2-byte values
    if (payload->sensor_type == SENSOR_TEMPERATURE || payload->sensor_type == SENSOR_HUMIDITY) {
        if (payload->data_length >= 2) {
            int16_t value = static_cast<int16_t>(payload->data[0] | (payload->data[1] << 8));
            const char *type_str = (payload->sensor_type == SENSOR_TEMPERATURE) ? "TEMP" : "HUM";
            snprintf(response, sizeof(response), "SENSOR_DATA %u %llu %s %d\n", source_addr,
                     device_id, type_str, value);
            uart_puts(API_UART_ID, response);
            logger.debug("Forwarded sensor data: node=%u, device_id=%llu, type=%s, value=%d",
                         source_addr, device_id, type_str, value);
        }
    } else {
        // Generic sensor data (hex encoded)
        snprintf(response, sizeof(response), "SENSOR_DATA %u %llu %u ", source_addr, device_id,
                 payload->sensor_type);
        uart_puts(API_UART_ID, response);

        // Send data as hex
        for (uint8_t i = 0; i < payload->data_length && i < MAX_SENSOR_DATA_LENGTH; i++) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%02X", payload->data[i]);
            uart_puts(API_UART_ID, hex);
        }
        uart_puts(API_UART_ID, "\n");
    }
}

void HubMode::handleSensorDataBatch(uint16_t source_addr, const SensorDataBatchPayload *payload)
{
    if (!payload || payload->record_count == 0 || payload->record_count > MAX_BATCH_RECORDS) {
        logger.warn("Invalid batch payload from node 0x%04X", source_addr);
        return;
    }

    // Look up device_id from address manager
    uint64_t device_id = 0;
    const NodeInfo *node = address_manager_->getNodeInfo(source_addr);
    if (node) {
        device_id = node->device_id;
    }

    logger.debug("Received batch from node 0x%04X (device_id=%llu): %u records (start_idx=%lu)",
                 source_addr, device_id, payload->record_count, payload->start_index);

    // Send batch start marker to Raspberry Pi
    char response[128];
    snprintf(response, sizeof(response), "SENSOR_BATCH %u %llu %u\n", source_addr, device_id,
             payload->record_count);
    uart_puts(API_UART_ID, response);

    // Forward each record
    for (uint8_t i = 0; i < payload->record_count; i++) {
        const BatchSensorRecord &record = payload->records[i];

        // Format: SENSOR_RECORD <node_addr> <device_id> <timestamp> <temp> <humidity> <flags>
        snprintf(response, sizeof(response), "SENSOR_RECORD %u %llu %lu %d %u %u\n", source_addr,
                 device_id, record.timestamp, record.temperature, record.humidity, record.flags);
        uart_puts(API_UART_ID, response);
    }

    // Send batch complete marker
    snprintf(response, sizeof(response), "BATCH_COMPLETE %u %llu %u\n", source_addr, device_id,
             payload->record_count);
    uart_puts(API_UART_ID, response);

    logger.debug("Forwarded batch to RasPi: %u records", payload->record_count);

    // Note: BATCH_ACK will be sent when Raspberry Pi responds with confirmation
    // For now, send immediate ACK to sensor node to confirm receipt
    // The Raspberry Pi can send BATCH_ACK response which we'll forward back
}

void HubMode::sendBatchAck(uint16_t dest_addr, uint8_t seq_num, uint8_t status,
                           uint8_t records_received)
{
    // Create and send BATCH_ACK message
    uint8_t buffer[MESSAGE_MAX_SIZE];
    Message *message = reinterpret_cast<Message *>(buffer);

    message->header.magic = MESSAGE_MAGIC;
    message->header.type = MSG_TYPE_BATCH_ACK;
    message->header.flags = 0;  // No ACK required for BATCH_ACK
    message->header.src_addr = ADDRESS_HUB;
    message->header.dst_addr = dest_addr;
    message->header.seq_num = seq_num;

    BatchAckPayload *ack = reinterpret_cast<BatchAckPayload *>(message->payload);
    ack->ack_seq_num = seq_num;
    ack->status = status;
    ack->records_received = records_received;

    size_t message_size = MESSAGE_HEADER_SIZE + sizeof(BatchAckPayload);

    if (messenger_.send(buffer, message_size, BEST_EFFORT)) {
        logger.debug("Sent BATCH_ACK to 0x%04X: seq=%u, status=%u, records=%u", dest_addr, seq_num,
                     status, records_received);
    } else {
        logger.error("Failed to send BATCH_ACK to 0x%04X", dest_addr);
    }
}

void HubMode::handleBatchAckResponse(const char *args)
{
    // Parse: BATCH_ACK <node_addr> <count> <status>
    uint16_t node_addr;
    int count, status;

    if (sscanf(args, "%hu %d %d", &node_addr, &count, &status) != 3) {
        uart_puts(API_UART_ID, "ERROR Invalid BATCH_ACK syntax\n");
        return;
    }

    // Look up the stored seq_num for this node's pending batch
    uint8_t seq_num = 0;
    auto it = pending_batch_seq_num_.find(node_addr);
    if (it != pending_batch_seq_num_.end()) {
        seq_num = it->second;
        pending_batch_seq_num_.erase(it);  // Clear after use
    } else {
        logger.warn("No pending batch seq_num for node 0x%04X, using 0", node_addr);
    }

    logger.debug("RasPi confirmed batch: node=0x%04X, count=%d, status=%d, seq=%d", node_addr,
                 count, status, seq_num);

    // Forward BATCH_ACK to sensor node with correct seq_num
    sendBatchAck(node_addr, seq_num, static_cast<uint8_t>(status), static_cast<uint8_t>(count));

    // Respond to RasPi
    char response[64];
    snprintf(response, sizeof(response), "BATCH_ACK_SENT %u %d\n", node_addr, count);
    uart_puts(API_UART_ID, response);
}