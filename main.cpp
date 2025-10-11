/**
 * @file main.cpp
 * @brief Unified entry point for all Bramble hardware variants
 * 
 * This replaces the previous bramble.cpp and provides a single entry point
 * that creates the appropriate mode based on compile-time hardware configuration.
 */

#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

// HAL includes
#include "hal/neopixel.h"
#include "hal/logger.h"
#include "hal/flash.h"

// LoRa includes
#include "lora/sx1276.h"
#include "lora/reliable_messenger.h"
#include "lora/message.h"
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"

// Configuration includes
#include "config/node_config.h"
#include "config/hub_config.h"

// Common mode includes
#include "modes/demo_mode.h"
#include "modes/hub_mode.h"

// Mode includes based on hardware variant
#ifdef HARDWARE_CONTROLLER
    #include "modes/controller_mode.h"
#elif HARDWARE_IRRIGATION
    #include "modes/irrigation_mode.h"
#elif HARDWARE_SENSOR
    #include "modes/sensor_mode.h"
#else
    #include "modes/generic_mode.h"
#endif

// Hardware configuration - Adafruit Feather RP2040 LoRa
static auto SPI_PORT = spi1;  // SPI1 for LoRa module
constexpr uint PIN_MISO = 8;   // SPI1 RX
constexpr uint PIN_CS   = 16;  // GPIO as chip select
constexpr uint PIN_SCK  = 14;  // SPI1 SCK
constexpr uint PIN_MOSI = 15;  // SPI1 TX
constexpr uint PIN_RST  = 17;  // Reset pin
constexpr uint PIN_DIO0 = 21;  // DIO0 interrupt pin
constexpr uint PIN_NEOPIXEL = 4;

// Controller input pins (Adafruit Feather RP2040)
constexpr uint PIN_A0 = 26;  // A0 analog/digital input
constexpr uint PIN_A1 = 27;  // A1 analog/digital input

// Demo mode configuration is set by CMake

// Forward declarations
bool initializeHardware(SX1276& lora, NeoPixel& led);
uint64_t getDeviceId();
bool attemptRegistration(ReliableMessenger& messenger, SX1276& lora, 
                        NodeConfigManager& config_manager, uint64_t device_id);
void sleepUntilInterrupt();

/**
 * @brief Main entry point
 * 
 * Initializes hardware and starts the appropriate mode based on
 * compile-time configuration and role.
 */
int main() {
    stdio_init_all();
    sleep_ms(2000);  // Give USB time to enumerate
    
    printf("\n==== Bramble Network Device ====\n");
    
    // Determine role from build configuration
    #if defined(DEFAULT_IS_HUB) && DEFAULT_IS_HUB
        constexpr bool is_hub = true;
    #else
        constexpr bool is_hub = false;
    #endif
    
    printf("Hardware variant: ");
    #ifdef HARDWARE_CONTROLLER
        printf("CONTROLLER\n");
    #elif HARDWARE_IRRIGATION
        printf("IRRIGATION\n");
    #elif HARDWARE_SENSOR
        printf("SENSOR\n");
    #else
        printf("GENERIC\n");
    #endif
    
    printf("Network role: %s\n", is_hub ? "HUB" : "NODE");
    
    // Initialize hardware
    NeoPixel led(PIN_NEOPIXEL, 1);
    
    // Show role with initial LED color
    if (is_hub) {
        led.setPixel(0, 0, 0, 255);  // Blue for hub
        led.show();
    } else {
        led.setPixel(0, 0, 255, 0);  // Green for node
        led.show();
    }
    
    // Initialize LoRa
    SX1276 lora(SPI_PORT, PIN_CS, PIN_RST, PIN_DIO0);
    
    if (!initializeHardware(lora, led)) {
        led.setPixel(0, 255, 0, 0);  // Red for error
        led.show();
        panic("Hardware initialization failed!");
    }
    
    // Initialize network statistics
    NetworkStats network_stats;
    
    if (is_hub) {
        // Hub mode initialization
        printf("=== STARTING AS HUB ===\n");
        
        Flash flash;
        HubConfigManager hub_config_manager(flash);
        
        // Hub uses address 0x0000
        ReliableMessenger messenger(&lora, ADDRESS_HUB, &network_stats);
        AddressManager address_manager;
        HubRouter hub_router(address_manager, messenger);
        
        // Create appropriate hub mode
        #ifdef HARDWARE_CONTROLLER
            if (DEMO_MODE) {
                printf("Starting CONTROLLER DEMO mode\n");
                // TODO: Create ControllerDemoMode when needed
                DemoMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
                mode.run();
            } else {
                printf("Starting CONTROLLER PRODUCTION mode\n");
                ControllerMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
                mode.run();
            }
        #else
            // Non-controller hardware running as hub (emergency mode)
            printf("WARNING: Non-controller hardware running as HUB!\n");
            if (DEMO_MODE) {
                DemoMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
                mode.run();
            } else {
                HubMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
                mode.run();
            }
        #endif
        
    } else {
        // Node mode initialization
        printf("=== STARTING AS NODE ===\n");
        
        NodeConfiguration node_config;
        Flash flash;
        NodeConfigManager config_manager(flash);
        
        // Start with unregistered address
        uint16_t current_address = ADDRESS_UNREGISTERED;
        
        // Check for saved configuration
        if (config_manager.loadConfiguration(node_config) && 
            node_config.assigned_address != ADDRESS_UNREGISTERED) {
            current_address = node_config.assigned_address;
            printf("Found saved address: 0x%04X\n", current_address);
        }
        
        // Create messenger with current address
        ReliableMessenger messenger(&lora, current_address, &network_stats);
        
        // Get device ID for registration
        uint64_t device_id = getDeviceId();
        
        // Attempt registration (updates messenger address if successful)
        if (attemptRegistration(messenger, lora, config_manager, device_id)) {
            printf("Registration successful!\n");
        } else {
            printf("Registration failed - continuing with address 0x%04X\n", 
                   messenger.getNodeAddress());
        }
        
        // Create appropriate node mode
        #ifdef HARDWARE_IRRIGATION
            if (DEMO_MODE) {
                printf("Starting IRRIGATION DEMO mode\n");
                // For now use generic demo mode
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                printf("Starting IRRIGATION PRODUCTION mode\n");
                IrrigationMode mode(messenger, lora, led, nullptr, nullptr, &network_stats, false); // Disable multicore
                mode.run();
            }
        #elif HARDWARE_SENSOR
            if (DEMO_MODE) {
                printf("Starting SENSOR DEMO mode\n");
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                printf("Starting SENSOR PRODUCTION mode\n");
                SensorMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            }
        #elif HARDWARE_CONTROLLER
            // Controller hardware running as node (unusual but possible)
            printf("WARNING: Controller hardware running as NODE!\n");
            if (DEMO_MODE) {
                printf("Starting CONTROLLER NODE DEMO mode\n");
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                printf("Starting CONTROLLER NODE mode (using hub mode)\n");
                HubMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            }
        #else
            // Generic node
            if (DEMO_MODE) {
                printf("Starting GENERIC DEMO mode\n");
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                printf("Starting GENERIC PRODUCTION mode\n");
                GenericMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            }
        #endif
    }
    
    return 0;  // Should never reach here
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
    
    // Initialize LoRa module
    if (!lora.begin()) {
        printf("ERROR: Failed to initialize LoRa module!\n");
        return false;
    }
    
    // Configure LoRa parameters
    lora.setFrequency(915000000);  // 915 MHz for US
    lora.setTxPower(17);  // 17 dBm
    lora.setBandwidth(125000);  // 125 kHz
    lora.setSpreadingFactor(7);
    lora.setCodingRate(5);
    lora.setPreambleLength(8);
    lora.setCrc(true);  // Enable CRC
    
    printf("LoRa module initialized successfully\n");
    
    // Enable interrupt mode for efficient operation
    if (lora.enableInterruptMode()) {
        printf("Interrupt mode enabled - efficient power usage\n");
    } else {
        printf("Failed to enable interrupt mode - using polling\n");
    }
    
    // Start in receive mode
    lora.startReceive();
    
    return true;
}

uint64_t getDeviceId() {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    
    // Convert 8-byte array to 64-bit value
    uint64_t id = 0;
    for (int i = 0; i < 8; i++) {
        id = (id << 8) | board_id.id[i];
    }
    
    return id;
}

bool attemptRegistration(ReliableMessenger& messenger, SX1276& lora, 
                        NodeConfigManager& config_manager, uint64_t device_id) {
    printf("Attempting registration with hub...\n");
    printf("Device ID: 0x%016llX\n", device_id);
    
    // Determine node type and capabilities based on hardware
    uint8_t node_type = NODE_TYPE_SENSOR;  // Default
    uint8_t capabilities = 0;
    const char* device_name = "Generic Node";
    
    #ifdef HARDWARE_IRRIGATION
        node_type = NODE_TYPE_HYBRID;
        capabilities = CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE;
        device_name = "Irrigation Node";
    #elif HARDWARE_SENSOR
        node_type = NODE_TYPE_SENSOR;
        capabilities = CAP_TEMPERATURE | CAP_HUMIDITY;
        device_name = "Sensor Node";
    #elif HARDWARE_CONTROLLER
        node_type = NODE_TYPE_CONTROLLER;
        capabilities = CAP_CONTROLLER | CAP_SCHEDULING;
        device_name = "Controller";
    #endif
    
    // Send registration request
    if (!messenger.sendRegistrationRequest(ADDRESS_HUB, device_id, node_type,
                                         capabilities, 0x0100, device_name)) {
        printf("Failed to send registration request\n");
        return false;
    }
    
    // Wait for response
    uint32_t start_time = to_ms_since_boot(get_absolute_time());
    uint32_t timeout_ms = 5000;  // 5 second timeout
    
    while (to_ms_since_boot(get_absolute_time()) - start_time < timeout_ms) {
        // Check for interrupt
        if (lora.isInterruptPending()) {
            lora.handleInterrupt();
        }
        
        // Check for message
        if (lora.isMessageReady()) {
            uint8_t buffer[256];
            int len = lora.receive(buffer, sizeof(buffer));
            
            if (len > 0) {
                // Let messenger process it (will handle registration response)
                if (messenger.processIncomingMessage(buffer, len)) {
                    // Check if we got a new address
                    uint16_t new_addr = messenger.getNodeAddress();
                    if (new_addr != ADDRESS_UNREGISTERED) {
                        printf("Got address assignment: 0x%04X\n", new_addr);
                        
                        // Save to flash
                        NodeConfiguration config;
                        config.assigned_address = new_addr;
                        config.device_id = device_id;
                        
                        if (config_manager.saveConfiguration(config)) {
                            printf("Configuration saved to flash\n");
                        }
                        
                        return true;
                    }
                }
            }
        }
        
        sleep_ms(10);
    }
    
    printf("Registration timeout\n");
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
    // For now, just use a short sleep
    // TODO: Implement proper interrupt-based wakeup
    sleep_ms(10);
}