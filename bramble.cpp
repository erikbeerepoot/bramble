#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "lora/sx1276.h"
#include "lora/message.h"
#include "lora/reliable_messenger.h"
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "hal/neopixel.h"
#include "hal/flash.h"
#include "config/node_config.h"
#include "pico/unique_id.h"

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
#define NODE_ADDRESS            ADDRESS_HUB      // Set to ADDRESS_HUB for hub mode, or specific address for node
#define HUB_ADDRESS             ADDRESS_HUB      // Hub/gateway address
#define SENSOR_INTERVAL_MS      30000       // Send sensor data every 30 seconds
#define HEARTBEAT_INTERVAL_MS   60000       // Send heartbeat every minute
#define MAIN_LOOP_DELAY_MS      100         // Main loop processing delay

// Demo mode - set to false for production deployment
#define DEMO_MODE               true

// Forward declarations
void runDemoMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led, 
                 AddressManager* address_manager = nullptr, HubRouter* hub_router = nullptr);
void runProductionMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                       AddressManager* address_manager = nullptr, HubRouter* hub_router = nullptr);
void runHubMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                AddressManager& address_manager, HubRouter& hub_router);
bool initializeHardware(SX1276& lora, NeoPixel& led);
uint64_t getDeviceId();

int main()
{
    stdio_init_all();
    sleep_ms(2000); // Give USB time to enumerate
    
    printf("=== Bramble Starting ===\n");

    // Initialize hardware
    NeoPixel led(PIN_NEOPIXEL, 1);
    SX1276 lora(SPI_PORT, PIN_CS, PIN_RST, PIN_DIO0);
    
    if (!initializeHardware(lora, led)) {
        printf("Hardware initialization failed!\n");
        while(true) {
            sleep_ms(1000);
        }
    }
    
    // Initialize reliable messenger
    ReliableMessenger messenger(&lora, NODE_ADDRESS);
    
    // Start in receive mode
    lora.startReceive();
    
    // Check if this device should run as hub
    if (NODE_ADDRESS == ADDRESS_HUB) {
        printf("Starting as HUB - managing network and routing\n");
        
        // Initialize hub components
        Flash flash_hal;
        AddressManager address_manager;
        HubRouter hub_router(address_manager, messenger);
        
        if (DEMO_MODE) {
            printf("Starting HUB DEMO mode\n");
            runDemoMode(messenger, lora, led, &address_manager, &hub_router);
        } else {
            printf("Starting HUB PRODUCTION mode\n");
            runHubMode(messenger, lora, led, address_manager, hub_router);
        }
    } else {
        // Run as regular node
        if (DEMO_MODE) {
            printf("Starting NODE DEMO mode - sending test messages\n");
            runDemoMode(messenger, lora, led);
        } else {
            printf("Starting NODE PRODUCTION mode - real sensor monitoring\n");
            runProductionMode(messenger, lora, led);
        }
    }
    
    return 0; // Should never reach here
}

bool initializeHardware(SX1276& lora, NeoPixel& led) {
    // Initialize NeoPixel LED
    if (!led.begin()) {
        printf("Failed to initialize NeoPixel!\n");
        return false;
    }
    printf("NeoPixel initialized successfully!\n");

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
                AddressManager& address_manager, HubRouter& hub_router) {
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
        led.setPixelColor(0, 0, 0, brightness);  // Blue breathing
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
            
            last_maintenance_time = current_time;
        }
        
        // Check for incoming messages
        uint8_t rx_buffer[MESSAGE_MAX_SIZE];
        int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
        
        if (rx_len > 0) {
            printf("Hub received message (len=%d, RSSI=%d dBm)\n", 
                   rx_len, lora.getRssi());
            
            // Process message normally (handles registration, ACKs, etc.)
            messenger.processIncomingMessage(rx_buffer, rx_len);
            
            // Check if message needs routing to another node
            if (rx_len >= sizeof(MessageHeader)) {
                const MessageHeader* header = reinterpret_cast<const MessageHeader*>(rx_buffer);
                uint16_t source_address = header->src_addr;
                
                // Update node activity tracking
                address_manager.updateLastSeen(source_address, current_time);
                hub_router.updateRouteOnline(source_address);
                
                // Handle heartbeat messages with status logging
                if (header->type == MSG_TYPE_HEARTBEAT) {
                    const HeartbeatPayload* heartbeat = 
                        reinterpret_cast<const HeartbeatPayload*>(rx_buffer + sizeof(MessageHeader));
                    
                    printf("Heartbeat from 0x%04X: uptime=%lus, battery=%u%%, signal=%u, sensors=0x%02X\n",
                           source_address, heartbeat->uptime_seconds, heartbeat->battery_level,
                           heartbeat->signal_strength, heartbeat->active_sensors);
                }
                
                // Try to route the message if it's not for the hub
                hub_router.processMessage(rx_buffer, rx_len, source_address);
            }
        } else if (rx_len < 0) {
            lora.startReceive();
        }
        
        // Update retry timers and process queued messages
        messenger.update();
        hub_router.processQueuedMessages();
        
        sleep_ms(MAIN_LOOP_DELAY_MS);
    }
}

void runDemoMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led, 
                 AddressManager* address_manager, HubRouter* hub_router) {
    uint32_t last_demo_time = 0;
    uint32_t last_heartbeat_time = 0;
    
    printf("=== DEMO MODE ACTIVE ===\n");
    printf("- Colorful LED cycling\n");
    printf("- Test messages every 15 seconds\n");
    printf("- Verbose debug output\n");
    
    while (true) {
        // Demo LED: Cycle through colors for visual feedback
        static uint8_t hue = 0;
        led.setPixelColor(0, NeoPixel::colorHSV(hue * 256, 255, 50));
        led.show();
        hue = (hue + 1) % 64;
        
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
        
        // Check for incoming messages
        uint8_t rx_buffer[MESSAGE_MAX_SIZE];
        int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
        
        if (rx_len > 0) {
            printf("Received message (len=%d, RSSI=%d dBm, SNR=%.1f dB)\n", 
                   rx_len, lora.getRssi(), lora.getSnr());
            
            // Process message normally
            messenger.processIncomingMessage(rx_buffer, rx_len);
            
            // If running as hub, also handle routing
            if (hub_router && rx_len >= sizeof(MessageHeader)) {
                const MessageHeader* header = reinterpret_cast<const MessageHeader*>(rx_buffer);
                uint16_t source_address = header->src_addr;
                
                // Update node as online and try routing
                hub_router->updateRouteOnline(source_address);
                hub_router->processMessage(rx_buffer, rx_len, source_address);
            }
        } else if (rx_len < 0) {
            printf("Receive error (CRC or buffer issue)\n");
            lora.startReceive();
        }
        
        // Update retry timers for reliable message delivery
        messenger.update();
        
        // Update hub router if in hub mode
        if (hub_router) {
            hub_router->processQueuedMessages();
        }
        
        sleep_ms(MAIN_LOOP_DELAY_MS);
    }
}

void runProductionMode(ReliableMessenger& messenger, SX1276& lora, NeoPixel& led,
                       AddressManager* address_manager, HubRouter* hub_router) {
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
        led.setPixelColor(0, 0, brightness, 0);  // Green heartbeat
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
        
        // Check for incoming messages (commands from hub, ACKs, etc.)
        uint8_t rx_buffer[MESSAGE_MAX_SIZE];
        int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
        
        if (rx_len > 0) {
            // Process with reliable messenger (handles ACKs and commands automatically)
            messenger.processIncomingMessage(rx_buffer, rx_len);
            
            // TODO: Add command processing for actuator control
            // Parse incoming actuator commands and control valves/pumps accordingly
        } else if (rx_len < 0) {
            lora.startReceive();  // Restart receive mode after error
        }
        
        // Update retry timers for reliable message delivery
        messenger.update();
        
        sleep_ms(MAIN_LOOP_DELAY_MS);
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
