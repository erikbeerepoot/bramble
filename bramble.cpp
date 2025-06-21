#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "lora/sx1276.h"
#include "lora/message.h"
#include "lora/reliable_messenger.h"
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"
#include "hal/neopixel.h"
#include "hal/flash.h"
#include "hal/logger.h"
#include "config/node_config.h"
#include "pico/unique_id.h"

// Include sleep headers if pico-extras is available
#ifdef PICO_EXTRAS_PATH
#include "pico/sleep.h"
#include "hardware/rosc.h"
#define PICO_EXTRAS_AVAILABLE
#endif

// Global logger for main application
static Logger main_logger("MAIN");

// SPI Defines - Feather RP2040 RFM95 LoRa board
// Use SPI1: GPIO8(RX), GPIO14(SCK), GPIO15(TX)
#define SPI_PORT spi1
#define PIN_MISO 8   // SPI1 RX
#define PIN_CS   16  // GPIO as chip select (not SPI function)
#define PIN_SCK  14  // SPI1 SCK
#define PIN_MOSI 15  // SPI1 TX
#define PIN_RST  17
#define PIN_DIO0 21
#define PIN_NEOPIXEL 4

// Application configuration
// IS_HUB is now defined by CMake (1 for hub, 0 for node)
#ifndef IS_HUB
#error "IS_HUB must be defined by build system"
#endif

// DEMO_MODE is now defined by CMake (1 for demo, 0 for production)
#ifndef DEMO_MODE
#error "DEMO_MODE must be defined by build system"
#endif

#define HUB_ADDRESS             ADDRESS_HUB      // Hub/gateway address
#define SENSOR_INTERVAL_MS      30000       // Send sensor data every 30 seconds
#define HEARTBEAT_INTERVAL_MS   60000       // Send heartbeat every minute
#define MAIN_LOOP_DELAY_MS      100         // Main loop processing delay

// Automatically determine node address based on role
#define NODE_ADDRESS            (IS_HUB ? ADDRESS_HUB : ADDRESS_UNREGISTERED)

// Forward declarations
void runDemoMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led, 
                 AddressManager* address_manager = nullptr, HubRouter* hub_router = nullptr,
                 NetworkStats* network_stats = nullptr);
void runProductionMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                       AddressManager* address_manager = nullptr, HubRouter* hub_router = nullptr,
                       NetworkStats* network_stats = nullptr);
void runHubMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                AddressManager& address_manager, HubRouter& hub_router,
                NetworkStats* network_stats);
bool initializeHardware(SX1276& lora, NeoPixel& led);
uint64_t getDeviceId();
bool attemptRegistration(ReliableMessenger& messenger, SX1276& lora, 
                        NodeConfigManager& config_manager, uint64_t device_id);
void processIncomingMessage(uint8_t* rx_buffer, int rx_len, ReliableMessenger& messenger,
                          AddressManager* address_manager, HubRouter* hub_router, 
                          uint32_t current_time, NetworkStats* network_stats = nullptr,
                          SX1276* lora = nullptr);
void sleepUntilInterrupt();

int main()
{
    stdio_init_all();
    sleep_ms(2000); // Give USB time to enumerate
    
    // Configure logging based on mode
    if (DEMO_MODE) {
        Logger::setLogLevel(LOG_DEBUG);  // Verbose logging for demo
        Logger::checkForUsbConnection(false);  // Always log in demo mode
        main_logger.info("Starting in DEMO mode with DEBUG logging");
    } else {
        Logger::setLogLevel(LOG_WARN);   // Production: warnings and errors only
        Logger::checkForUsbConnection(true);  // Only log when USB connected to save power
        main_logger.info("Starting in PRODUCTION mode with WARN logging");
    }
    
    main_logger.info("=== Bramble Starting ===");

    // Initialize hardware
    NeoPixel led(PIN_NEOPIXEL, 1);
    SX1276 lora(SPI_PORT, PIN_CS, PIN_RST, PIN_DIO0);
    
    if (!initializeHardware(lora, led)) {
        printf("Hardware initialization failed!\n");
        while(true) {
            sleep_ms(1000);
        }
    }
    
    // Enable interrupt mode for efficient operation
    if (lora.enableInterruptMode()) {
        main_logger.info("Interrupt mode enabled - efficient power usage");
    } else {
        main_logger.warn("Failed to enable interrupt mode - using polling");
    }
    
    // Start in receive mode
    lora.startReceive();
    
    // Initialize network statistics (only for hub)
    NetworkStats* network_stats = nullptr;
    if (IS_HUB) {
        network_stats = new NetworkStats();
    }
    
    // Initialize reliable messenger with optional network stats
    ReliableMessenger messenger(&lora, NODE_ADDRESS, network_stats);
    
    // Check if this device should run as hub
    if (IS_HUB) {
        printf("Starting as HUB - managing network and routing\n");
        
        // Initialize hub components
        Flash flash_hal;
        AddressManager address_manager;
        HubRouter hub_router(address_manager, messenger);
        
        if (DEMO_MODE) {
            printf("Starting HUB DEMO mode\n");
            runDemoMode(messenger, lora, led, &address_manager, &hub_router, network_stats);
        } else {
            printf("Starting HUB PRODUCTION mode\n");
            runHubMode(messenger, lora, led, address_manager, hub_router, network_stats);
        }
    } else {
        // Run as regular node
        main_logger.info("Starting as NODE - will auto-register with hub");
        
        // Initialize node configuration for potential address assignment
        Flash flash_hal;
        NodeConfigManager config_manager(flash_hal);
        
        // Get unique device ID
        uint64_t device_id = getDeviceId();
        
        // Check if we have a saved address from previous registration
        NodeConfiguration saved_config;
        if (config_manager.loadConfiguration(saved_config) && 
            saved_config.assigned_address != ADDRESS_UNREGISTERED) {
            
            main_logger.info("Found saved address 0x%04X - using it while re-registering", saved_config.assigned_address);
            // Update messenger to use saved address immediately
            messenger.updateNodeAddress(saved_config.assigned_address);
        } else {
            main_logger.info("No saved address - using unregistered address");
        }
        
        // Always attempt registration with hub to get authoritative address
        if (attemptRegistration(messenger, lora, config_manager, device_id)) {
            main_logger.info("Registration successful!");
        } else {
            main_logger.warn("Registration failed - continuing with current address");
        }
        
        if (DEMO_MODE) {
            main_logger.info("Starting NODE DEMO mode - sending test messages");
            runDemoMode(messenger, lora, led, nullptr, nullptr, network_stats);
        } else {
            main_logger.info("Starting NODE PRODUCTION mode - real sensor monitoring");
            runProductionMode(messenger, lora, led, nullptr, nullptr, network_stats);
        }
    }
    
    return 0; // Should never reach here
}

bool initializeHardware(SX1276& lora, NeoPixel& led) {
    // SPI initialization - 1MHz for reliable communication
    spi_init(SPI_PORT, 1000*1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    
    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    
    printf("System Clock: %d Hz, USB Clock: %d Hz\n", 
           clock_get_hz(clk_sys), clock_get_hz(clk_usb));
    
    // Initialize SX1276 LoRa module
    printf("Initializing SX1276 LoRa module...\n");
    if (!lora.begin()) {
        printf("Failed to initialize SX1276!\n");
        return false;
    }
    
    // Set higher transmit power for better signal strength
    lora.setTxPower(20);  // 20 dBm = 100mW (maximum)
    printf("SX1276 initialized successfully! (TX Power: 20 dBm)\n");
    
    return true;
}

void runHubMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                AddressManager& address_manager, HubRouter& hub_router,
                NetworkStats* network_stats) {
    uint32_t last_stats_time = 0;
    uint32_t last_maintenance_time = 0;
    
    printf("=== HUB MODE ACTIVE ===\n");
    printf("- Managing node registrations\n");
    printf("- Routing node-to-node messages\n");
    printf("- Blue LED indicates hub status\n");
    
    while (true) {
        // Hub LED: Blue breathing pattern
        static uint8_t breath_counter = 0;
        uint8_t brightness = (breath_counter < 64) ? breath_counter * 2 : (128 - breath_counter) * 2;
        led.setPixel(0, 0, 0, brightness);  // Blue breathing
        led.show();
        breath_counter = (breath_counter + 1) % 128;
        
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Print routing stats every 30 seconds
        if (current_time - last_stats_time >= 30000) {
            uint32_t routed, queued, dropped;
            hub_router.getRoutingStats(routed, queued, dropped);
            printf("Hub stats - Routed: %lu, Queued: %lu, Dropped: %lu\n", 
                   routed, queued, dropped);
            
            printf("Registered nodes: %u\n", address_manager.getRegisteredNodeCount());
            
            // Print network statistics if available
            if (network_stats) {
                // Update node counts
                network_stats->updateNodeCounts(
                    address_manager.getRegisteredNodeCount(),
                    address_manager.getActiveNodeCount(),
                    address_manager.getRegisteredNodeCount() - address_manager.getActiveNodeCount()
                );
                network_stats->printSummary();
            }
            
            last_stats_time = current_time;
        }
        
        // Perform maintenance every 5 minutes
        if (current_time - last_maintenance_time >= 300000) {
            printf("Performing hub maintenance...\n");
            hub_router.clearOldRoutes(current_time);
            hub_router.processQueuedMessages();
            
            // Check for inactive nodes and update network status
            uint32_t inactive_count = address_manager.checkForInactiveNodes(current_time);
            if (inactive_count > 0) {
                printf("Marked %lu nodes as inactive\n", inactive_count);
            }
            
            // Deregister nodes that have been inactive for extended period
            uint32_t deregistered_count = address_manager.deregisterInactiveNodes(current_time);
            if (deregistered_count > 0) {
                printf("Deregistered %lu nodes (inactive > %lu hours)\n", 
                       deregistered_count, 86400000UL / 3600000UL); // Convert ms to hours
                // Persist the updated registry to flash
                Flash flash_hal;
                address_manager.persist(flash_hal);
            }
            
            last_maintenance_time = current_time;
        }
        
        // Check for interrupts first (more efficient than polling)
        if (lora.isInterruptPending()) {
            lora.handleInterrupt();
        }
        
        // Check for incoming messages
        if (lora.isMessageReady()) {
            uint8_t rx_buffer[MESSAGE_MAX_SIZE];
            int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
            
            if (rx_len > 0) {
                printf("Hub received message (len=%d, RSSI=%d dBm)\n", 
                       rx_len, lora.getRssi());
                
                // Use common message processing function
                processIncomingMessage(rx_buffer, rx_len, messenger, &address_manager, &hub_router, current_time, network_stats, &lora);
            } else if (rx_len < 0) {
                lora.startReceive();
            }
        }
        
        // Update retry timers and process queued messages
        messenger.update();
        hub_router.processQueuedMessages();
        
        // Sleep efficiently between iterations
        if (!lora.isInterruptPending()) {
            sleepUntilInterrupt();
        }
        // If interrupt is pending, loop immediately to process it
    }
}

void runDemoMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led, 
                 AddressManager* address_manager, HubRouter* hub_router,
                 NetworkStats* network_stats) {
    uint32_t last_demo_time = 0;
    uint32_t last_heartbeat_time = 0;
    
    printf("=== DEMO MODE ACTIVE ===\n");
    printf("- Test messages every 15 seconds\n");
    printf("- Verbose debug output\n");
    
    while (true) {
        // Demo LED: Use role-specific colors like production mode
        if (hub_router) {
            // Hub: Blue breathing pattern (same as production hub mode)
            static uint8_t breath_counter = 0;
            uint8_t brightness = (breath_counter < 64) ? breath_counter * 2 : (128 - breath_counter) * 2;
            led.setPixel(0, 0, 0, brightness);  // Blue breathing            
            led.show();
            breath_counter = (breath_counter + 1) % 128;
        } else {
            // Node: Green heartbeat (same as production node mode)
            static uint8_t led_counter = 0;
            uint8_t brightness = (led_counter < 10) ? led_counter * 5 : (20 - led_counter) * 5;
            led.setPixel(0, 0, brightness, 0);  // Green heartbeat
            led.show();
            led_counter = (led_counter + 1) % 20;
        }
        
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Send test messages every 15 seconds
        if (current_time - last_demo_time >= 15000) {
            printf("--- DEMO: Sending test messages ---\n");
            
            // Test temperature reading (best effort)
            uint8_t temp_data[] = {0x12, 0x34};
            messenger.sendSensorData(HUB_ADDRESS, SENSOR_TEMPERATURE, 
                                   temp_data, sizeof(temp_data), BEST_EFFORT);
            
            // Test moisture reading (reliable - critical for irrigation decisions)
            uint8_t moisture_data[] = {0x45, 0x67};
            messenger.sendSensorData(HUB_ADDRESS, SENSOR_SOIL_MOISTURE, 
                                   moisture_data, sizeof(moisture_data), RELIABLE);
            
            last_demo_time = current_time;
        }
        
        // Send heartbeat every minute
        if (current_time - last_heartbeat_time >= HEARTBEAT_INTERVAL_MS) {
            printf("--- DEMO: Sending heartbeat ---\n");
            
            // Calculate node status
            uint32_t uptime = current_time / 1000;  // Convert to seconds
            uint8_t battery_level = 255;  // External power for demo
            uint8_t signal_strength = 70;  // Simulated signal strength
            uint8_t active_sensors = CAP_TEMPERATURE | CAP_SOIL_MOISTURE;  // Active sensors
            uint8_t error_flags = 0;  // No errors
            
            messenger.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, 
                                  signal_strength, active_sensors, error_flags);
            
            last_heartbeat_time = current_time;
        }
        
        // Check for interrupts first (more efficient than polling)
        if (lora.isInterruptPending()) {
            lora.handleInterrupt();
        }
        
        // Check for incoming messages
        if (lora.isMessageReady()) {
            uint8_t rx_buffer[MESSAGE_MAX_SIZE];
            int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
            
            if (rx_len > 0) {
                printf("Received message (len=%d, RSSI=%d dBm, SNR=%.1f dB)\n", 
                       rx_len, lora.getRssi(), lora.getSnr());
                
                // Use common message processing function
                processIncomingMessage(rx_buffer, rx_len, messenger, address_manager, hub_router, current_time, network_stats, &lora);
            } else if (rx_len < 0) {
                printf("Receive error (CRC or buffer issue)\n");
                lora.startReceive();
            }
        }
        
        // Update retry timers for reliable message delivery
        messenger.update();
        
        // Update hub router if in hub mode
        if (hub_router) {
            hub_router->processQueuedMessages();
        }
        
        // Sleep efficiently between iterations
        if (!lora.isInterruptPending()) {
            sleepUntilInterrupt();
        }
        // If interrupt is pending, loop immediately to process it
    }
}

void runProductionMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                       AddressManager* address_manager, HubRouter* hub_router,
                       NetworkStats* network_stats) {
    uint32_t last_sensor_time = 0;
    uint32_t last_heartbeat_time = 0;
    uint8_t led_counter = 0;
    
    printf("=== PRODUCTION MODE ACTIVE ===\n");
    printf("- Green LED heartbeat\n");
    printf("- Sensor readings every %d seconds\n", SENSOR_INTERVAL_MS / 1000);
    printf("- Minimal power consumption\n");
    
    while (true) {
        // Production LED: Subtle green heartbeat
        uint8_t brightness = (led_counter < 10) ? led_counter * 5 : (20 - led_counter) * 5;
        led.setPixel(0, 0, brightness, 0);  // Green heartbeat
        led.show();
        led_counter = (led_counter + 1) % 20;
        
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Send sensor data periodically
        if (current_time - last_sensor_time >= SENSOR_INTERVAL_MS) {
            // TODO: Replace with actual sensor readings
            // Example: Read temperature, humidity, soil moisture, battery level
            printf("Sensor reading cycle\n");
            last_sensor_time = current_time;
        }
        
        // Send heartbeat to hub
        if (current_time - last_heartbeat_time >= HEARTBEAT_INTERVAL_MS) {
            printf("Heartbeat\n");
            
            // Calculate real node status
            uint32_t uptime = current_time / 1000;  // Convert to seconds
            uint8_t battery_level = 85;  // Example battery level
            uint8_t signal_strength = 65;  // Example signal strength
            uint8_t active_sensors = CAP_TEMPERATURE | CAP_HUMIDITY | CAP_SOIL_MOISTURE;
            uint8_t error_flags = 0;  // No errors in production
            
            messenger.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, 
                                  signal_strength, active_sensors, error_flags);
            
            last_heartbeat_time = current_time;
        }
        
        // Check for interrupts first (more efficient than polling)
        if (lora.isInterruptPending()) {
            lora.handleInterrupt();
        }
        
        // Check for incoming messages (commands from hub, ACKs, etc.)
        if (lora.isMessageReady()) {
            uint8_t rx_buffer[MESSAGE_MAX_SIZE];
            int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
            
            if (rx_len > 0) {
                // Use common message processing function  
                processIncomingMessage(rx_buffer, rx_len, messenger, address_manager, hub_router, current_time, network_stats, &lora);
                
                // TODO: Add command processing for actuator control
                // Parse incoming actuator commands and control valves/pumps accordingly
            } else if (rx_len < 0) {
                lora.startReceive();  // Restart receive mode after error
            }
        }
        
        // Update retry timers for reliable message delivery
        messenger.update();
        
        // Sleep efficiently between iterations
        if (!lora.isInterruptPending()) {
            sleepUntilInterrupt();
        }
        // If interrupt is pending, loop immediately to process it
    }
}

uint64_t getDeviceId() {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    
    // Convert 8-byte board ID to uint64_t
    uint64_t device_id = 0;
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        device_id = (device_id << 8) | board_id.id[i];
    }
    
    return device_id;
}

bool attemptRegistration(ReliableMessenger& messenger, SX1276& lora, 
                        NodeConfigManager& config_manager, uint64_t device_id) {
    const int MAX_REGISTRATION_ATTEMPTS = 3;
    const uint32_t REGISTRATION_TIMEOUT_MS = 10000;  // 10 seconds per attempt
    const uint32_t RETRY_DELAY_MS = 5000;  // 5 seconds between attempts
    
    // Define node capabilities
    uint8_t capabilities = CAP_TEMPERATURE | CAP_SOIL_MOISTURE | CAP_BATTERY_MONITOR;
    uint16_t firmware_version = 0x0100;  // Version 1.0
    const char* device_name = "Demo Farm Node";
    
    for (int attempt = 1; attempt <= MAX_REGISTRATION_ATTEMPTS; attempt++) {
        printf("Registration attempt %d/%d\n", attempt, MAX_REGISTRATION_ATTEMPTS);
        
        // Send registration request
        if (!messenger.sendRegistrationRequest(HUB_ADDRESS, device_id, 
                                             NODE_TYPE_SENSOR, capabilities,
                                             firmware_version, device_name)) {
            printf("Failed to send registration request\n");
            if (attempt < MAX_REGISTRATION_ATTEMPTS) {
                sleep_ms(RETRY_DELAY_MS);
                continue;
            }
            return false;
        }
        
        // Wait for registration response
        uint32_t start_time = to_ms_since_boot(get_absolute_time());
        uint16_t assigned_address = ADDRESS_UNREGISTERED;
        bool response_received = false;
        
        while (to_ms_since_boot(get_absolute_time()) - start_time < REGISTRATION_TIMEOUT_MS) {
            // Check for interrupts first
            if (lora.isInterruptPending()) {
                lora.handleInterrupt();
            }
            
            // Check for incoming messages
            if (lora.isMessageReady()) {
                uint8_t rx_buffer[MESSAGE_MAX_SIZE];
                int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
                
                if (rx_len > 0) {
                    Message message;
                    if (MessageHandler::parseMessage(rx_buffer, rx_len, &message)) {
                    // Process the message
                    messenger.processIncomingMessage(rx_buffer, rx_len);
                    
                    // Check if it's a registration response
                    if (message.header.type == MSG_TYPE_REG_RESPONSE) {
                        const RegistrationResponsePayload* reg_response = 
                            MessageHandler::getRegistrationResponsePayload(&message);
                        
                        if (reg_response && reg_response->device_id == device_id) {
                            response_received = true;
                            
                            if (reg_response->status == REG_SUCCESS) {
                                assigned_address = reg_response->assigned_addr;
                                printf("Registration successful! Assigned address: 0x%04X\n", 
                                       assigned_address);
                                
                                // Update messenger with new address
                                messenger.updateNodeAddress(assigned_address);
                                
                                // Save configuration to flash
                                NodeConfiguration new_config = {};  // Initialize all fields to zero
                                new_config.magic = 0xBEEF1234;  // Use the correct magic number
                                new_config.assigned_address = assigned_address;
                                new_config.device_id = device_id;
                                new_config.node_type = NODE_TYPE_SENSOR;
                                new_config.capabilities = capabilities;
                                new_config.firmware_version = firmware_version;
                                new_config.registration_time = 0;  // Deprecated field, kept for compatibility
                                strncpy(new_config.device_name, device_name, sizeof(new_config.device_name) - 1);
                                
                                if (config_manager.saveConfiguration(new_config)) {
                                    printf("Configuration saved to flash\n");
                                } else {
                                    printf("Warning: Failed to save configuration to flash\n");
                                }
                                
                                return true;
                            } else {
                                printf("Registration failed with status: %d\n", reg_response->status);
                                
                                // If duplicate, no point retrying
                                if (reg_response->status == REG_ERROR_DUPLICATE) {
                                    printf("Device already registered, aborting\n");
                                    return false;
                                }
                            }
                            break;
                        }
                    }
                }
                } else if (rx_len < 0) {
                    lora.startReceive();
                }
            }
            
            // Update message retries
            messenger.update();
            
            // Small delay to prevent tight loop
            sleep_ms(100);
        }
        
        if (!response_received) {
            printf("Registration timeout - no response from hub\n");
        }
        
        if (attempt < MAX_REGISTRATION_ATTEMPTS) {
            printf("Retrying in %d seconds...\n", RETRY_DELAY_MS / 1000);
            sleep_ms(RETRY_DELAY_MS);
        }
    }
    
    return false;
}

void processIncomingMessage(uint8_t* rx_buffer, int rx_len, ReliableMessenger& messenger,
                          AddressManager* address_manager, HubRouter* hub_router, 
                          uint32_t current_time, NetworkStats* network_stats, SX1276* lora) {
    // Process message with reliable messenger (handles ACKs, sensor data, etc.)
    messenger.processIncomingMessage(rx_buffer, rx_len);
    
    // Record statistics if available
    if (network_stats && lora && rx_len >= sizeof(MessageHeader)) {
        const MessageHeader* header = reinterpret_cast<const MessageHeader*>(rx_buffer);
        network_stats->recordMessageReceived(header->src_addr, lora->getRssi(), 
                                           lora->getSnr(), false);
    }
    
    // If not a hub, we're done
    if (!hub_router || !address_manager || rx_len < sizeof(MessageHeader)) {
        return;
    }
    
    // Hub-specific processing
    const MessageHeader* header = reinterpret_cast<const MessageHeader*>(rx_buffer);
    uint16_t source_address = header->src_addr;
    
    // Update node activity tracking
    address_manager->updateLastSeen(source_address, current_time);
    hub_router->updateRouteOnline(source_address);
    
    // Handle registration requests
    if (header->type == MSG_TYPE_REGISTRATION) {
        const Message* msg = reinterpret_cast<const Message*>(rx_buffer);
        const RegistrationPayload* reg_payload = reinterpret_cast<const RegistrationPayload*>(msg->payload);
        
        // Register the node with AddressManager
        uint16_t assigned_addr = address_manager->registerNode(
            reg_payload->device_id,
            reg_payload->node_type,
            reg_payload->capabilities,
            reg_payload->firmware_ver,
            reg_payload->device_name
        );
        
        // Determine registration status
        uint8_t status = REG_SUCCESS;
        if (assigned_addr == 0x0000) {
            // Registration failed
            if (address_manager->isDeviceRegistered(reg_payload->device_id)) {
                status = REG_ERROR_DUPLICATE;
            } else {
                status = REG_ERROR_FULL;
            }
        }
        
        // Send registration response
        messenger.sendRegistrationResponse(
            header->src_addr,  // Send back to requesting node
            reg_payload->device_id,
            assigned_addr,
            status,
            30,  // Retry interval in seconds
            current_time / 1000  // Network time in seconds
        );
        
        if (status == REG_SUCCESS) {
            printf("Successfully registered node 0x%016llX with address 0x%04X\n",
                   reg_payload->device_id, assigned_addr);
        }
    }
    
    // Handle heartbeat messages with status logging  
    if (header->type == MSG_TYPE_HEARTBEAT) {
        const HeartbeatPayload* heartbeat = 
            reinterpret_cast<const HeartbeatPayload*>(rx_buffer + sizeof(MessageHeader));
        
        printf("Heartbeat from 0x%04X: uptime=%lus, battery=%u%%, signal=%u, sensors=0x%02X\n",
               source_address, heartbeat->uptime_seconds, heartbeat->battery_level,
               heartbeat->signal_strength, heartbeat->active_sensors);
    }
    
    // Try to route the message if it's not for the hub
    hub_router->processMessage(rx_buffer, rx_len, source_address);
}

void sleepUntilInterrupt() {
    // Without pico-extras, we use a short sleep that still provides power savings
    // The interrupt will wake us if a message arrives before the sleep completes
    sleep_ms(50);  // 50ms provides good balance between power and responsiveness
    
    // With pico-extras installed, you could use:
    // #ifdef PICO_EXTRAS_AVAILABLE
    //     // Configure DIO0 pin as wake source
    //     sleep_run_from_xosc();
    //     sleep_goto_dormant_until_pin(PIN_DIO0, true, true);  // Wake on high edge
    //     // Clocks are restored automatically after wake
    // #endif
    
    // To install pico-extras:
    // git clone https://github.com/raspberrypi/pico-extras.git
    // Then rebuild with: rm -rf build && cmake -B build && cmake --build build
}
