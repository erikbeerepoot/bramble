#include "hub_mode.h"
#include <cstdio>
#include <cstring>
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"
#include "lora/sx1276.h"
#include "hal/flash.h"
#include "hal/pmu_protocol.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"

// UART configuration for Raspberry Pi communication
// Uses UART1 on Feather RP2040 RFM9x module header (D24/D25)
#define API_UART_ID uart1
#define API_UART_TX_PIN 24  // D24 - TX1 on RFM module header
#define API_UART_RX_PIN 25  // D25 - RX1 on RFM module header
#define API_UART_BAUD 115200

constexpr uint32_t STATS_INTERVAL_MS = 30000;      // 30 seconds
constexpr uint32_t MAINTENANCE_INTERVAL_MS = 300000; // 5 minutes
constexpr uint32_t DATETIME_QUERY_INTERVAL_MS = 3600000; // 1 hour

void HubMode::onStart() {
    printf("=== HUB MODE ACTIVE ===\n");
    printf("- Managing node registrations\n");
    printf("- Routing node-to-node messages\n");
    printf("- Blue LED indicates hub status\n");
    printf("- API UART on pins %d/%d @ %d baud\n", API_UART_TX_PIN, API_UART_RX_PIN, API_UART_BAUD);

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

    printf("API UART initialized successfully\n");
    uart_puts(API_UART_ID, "HUB_READY\n");  // Send ready signal to RasPi

    // Initialize RTC
    rtc_init();
    printf("RTC initialized\n");

    // Request initial time from RasPi
    syncTimeFromRaspberryPi();

    // Hub always uses blue breathing pattern
    led_pattern_ = std::make_unique<BreathingPattern>(led_, 0, 0, 255);
    
    // Add periodic stats reporting
    task_manager_.addTask(
        [this](uint32_t time) {
            uint32_t routed, queued, dropped;
            hub_router_->getRoutingStats(routed, queued, dropped);
            printf("Hub stats - Routed: %lu, Queued: %lu, Dropped: %lu\n", 
                   routed, queued, dropped);
            
            printf("Registered nodes: %u\n", address_manager_->getRegisteredNodeCount());
            
            // Print network statistics if available
            if (network_stats_) {
                // Update node counts
                network_stats_->updateNodeCounts(
                    address_manager_->getRegisteredNodeCount(),
                    address_manager_->getActiveNodeCount(),
                    address_manager_->getRegisteredNodeCount() - address_manager_->getActiveNodeCount()
                );
                network_stats_->printSummary();
            }
        },
        STATS_INTERVAL_MS,
        "Stats Reporting"
    );
    
    // Add periodic maintenance
    task_manager_.addTask(
        [this](uint32_t time) {
            printf("Performing hub maintenance...\n");
            hub_router_->clearOldRoutes(time);
            hub_router_->processQueuedMessages();
            
            // Check for inactive nodes and update network status
            uint32_t inactive_count = address_manager_->checkForInactiveNodes(time);
            if (inactive_count > 0) {
                printf("Marked %lu nodes as inactive\n", inactive_count);
            }
            
            // Deregister nodes that have been inactive for extended period
            uint32_t deregistered_count = address_manager_->deregisterInactiveNodes(time);
            if (deregistered_count > 0) {
                printf("Deregistered %lu nodes (inactive > %lu hours)\n", 
                       deregistered_count, 86400000UL / 3600000UL);
                // Persist the updated registry to flash
                Flash flash_hal;
                address_manager_->persist(flash_hal);
            }
        },
        MAINTENANCE_INTERVAL_MS,
        "Hub Maintenance"
    );

    // Add hourly time sync task
    task_manager_.addTask(
        [this](uint32_t current_time) {
            (void)current_time;  // Unused
            syncTimeFromRaspberryPi();
        },
        DATETIME_QUERY_INTERVAL_MS,
        "Time Sync"
    );
}

void HubMode::onLoop() {
    // Process serial input from Raspberry Pi
    processSerialInput();
}

void HubMode::processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) {
    printf("Hub received message (len=%d, RSSI=%d dBm)\n",
           rx_len, lora_.getRssi());

    // Handle heartbeat messages and send time response
    if (rx_len >= sizeof(MessageHeader)) {
        const MessageHeader* header = reinterpret_cast<const MessageHeader*>(rx_buffer);
        if (header->type == MSG_TYPE_HEARTBEAT) {
            const HeartbeatPayload* heartbeat =
                reinterpret_cast<const HeartbeatPayload*>(rx_buffer + sizeof(MessageHeader));
            handleHeartbeat(header->src_addr, heartbeat);
        }
    }

    // Call base class implementation which handles common processing
    ApplicationMode::processIncomingMessage(rx_buffer, rx_len, current_time);
}

// ===== Serial Command Processing =====

void HubMode::processSerialInput() {
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

void HubMode::handleSerialCommand(const char* cmd) {
    // Skip empty lines
    if (!cmd || cmd[0] == '\0') {
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
    } else if (strncmp(cmd, "SET_DATETIME ", 13) == 0) {
        handleSetDateTime(cmd + 13);
    } else if (strcmp(cmd, "GET_DATETIME") == 0) {
        handleGetDateTime();
    } else if (strncmp(cmd, "DATETIME ", 9) == 0) {
        handleDateTimeResponse(cmd + 9);
    } else {
        uart_puts(API_UART_ID, "ERROR Unknown command\n");
    }
}

void HubMode::handleListNodes() {
    // Get node count
    uint32_t count = address_manager_->getRegisteredNodeCount();

    // Send header
    char response[128];
    snprintf(response, sizeof(response), "NODE_LIST %lu\n", count);
    uart_puts(API_UART_ID, response);

    // Iterate all registered nodes
    for (uint16_t addr = ADDRESS_MIN_NODE; addr <= ADDRESS_MAX_NODE; addr++) {
        const NodeInfo* node = address_manager_->getNodeInfo(addr);
        if (node) {
            // Determine node type
            const char* type = "UNKNOWN";
            if (node->capabilities & CAP_VALVE_CONTROL) {
                type = "IRRIGATION";
            } else if (node->capabilities & CAP_TEMPERATURE) {
                type = "SENSOR";
            }

            // Calculate last seen
            uint32_t now = to_ms_since_boot(get_absolute_time());
            uint32_t last_seen_sec = (now - node->last_seen_time) / 1000;

            // Send node info
            snprintf(response, sizeof(response), "NODE %u %s %d %lu\n",
                   addr, type,
                   node->is_active ? 1 : 0,
                   last_seen_sec);
            uart_puts(API_UART_ID, response);
        }
    }
}

void HubMode::handleGetQueue(const char* args) {
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
            const char* type_str;
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
            snprintf(response, sizeof(response), "UPDATE %u %s %lu\n",
                     update.sequence, type_str, age_sec);
            uart_puts(API_UART_ID, response);
        }
    }
}

void HubMode::handleSetSchedule(const char* args) {
    // Parse: <addr> <index> <hour>:<min> <duration> <days> <valve>
    uint16_t node_addr, duration;
    uint8_t index, hour, minute, days, valve;

    if (!parseScheduleArgs(args, node_addr, index, hour, minute,
                          duration, days, valve)) {
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
        snprintf(response, sizeof(response), "QUEUED SET_SCHEDULE %u %zu\n",
                node_addr, position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue update\n");
    }
}

void HubMode::handleRemoveSchedule(const char* args) {
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
        snprintf(response, sizeof(response), "QUEUED REMOVE_SCHEDULE %u %zu\n",
                node_addr, position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue removal\n");
    }
}

void HubMode::handleSetWakeInterval(const char* args) {
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
        snprintf(response, sizeof(response), "QUEUED SET_WAKE_INTERVAL %u %zu\n",
                node_addr, position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue update\n");
    }
}

void HubMode::handleSetDateTime(const char* args) {
    // Parse: <addr> <year> <month> <day> <weekday> <hour> <minute> <second>
    uint16_t node_addr;
    int year, month, day, weekday, hour, minute, second;

    if (sscanf(args, "%hu %d %d %d %d %d %d %d",
               &node_addr, &year, &month, &day, &weekday, &hour, &minute, &second) != 8) {
        uart_puts(API_UART_ID, "ERROR Invalid SET_DATETIME syntax\n");
        return;
    }

    // Create datetime structure
    PMU::DateTime datetime(year, month, day, weekday, hour, minute, second);

    // Queue update
    if (hub_router_->queueDateTimeUpdate(node_addr, datetime)) {
        size_t position = hub_router_->getPendingUpdateCount(node_addr);
        char response[128];
        snprintf(response, sizeof(response), "QUEUED SET_DATETIME %u %zu\n",
                node_addr, position);
        uart_puts(API_UART_ID, response);
    } else {
        uart_puts(API_UART_ID, "ERROR Failed to queue update\n");
    }
}

void HubMode::handleDateTimeResponse(const char* args) {
    // Parse: DATETIME YYYY-MM-DD HH:MM:SS DOW (ISO 8601 + day of week)
    int year, month, day, hour, minute, second, weekday;
    if (sscanf(args, "%d-%d-%d %d:%d:%d %d",
               &year, &month, &day, &hour, &minute, &second, &weekday) != 7) {
        printf("ERROR: Invalid DATETIME response format: %s\n", args);
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
        printf("RTC set to: %04d-%02d-%02d %02d:%02d:%02d (dow=%d)\n",
               year, month, day, hour, minute, second, weekday);
    } else {
        printf("ERROR: Failed to set RTC\n");
    }
}

bool HubMode::parseScheduleArgs(const char* args, uint16_t& node_addr,
                               uint8_t& index, uint8_t& hour,
                               uint8_t& minute, uint16_t& duration,
                               uint8_t& days, uint8_t& valve) {
    // Parse: <addr> <index> <hour>:<min> <duration> <days> <valve>
    // Example: 42 0 14:30 900 127 0

    int addr, idx, h, m, dur, d, v;
    if (sscanf(args, "%d %d %d:%d %d %d %d",
               &addr, &idx, &h, &m, &dur, &d, &v) != 7) {
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

void HubMode::syncTimeFromRaspberryPi() {
    // Send request for datetime
    uart_puts(API_UART_ID, "GET_DATETIME\n");
    last_datetime_sync_ms_ = to_ms_since_boot(get_absolute_time());
    printf("Requesting datetime from RasPi\n");
}

void HubMode::handleGetDateTime() {
    // Parse response: DATETIME YYYY MM DD DOW HH MM SS
    // This will be called when RasPi responds with datetime
    // The response is handled in handleSerialCommand via handleDateTimeResponse
}

void HubMode::handleHeartbeat(uint16_t source_addr, const HeartbeatPayload* payload) {
    // Get current datetime from RTC
    datetime_t dt;
    if (!rtc_get_datetime(&dt)) {
        printf("Warning: RTC not running, cannot send time to node 0x%04X\n", source_addr);
        return;
    }

    // Send heartbeat response with current time
    messenger_.sendHeartbeatResponse(source_addr, dt.year, dt.month, dt.day,
                                     dt.dotw, dt.hour, dt.min, dt.sec);

    printf("Sent time to node 0x%04X: %04d-%02d-%02d %02d:%02d:%02d\n",
           source_addr, dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
}