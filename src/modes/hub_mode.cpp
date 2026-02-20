#include "hub_mode.h"

#include <cstring>

#include "pico/stdlib.h"

#include "hardware/uart.h"

#include "hal/flash.h"
#include "hal/logger.h"
#include "util/format.h"
#include "hal/pmu_protocol.h"
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"
#include "lora/sx1276.h"
#include "storage/sensor_data_record.h"

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
constexpr uint32_t DATETIME_RETRY_INTERVAL_MS = 5000;     // 5 seconds retry if RTC not running

// Safe uint64_t formatting — see src/util/format.h for details
using bramble::format::uint64_to_str;
using bramble::format::uint64_to_hex;

// ===== Buffered UART Output =====
// All UART writes go into a software buffer, which is flushed at the top of
// onLoop() — before any LoRa processing.  This guarantees the hardware TX FIFO
// is empty when the radio fires, preventing 20 dBm LoRa TX from corrupting
// UART bytes still on the wire.

void HubMode::uartSend(const char *str)
{
    if (!str) return;
    size_t len = strlen(str);
    size_t available = UART_TX_BUFFER_SIZE - uart_tx_pos_;
    if (len > available) {
        // Buffer full — flush now as a safety valve
        flushUartBuffer();
        available = UART_TX_BUFFER_SIZE - uart_tx_pos_;
    }
    if (len > available) {
        len = available;  // Truncate if still too large after flush
    }
    memcpy(uart_tx_buffer_ + uart_tx_pos_, str, len);
    uart_tx_pos_ += len;
}

void HubMode::flushUartBuffer()
{
    if (uart_tx_pos_ == 0) return;
    uart_write_blocking(API_UART_ID, reinterpret_cast<const uint8_t *>(uart_tx_buffer_), uart_tx_pos_);
    uart_tx_wait_blocking(API_UART_ID);
    uart_tx_pos_ = 0;
}

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
    uart_tx_pos_ = 0;
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
    // Flush any buffered UART output from the previous loop iteration.
    // This runs before LoRa interrupt checks, so the TX FIFO is guaranteed
    // empty when the radio starts transmitting.
    flushUartBuffer();

    // Process serial input from Raspberry Pi
    processSerialInput();

    // Flush again so command responses (e.g. LIST_NODES) are sent immediately,
    // before any LoRa processing that could delay or interfere with UART TX.
    flushUartBuffer();

    // Retry GET_DATETIME until RTC is running (belt-and-suspenders approach)
    // This handles cases where RasPi wasn't ready when hub first booted
    if (!rtc_running()) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_datetime_sync_ms_ >= DATETIME_RETRY_INTERVAL_MS) {
            logger.debug("RTC not running - retrying datetime sync");
            syncTimeFromRaspberryPi();
        }
    }
}

void HubMode::processIncomingMessage(uint8_t *rx_buffer, int rx_len, uint32_t current_time)
{
    logger.debug("Hub received message (len=%d, RSSI=%d dBm)", rx_len, lora_.getRssi());

    // Handle special message types
    if (rx_len >= static_cast<int>(sizeof(MessageHeader))) {
        const MessageHeader *header = reinterpret_cast<const MessageHeader *>(rx_buffer);

        if (header->type == MSG_TYPE_HEARTBEAT) {
            const HeartbeatPayload *heartbeat =
                reinterpret_cast<const HeartbeatPayload *>(rx_buffer + sizeof(MessageHeader));
            int16_t rssi = lora_.getRssi();  // Capture RSSI of this packet
            handleHeartbeat(header->src_addr, heartbeat, rssi);
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

    logger.info("Serial RX: %s", cmd);

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
    } else if (strncmp(cmd, "SET_DATETIME ", 13) == 0) {
        handleSetDateTime(cmd + 13);
    } else if (strcmp(cmd, "GET_DATETIME") == 0) {
        handleGetDateTime();
    } else if (strncmp(cmd, "DATETIME ", 9) == 0) {
        handleDateTimeResponse(cmd + 9);
    } else if (strncmp(cmd, "BATCH_ACK ", 10) == 0) {
        handleBatchAckResponse(cmd + 10);
    } else if (strncmp(cmd, "DELETE_NODE ", 12) == 0) {
        handleDeleteNode(cmd + 12);
    } else if (strncmp(cmd, "REBOOT_NODE ", 12) == 0) {
        handleRebootNode(cmd + 12);
    } else {
        uartSend("ERROR Unknown command\n");
    }
}

void HubMode::handleListNodes()
{
    // Get registered addresses directly (avoids scanning 65k addresses)
    auto addresses = address_manager_->getRegisteredAddresses();

    // Send header with actual count
    char response[128];
    snprintf(response, sizeof(response), "NODE_LIST %u\n", (unsigned)addresses.size());
    logger.info("LIST_NODES: responding with %u nodes", (unsigned)addresses.size());
    uartSend(response);

    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (uint16_t addr : addresses) {
        const NodeInfo *node = address_manager_->getNodeInfo(addr);
        if (!node) continue;

        // Determine node type
        const char *type = "UNKNOWN";
        if (node->capabilities & CAP_VALVE_CONTROL) {
            type = "IRRIGATION";
        } else if (node->capabilities & CAP_TEMPERATURE) {
            type = "SENSOR";
        }

        uint32_t last_seen_sec = (now - node->last_seen_time) / 1000;

        // Send node info (include device_id and firmware version for identification)
        char device_id_str[21];
        uint64_to_str(node->device_id, device_id_str, sizeof(device_id_str));
        int len = snprintf(response, sizeof(response), "NODE %u %s %s %d %lu %lu\n", addr,
                           device_id_str, type, node->is_active ? 1 : 0, last_seen_sec,
                           (unsigned long)node->firmware_version);
        if (len >= (int)sizeof(response)) {
            response[sizeof(response) - 2] = '\n';
            response[sizeof(response) - 1] = '\0';
        }
        uartSend(response);
    }
}

void HubMode::handleGetQueue(const char *args)
{
    // Parse node address
    uint16_t node_addr = atoi(args);

    if (node_addr < ADDRESS_MIN_NODE || node_addr > ADDRESS_MAX_NODE) {
        uartSend("ERROR Invalid node address\n");
        return;
    }

    // Get queue size
    size_t count = hub_router_->getPendingUpdateCount(node_addr);

    // Send header
    char response[128];
    snprintf(response, sizeof(response), "QUEUE %u %zu\n", node_addr, count);
    uartSend(response);

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
                default:
                    type_str = "UNKNOWN";
                    break;
            }

            // Format: UPDATE <seq> <type> <age_sec>
            snprintf(response, sizeof(response), "UPDATE %u %s %lu\n", update.sequence, type_str,
                     age_sec);
            uartSend(response);
        }
    }
}

void HubMode::handleSetSchedule(const char *args)
{
    // Parse: <addr> <index> <hour>:<min> <duration> <days> <valve>
    uint16_t node_addr, duration;
    uint8_t index, hour, minute, days, valve;

    if (!parseScheduleArgs(args, node_addr, index, hour, minute, duration, days, valve)) {
        uartSend("ERROR Invalid SET_SCHEDULE syntax\n");
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
        uartSend(response);
    } else {
        uartSend("ERROR Failed to queue update\n");
    }
}

void HubMode::handleRemoveSchedule(const char *args)
{
    // Parse: <addr> <index>
    uint16_t node_addr;
    uint8_t index;

    if (sscanf(args, "%hu %hhu", &node_addr, &index) != 2) {
        uartSend("ERROR Invalid REMOVE_SCHEDULE syntax\n");
        return;
    }

    // Queue removal
    if (hub_router_->queueRemoveSchedule(node_addr, index)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED REMOVE_SCHEDULE %u %zu\n", node_addr,
                 position);
        uartSend(response);
    } else {
        uartSend("ERROR Failed to queue removal\n");
    }
}

void HubMode::handleSetWakeInterval(const char *args)
{
    // Parse: <addr> <seconds>
    uint16_t node_addr, interval;

    if (sscanf(args, "%hu %hu", &node_addr, &interval) != 2) {
        uartSend("ERROR Invalid SET_WAKE_INTERVAL syntax\n");
        return;
    }

    // Queue update
    if (hub_router_->queueWakeIntervalUpdate(node_addr, interval)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED SET_WAKE_INTERVAL %u %zu\n", node_addr,
                 position);
        uartSend(response);
    } else {
        uartSend("ERROR Failed to queue update\n");
    }
}

void HubMode::handleSetDateTime(const char *args)
{
    // Parse: <addr> <year> <month> <day> <weekday> <hour> <minute> <second>
    uint16_t node_addr;
    int year, month, day, weekday, hour, minute, second;

    if (sscanf(args, "%hu %d %d %d %d %d %d %d", &node_addr, &year, &month, &day, &weekday, &hour,
               &minute, &second) != 8) {
        uartSend("ERROR Invalid SET_DATETIME syntax\n");
        return;
    }

    // Create datetime structure
    PMU::DateTime datetime(year, month, day, weekday, hour, minute, second);

    // Queue update
    if (hub_router_->queueDateTimeUpdate(node_addr, datetime)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED SET_DATETIME %u %zu\n", node_addr, position);
        uartSend(response);
    } else {
        uartSend("ERROR Failed to queue update\n");
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
        Logger::syncSubsecondCounter();
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
    uartSend("GET_DATETIME\n");
    last_datetime_sync_ms_ = to_ms_since_boot(get_absolute_time());
    logger.debug("Requesting datetime from RasPi");
}

void HubMode::handleGetDateTime()
{
    // Parse response: DATETIME YYYY MM DD DOW HH MM SS
    // This will be called when RasPi responds with datetime
    // The response is handled in handleSerialCommand via handleDateTimeResponse
}

void HubMode::handleDeleteNode(const char *args)
{
    // Parse node address
    uint16_t node_addr = atoi(args);

    if (node_addr < ADDRESS_MIN_NODE || node_addr > ADDRESS_MAX_NODE) {
        uartSend("ERROR Invalid node address\n");
        return;
    }

    // Check if node exists
    const NodeInfo *node = address_manager_->getNodeInfo(node_addr);
    if (!node) {
        uartSend("ERROR Node not found\n");
        return;
    }

    // Unregister the node
    if (!address_manager_->unregisterNode(node_addr)) {
        uartSend("ERROR Failed to unregister node\n");
        return;
    }

    // Clear any pending updates for this node
    hub_router_->clearPendingUpdates(node_addr);

    // Persist the registry to flash
    Flash flash_hal;
    address_manager_->persist(flash_hal);

    // Send success response
    char response[64];
    snprintf(response, sizeof(response), "DELETED_NODE %u\n", node_addr);
    uartSend(response);

    logger.info("Deleted node 0x%04X from registry", node_addr);
}

void HubMode::handleRebootNode(const char *args)
{
    uint16_t node_addr;
    if (sscanf(args, "%hu", &node_addr) != 1) {
        uartSend("ERROR Invalid REBOOT_NODE syntax\n");
        return;
    }

    if (node_addr < ADDRESS_MIN_NODE || node_addr > ADDRESS_MAX_NODE) {
        uartSend("ERROR Invalid node address\n");
        return;
    }

    pending_reboots_.insert(node_addr);

    char response[64];
    snprintf(response, sizeof(response), "QUEUED REBOOT_NODE %u\n", node_addr);
    uartSend(response);

    logger.info("Queued reboot for node 0x%04X", node_addr);
}

void HubMode::handleHeartbeat(uint16_t source_addr, const HeartbeatPayload *payload, int16_t rssi)
{
    // Get current datetime from RTC
    datetime_t dt;
    if (!rtc_get_datetime(&dt)) {
        logger.warn("RTC not running, cannot send time to node 0x%04X", source_addr);
        return;
    }

    // Compute pending update flags for this node
    uint8_t pending_flags = hub_router_->getPendingUpdateFlags(source_addr);

    // Set REREGISTER flag if the node is unknown or device_id doesn't match
    const NodeInfo *node = address_manager_->getNodeInfo(source_addr);
    if (!node) {
        pending_flags |= PENDING_FLAG_REREGISTER;
    } else if (node->device_id != payload->device_id) {
        char reg_id[17], rcv_id[17];
        uint64_to_hex(node->device_id, reg_id, sizeof(reg_id));
        uint64_to_hex(payload->device_id, rcv_id, sizeof(rcv_id));
        logger.warn("device_id mismatch on addr 0x%04X: registered=0x%s, received=0x%s",
                    source_addr, reg_id, rcv_id);
        pending_flags |= PENDING_FLAG_REREGISTER;
        // Deregister stale mapping so the mismatched sensor gets a fresh address
        address_manager_->unregisterNode(source_addr);
        Flash flash_hal;
        address_manager_->persist(flash_hal);
    }

    // Set REBOOT flag if this node has a pending reboot request
    if (pending_reboots_.count(source_addr)) {
        pending_flags |= PENDING_FLAG_REBOOT;
        pending_reboots_.erase(source_addr);
        logger.info("Setting PENDING_FLAG_REBOOT for node 0x%04X", source_addr);
    }

    // Send heartbeat response with current time and pending flags
    messenger_.sendHeartbeatResponse(source_addr, dt.year, dt.month, dt.day, dt.dotw, dt.hour,
                                     dt.min, dt.sec, pending_flags);

    if (pending_flags != PENDING_FLAG_NONE) {
        logger.debug("Sent time + pending flags 0x%02X to node 0x%04X", pending_flags, source_addr);
    } else {
        logger.debug("Sent time to node 0x%04X: %04d-%02d-%02d %02d:%02d:%02d", source_addr,
                     dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
    }

    // Forward heartbeat status to Raspberry Pi via UART
    // Use device_id from the payload (authoritative) rather than address lookup
    // This ensures correct reporting even during address mismatch scenarios
    uint64_t device_id = payload->device_id;

    // rssi parameter is measured by hub when receiving this heartbeat (already in dBm)

    // Format: HEARTBEAT <node_addr> <device_id> <battery> <error_flags> <signal> <uptime>
    // <pending_records>
    char device_id_str[21];
    uint64_to_str(device_id, device_id_str, sizeof(device_id_str));
    char response[128];
    int len = snprintf(response, sizeof(response), "HEARTBEAT %u %s %u %u %d %lu %u\n", source_addr,
                       device_id_str, payload->battery_level, payload->error_flags, rssi,
                       payload->uptime_seconds, payload->pending_records);
    if (len >= (int)sizeof(response)) {
        response[sizeof(response) - 2] = '\n';
        response[sizeof(response) - 1] = '\0';
    }
    uartSend(response);

    logger.debug("Forwarded heartbeat: node=%u, battery=%u, errors=0x%02X, rssi=%d, pending=%u",
                 source_addr, payload->battery_level, payload->error_flags, rssi,
                 payload->pending_records);
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

    char device_id_str[21];
    uint64_to_str(device_id, device_id_str, sizeof(device_id_str));

    // For temperature/humidity, extract the 2-byte values
    if (payload->sensor_type == SENSOR_TEMPERATURE || payload->sensor_type == SENSOR_HUMIDITY) {
        if (payload->data_length >= 2) {
            int16_t value = static_cast<int16_t>(payload->data[0] | (payload->data[1] << 8));
            const char *type_str = (payload->sensor_type == SENSOR_TEMPERATURE) ? "TEMP" : "HUM";
            int len = snprintf(response, sizeof(response), "SENSOR_DATA %u %s %s %d\n", source_addr,
                               device_id_str, type_str, value);
            if (len >= (int)sizeof(response)) {
                response[sizeof(response) - 2] = '\n';
                response[sizeof(response) - 1] = '\0';
            }
            uartSend(response);
            logger.debug("Forwarded sensor data: node=%u, device_id=%s, type=%s, value=%d",
                         source_addr, device_id_str, type_str, value);
        }
    } else {
        // Generic sensor data (hex encoded)
        snprintf(response, sizeof(response), "SENSOR_DATA %u %s %u ", source_addr, device_id_str,
                 payload->sensor_type);
        uartSend(response);

        // Send data as hex
        for (uint8_t i = 0; i < payload->data_length && i < MAX_SENSOR_DATA_LENGTH; i++) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%02X", payload->data[i]);
            uartSend(hex);
        }
        uartSend("\n");
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

    char device_id_str[21];
    uint64_to_str(device_id, device_id_str, sizeof(device_id_str));

    logger.debug("Received batch from node 0x%04X (device_id=%s): %u records (start_idx=%lu)",
                 source_addr, device_id_str, payload->record_count, payload->start_index);

    // Send batch start marker to Raspberry Pi
    char response[128];
    int len = snprintf(response, sizeof(response), "SENSOR_BATCH %u %s %u\n", source_addr,
                       device_id_str, payload->record_count);
    if (len >= (int)sizeof(response)) {
        response[sizeof(response) - 2] = '\n';
        response[sizeof(response) - 1] = '\0';
    }
    uartSend(response);

    // Forward each record
    for (uint8_t i = 0; i < payload->record_count; i++) {
        const BatchSensorRecord &record = payload->records[i];

        // Strip storage-internal VALID flag before forwarding to API
        uint8_t error_flags = record.flags & ~RECORD_FLAG_VALID;

        // Format: SENSOR_RECORD <node_addr> <device_id> <timestamp> <temp> <humidity> <flags>
        len = snprintf(response, sizeof(response), "SENSOR_RECORD %u %s %lu %d %u %u\n",
                       source_addr, device_id_str, record.timestamp, record.temperature,
                       record.humidity, error_flags);
        if (len >= (int)sizeof(response)) {
            response[sizeof(response) - 2] = '\n';
            response[sizeof(response) - 1] = '\0';
        }
        uartSend(response);
    }

    // Send batch complete marker
    len = snprintf(response, sizeof(response), "BATCH_COMPLETE %u %s %u\n", source_addr,
                   device_id_str, payload->record_count);
    if (len >= (int)sizeof(response)) {
        response[sizeof(response) - 2] = '\n';
        response[sizeof(response) - 1] = '\0';
    }
    uartSend(response);

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
        uartSend("ERROR Invalid BATCH_ACK syntax\n");
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
    uartSend(response);
}