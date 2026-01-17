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
#include "hardware/structs/iobank0.h"

// HAL includes
#include "hal/neopixel.h"
#include "hal/logger.h"
#include "hal/flash.h"
#include "hal/external_flash.h"

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

// Hardware configuration - Bramble board v3
static auto SPI_PORT = spi1;  // SPI1 for LoRa module
constexpr uint PIN_MISO = 8;   // SPI1 RX
constexpr uint PIN_SCK  = 14;  // SPI1 SCK
constexpr uint PIN_MOSI = 15;  // SPI1 TX
constexpr uint PIN_CS   = 16;  // GPIO as chip select
constexpr uint PIN_RST  = 17;  // Reset pin (directly to RFM95W)
constexpr uint PIN_DIO0 = 21;  // DIO0 interrupt pin (RFM_IO0)
constexpr uint PIN_NEOPIXEL = 4;

// Controller input pins (Adafruit Feather RP2040)
constexpr uint PIN_A0 = 26;  // A0 analog/digital input
constexpr uint PIN_A1 = 27;  // A1 analog/digital input

// Demo mode configuration is set by CMake
// Debug UART: printf goes to GPIO12 (VALVE_3) via CMakeLists.txt config

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
    // Initialize stdio (UART on GPIO12 configured via CMakeLists.txt)
    stdio_init_all();

    // Create main logger
    Logger log("Main");

    log.info("==== Bramble Network Device ====");

    // Test external flash
    // Results stored in volatile vars for debugger inspection
    log.debug("--- External Flash Test ---");
    ExternalFlash ext_flash;

    volatile bool flash_ok = false;
    volatile uint8_t flash_mfr = 0;
    volatile uint8_t flash_type = 0;
    volatile uint8_t flash_cap = 0;
    volatile uint8_t flash_data[16] = {0};

    if (ext_flash.init()) {
        uint8_t mfr, mem_type, capacity;
        if (ext_flash.readId(mfr, mem_type, capacity) == ExternalFlashResult::Success) {
            flash_mfr = mfr;
            flash_type = mem_type;
            flash_cap = capacity;

            log.debug("Flash ID: Mfr=0x%02X Type=0x%02X Cap=0x%02X", mfr, mem_type, capacity);

            // Check if it's a Micron flash (0x20)
            if (mfr == 0x20) {
                flash_ok = true;
            }

            // Try reading first 16 bytes
            uint8_t buffer[16];
            if (ext_flash.read(0, buffer, sizeof(buffer)) == ExternalFlashResult::Success) {
                for (int i = 0; i < 16; i++) {
                    flash_data[i] = buffer[i];
                }
                // Log hex data (simplified - just first 4 bytes to avoid long output)
                log.debug("First 4 bytes: %02X %02X %02X %02X ...",
                         buffer[0], buffer[1], buffer[2], buffer[3]);
            }
        }
    } else {
        log.error("External flash init FAILED");
    }

    // SET BREAKPOINT HERE to inspect results:
    // flash_ok, flash_mfr, flash_type, flash_cap, flash_data
    volatile int flash_test_done = 1;  // Breakpoint marker
    (void)flash_test_done;  // Prevent unused warning

    log.debug("--- End Flash Test ---");
    
    // Determine role from build configuration
    #if defined(DEFAULT_IS_HUB) && DEFAULT_IS_HUB
        constexpr bool is_hub = true;
    #else
        constexpr bool is_hub = false;
    #endif
    
    #ifdef HARDWARE_CONTROLLER
        log.info("Hardware variant: CONTROLLER");
    #elif HARDWARE_IRRIGATION
        log.info("Hardware variant: IRRIGATION");
    #elif HARDWARE_SENSOR
        log.info("Hardware variant: SENSOR");
    #else
        log.info("Hardware variant: GENERIC");
    #endif

    log.info("Network role: %s", is_hub ? "HUB" : "NODE");
    
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
        // panic("Hardware initialization failed!");
    }
    
    // Initialize network statistics
    NetworkStats network_stats;
    
    if (is_hub) {
        // Hub mode initialization
        log.info("=== STARTING AS HUB ===");

        Flash flash;
        HubConfigManager hub_config_manager(flash);

        // Hub uses address 0x0000
        ReliableMessenger messenger(&lora, ADDRESS_HUB, &network_stats);
        AddressManager address_manager;
        HubRouter hub_router(address_manager, messenger);

        // Create appropriate hub mode
        #ifdef HARDWARE_CONTROLLER
            if (DEMO_MODE) {
                log.info("Starting CONTROLLER DEMO mode");
                // TODO: Create ControllerDemoMode when needed
                DemoMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
                mode.run();
            } else {
                log.info("Starting CONTROLLER PRODUCTION mode");
                ControllerMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
                mode.run();
            }
        #else
            // Non-controller hardware running as hub (emergency mode)
            log.warn("Non-controller hardware running as HUB!");
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
        log.info("=== STARTING AS NODE ===");

        NodeConfiguration node_config;
        Flash flash;
        NodeConfigManager config_manager(flash);

        // Start with unregistered address
        uint16_t current_address = ADDRESS_UNREGISTERED;

        // Check for saved configuration
        if (config_manager.loadConfiguration(node_config) &&
            node_config.assigned_address != ADDRESS_UNREGISTERED) {
            current_address = node_config.assigned_address;
            log.info("Found saved address: 0x%04X", current_address);
        }

        // Create messenger with current address
        ReliableMessenger messenger(&lora, current_address, &network_stats);

        // Get device ID for registration
        uint64_t device_id = getDeviceId();

        // Skip registration if we have a saved address
        // IrrigationMode will handle deferred registration based on PMU wake reason
        if (current_address == ADDRESS_UNREGISTERED) {
            log.info("No saved address - attempting registration...");
            if (attemptRegistration(messenger, lora, config_manager, device_id)) {
                log.info("Registration successful!");
            } else {
                log.warn("Registration failed - will retry after PMU init");
            }
        } else {
            log.info("Using saved address 0x%04X - skipping boot registration", current_address);
            log.info("(Will re-register if PMU reports External wake)");
        }

        // Create appropriate node mode
        #ifdef HARDWARE_IRRIGATION
            if (DEMO_MODE) {
                log.info("Starting IRRIGATION DEMO mode");
                // For now use generic demo mode
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                log.info("Starting IRRIGATION PRODUCTION mode");
                IrrigationMode mode(messenger, lora, led, nullptr, nullptr, &network_stats, false); // Disable multicore
                mode.run();
            }
        #elif HARDWARE_SENSOR
            if (DEMO_MODE) {
                log.info("Starting SENSOR DEMO mode");
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                log.info("Starting SENSOR PRODUCTION mode");
                SensorMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            }
        #elif HARDWARE_CONTROLLER
            // Controller hardware running as node (unusual but possible)
            log.warn("Controller hardware running as NODE!");
            if (DEMO_MODE) {
                log.info("Starting CONTROLLER NODE DEMO mode");
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                log.info("Starting CONTROLLER NODE mode (using hub mode)");
                HubMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            }
        #else
            // Generic node
            if (DEMO_MODE) {
                log.info("Starting GENERIC DEMO mode");
                DemoMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            } else {
                log.info("Starting GENERIC PRODUCTION mode");
                GenericMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
                mode.run();
            }
        #endif
    }
    
    return 0;  // Should never reach here
}

bool initializeHardware(SX1276& lora, NeoPixel& led) {
    Logger log("Hardware");

    // SPI initialization - 1MHz for reliable communication
    spi_init(SPI_PORT, 1000*1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    log.debug("System Clock: %d Hz, USB Clock: %d Hz",
              clock_get_hz(clk_sys), clock_get_hz(clk_usb));

    // Initialize LoRa module
    if (!lora.begin()) {
        log.error("Failed to initialize LoRa module!");
        return false;
    }

    // Configure LoRa parameters
    lora.setFrequency(915000000);  // 915 MHz for US
    lora.setTxPower(20);  // 17 dBm
    lora.setBandwidth(125000);  // 125 kHz
    lora.setSpreadingFactor(9);
    lora.setCodingRate(5);
    lora.setPreambleLength(8);
    lora.setCrc(true);  // Enable CRC

    log.info("LoRa module initialized successfully");

    // Enable interrupt mode for efficient operation
    if (lora.enableInterruptMode()) {
        log.debug("Interrupt mode enabled - efficient power usage");
    } else {
        log.warn("Failed to enable interrupt mode - using polling");
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
    Logger log("Registration");

    log.info("Attempting registration with hub...");
    log.info("Device ID: 0x%016llX", device_id);

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

    // Send registration request and store sequence number
    uint8_t registration_seq = messenger.sendRegistrationRequest(ADDRESS_HUB, device_id, node_type,
                                                                 capabilities, 0x0100, device_name);
    if (registration_seq == 0) {
        log.error("Failed to send registration request");
        return false;
    }

    // Wait for response
    uint32_t start_time = to_ms_since_boot(get_absolute_time());
    uint32_t timeout_ms = 5000;  // 5 second timeout

    while (to_ms_since_boot(get_absolute_time()) - start_time < timeout_ms) {
        // CRITICAL: Process message queue to actually send the registration request
        // Without this, the message stays queued and never transmits!
        messenger.update();

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
                        log.info("Got address assignment: 0x%04X", new_addr);

                        // CRITICAL: Cancel the pending registration message to prevent retries
                        // The registration succeeded, so we don't want it retrying in application mode
                        messenger.cancelPendingMessage(registration_seq);

                        // Save to flash
                        NodeConfiguration config;
                        config.assigned_address = new_addr;
                        config.device_id = device_id;

                        if (config_manager.saveConfiguration(config)) {
                            log.debug("Configuration saved to flash");
                        }

                        return true;
                    }
                }
            }
        }

        sleep_ms(10);
    }

    log.warn("Registration timeout");
    return false;
}

void processIncomingMessage(uint8_t* rx_buffer, int rx_len, ReliableMessenger& messenger,
                          AddressManager* address_manager, HubRouter* hub_router,
                          uint32_t current_time, NetworkStats* network_stats, SX1276* lora) {
    Logger log("Message");

    // Process message with reliable messenger (handles ACKs, sensor data, etc.)
    messenger.processIncomingMessage(rx_buffer, rx_len);

    // Record statistics if available
    if (network_stats && lora && rx_len >= static_cast<int>(sizeof(MessageHeader))) {
        const MessageHeader* header = reinterpret_cast<const MessageHeader*>(rx_buffer);
        network_stats->recordMessageReceived(header->src_addr, lora->getRssi(),
                                           lora->getSnr(), false);
    }

    // If not a hub, we're done
    if (!hub_router || !address_manager || rx_len < static_cast<int>(sizeof(MessageHeader))) {
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
            log.info("Successfully registered node 0x%016llX with address 0x%04X",
                   reg_payload->device_id, assigned_addr);
        }
    }

    // Handle heartbeat messages with status logging
    if (header->type == MSG_TYPE_HEARTBEAT) {
        const HeartbeatPayload* heartbeat =
            reinterpret_cast<const HeartbeatPayload*>(rx_buffer + sizeof(MessageHeader));

        log.debug("Heartbeat from 0x%04X: uptime=%lus, battery=%u%%, signal=%u, sensors=0x%02X",
               source_address, heartbeat->uptime_seconds, heartbeat->battery_level,
               heartbeat->signal_strength, heartbeat->active_sensors);
    }

    // Handle CHECK_UPDATES from nodes
    if (header->type == MSG_TYPE_CHECK_UPDATES) {
        const Message* msg = reinterpret_cast<const Message*>(rx_buffer);
        const CheckUpdatesPayload* payload = reinterpret_cast<const CheckUpdatesPayload*>(msg->payload);

        log.debug("CHECK_UPDATES from 0x%04X (node_seq=%d)",
               source_address, payload->node_sequence);

        hub_router->handleCheckUpdates(source_address, payload->node_sequence);
    }

    // Try to route the message if it's not for the hub
    hub_router->processMessage(rx_buffer, rx_len, source_address);
}

void sleepUntilInterrupt() {
    // For now, just use a short sleep
    // TODO: Implement proper interrupt-based wakeup
    sleep_ms(10);
}