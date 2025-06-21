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

// Application modes
#include "src/demo_mode.h"
#include "src/hub_mode.h"
#include "src/production_mode.h"

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
            DemoMode mode(messenger, lora, led, &address_manager, &hub_router, network_stats);
            mode.run();
        } else {
            printf("Starting HUB PRODUCTION mode\n");
            HubMode mode(messenger, lora, led, &address_manager, &hub_router, network_stats);
            mode.run();
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
            DemoMode mode(messenger, lora, led, nullptr, nullptr, network_stats);
            mode.run();
        } else {
            main_logger.info("Starting NODE PRODUCTION mode - real sensor monitoring");
            ProductionMode mode(messenger, lora, led, nullptr, nullptr, network_stats);
            mode.run();
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
