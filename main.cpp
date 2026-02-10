/**
 * @file main.cpp
 * @brief Unified entry point for all Bramble hardware variants
 *
 * Creates the appropriate mode based on compile-time hardware configuration.
 */

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/structs/iobank0.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"

// HAL includes
#include "hal/external_flash.h"
#include "hal/flash.h"
#include "hal/logger.h"
#include "hal/neopixel.h"
#include "storage/log_flash_buffer.h"

// USB includes
#include "usb/msc_disk.h"
#include "usb/usb_stdio.h"

// LoRa includes
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/message.h"
#include "lora/network_stats.h"
#include "lora/reliable_messenger.h"
#include "lora/sx1276.h"

// Configuration includes
#include "config/hub_config.h"
#include "config/node_config.h"
#include "version.h"

// Common mode includes
#include "modes/hub_mode.h"

// Mode includes based on hardware variant
#ifdef HARDWARE_CONTROLLER
#include "modes/controller_mode.h"
#elif HARDWARE_IRRIGATION
#include "modes/irrigation_mode.h"
#elif HARDWARE_SENSOR
#include "modes/sensor_mode.h"
#else
#include "modes/application_mode.h"
#endif

// Hardware configuration - Bramble board v3
static auto SPI_PORT = spi1;   // SPI1 for LoRa module
constexpr uint PIN_MISO = 8;   // SPI1 RX
constexpr uint PIN_SCK = 14;   // SPI1 SCK
constexpr uint PIN_MOSI = 15;  // SPI1 TX
constexpr uint PIN_CS = 16;    // GPIO as chip select
constexpr uint PIN_RST = 17;   // Reset pin (directly to RFM95W)
constexpr uint PIN_DIO0 = 21;  // DIO0 interrupt pin (RFM_IO0)
constexpr uint PIN_NEOPIXEL = 4;

// Controller input pins (Adafruit Feather RP2040)
constexpr uint PIN_A0 = 26;  // A0 analog/digital input
constexpr uint PIN_A1 = 27;  // A1 analog/digital input

// Demo mode configuration is set by CMake
// Debug UART: printf goes to GPIO12 (VALVE_3) via CMakeLists.txt config

/**
 * @brief Node configuration info for registration
 */
struct VariantInfo {
    uint8_t node_type;
    uint8_t capabilities;
    const char *variant_name;
};

/**
 * @brief Get variant info based on hardware variant
 * @return VariantInfo with type, capabilities, and name
 */
inline VariantInfo getVariantInfo()
{
#ifdef HARDWARE_IRRIGATION
    return {NODE_TYPE_HYBRID, CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE, "Irrigation Node"};
#elif HARDWARE_SENSOR
    return {NODE_TYPE_SENSOR, CAP_TEMPERATURE | CAP_HUMIDITY, "Sensor Node"};
#elif HARDWARE_CONTROLLER
    return {NODE_TYPE_CONTROLLER, CAP_CONTROLLER | CAP_SCHEDULING, "Controller"};
#else
    return {NODE_TYPE_SENSOR, 0, "Generic Node"};
#endif
}

// Forward declarations
bool initializeHardware(SX1276 &lora, NeoPixel &led);
uint64_t getDeviceId();
bool attemptRegistration(ReliableMessenger &messenger, SX1276 &lora,
                         NodeConfigManager &config_manager, uint64_t device_id);

/**
 * @brief Main entry point
 *
 * Initializes hardware and starts the appropriate mode based on
 * compile-time configuration and role.
 */
int main()
{
    // Initialize stdio (UART configured via CMakeLists.txt)
    stdio_init_all();

    // Initialize composite USB CDC+MSC (replaces pico_stdio_usb)
    usb_stdio_init();

    // Configure Logger to check USB connection state for power savings
    Logger::setUsbConnectedCheck(usb_stdio_connected);

    // Create main logger
    Logger log("Main");

    log.info("==== Bramble Network Device ====");
    log.info("Firmware version: v%d.%d.%d", BRAMBLE_VERSION_MAJOR, BRAMBLE_VERSION_MINOR,
             BRAMBLE_VERSION_BUILD);

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

    // Initialize external flash and log buffer for all modes
    ExternalFlash log_external_flash;
    static LogFlashBuffer log_flash_buffer(log_external_flash);
    if (log_external_flash.init()) {
        if (log_flash_buffer.init()) {
            Logger::setFlashSink(&log_flash_buffer);
            // Initialize MSC disk so log drive is accessible over USB
            msc_disk_init(&log_flash_buffer, &log_external_flash);
            log.info("Flash log storage initialized (USB log drive active)");
        } else {
            log.warn("Failed to init log flash buffer");
        }
    } else {
        log.warn("Failed to init external flash for logging");
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
        if (address_manager.load(flash)) {
            log.info("Loaded %zu registered nodes from flash",
                     address_manager.getRegisteredNodeCount());
        } else {
            log.info("No saved registry found - starting fresh");
        }
        HubRouter hub_router(address_manager, messenger);

// Create appropriate hub mode
#ifdef HARDWARE_CONTROLLER
        log.info("Starting CONTROLLER mode");
        ControllerMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
        mode.run();
#else
        // Non-controller hardware running as hub (emergency mode)
        log.warn("Non-controller hardware running as HUB!");
        HubMode mode(messenger, lora, led, &address_manager, &hub_router, &network_stats);
        mode.run();
#endif

    } else {
        // Node mode initialization
        log.info("=== STARTING AS NODE ===");

        NodeConfiguration node_config;
        Flash flash;
        NodeConfigManager config_manager(flash);

#ifdef HARDWARE_SENSOR
        // Sensor nodes get address from PMU RAM, not flash
        // SensorMode handles registration on cold start via attemptDeferredRegistration()
        uint16_t current_address = ADDRESS_UNREGISTERED;
        log.info("Sensor node - address managed by PMU state");

        // Create messenger with unregistered address - will be set by SensorMode
        ReliableMessenger messenger(&lora, current_address, &network_stats);

        // Get device ID for re-registration callback
        uint64_t device_id = getDeviceId();
        VariantInfo variant = getVariantInfo();

        // Set up registration success callback (update messenger address only, not flash)
        messenger.setRegistrationSuccessCallback([](uint16_t new_address) {
            printf("Registered with address 0x%04X (stored in PMU RAM)\n", new_address);
        });

        // Re-registration callback: hub doesn't recognize us, re-send registration
        auto reregistration_callback = [&messenger, device_id, variant]() {
            printf("Re-registering with hub...\n");
            messenger.sendRegistrationRequest(ADDRESS_HUB, device_id, variant.node_type,
                                              variant.capabilities, BRAMBLE_FIRMWARE_VERSION,
                                              variant.variant_name);
        };

        // No registration attempt here - SensorMode handles it on cold start
        log.info("Starting SENSOR mode");
        SensorMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
        mode.setReregistrationCallback(reregistration_callback);
        mode.run();
#else
        // Non-sensor nodes: use flash-based address management
        // Start with unregistered address
        uint16_t current_address = ADDRESS_UNREGISTERED;

        // Check for saved configuration
        if (config_manager.loadConfiguration(node_config) &&
            node_config.assigned_address != ADDRESS_UNREGISTERED) {
            // Force re-registration if firmware version changed
            if (node_config.firmware_version != BRAMBLE_FIRMWARE_VERSION) {
                log.info("Firmware changed (0x%08lX -> 0x%08lX) - forcing re-registration",
                         node_config.firmware_version, BRAMBLE_FIRMWARE_VERSION);
                // Keep ADDRESS_UNREGISTERED to trigger registration
            } else {
                current_address = node_config.assigned_address;
            }
        }

        // Create messenger with current address
        ReliableMessenger messenger(&lora, current_address, &network_stats);

        // Get device ID for registration
        uint64_t device_id = getDeviceId();

        // Get variant info for registration
        VariantInfo variant = getVariantInfo();

        // Set up registration success callback (save new address to flash)
        messenger.setRegistrationSuccessCallback(
            [&config_manager, device_id](uint16_t new_address) {
                printf("Saving new address 0x%04X to flash\n", new_address);
                NodeConfiguration config;
                config.assigned_address = new_address;
                config.device_id = device_id;
                if (config_manager.saveConfiguration(config)) {
                    printf("Configuration saved successfully\n");
                } else {
                    printf("ERROR: Failed to save configuration\n");
                }
            });

        // Always register on boot - hub returns correct address for our device_id
        // This ensures address conflicts are detected and resolved
        log.info("Registering node with hub...");
        if (attemptRegistration(messenger, lora, config_manager, device_id)) {
            uint16_t assigned = messenger.getNodeAddress();
            if (assigned != current_address && current_address != ADDRESS_UNREGISTERED) {
                log.info("Address changed: 0x%04X -> 0x%04X", current_address, assigned);
            }
            current_address = assigned;
            log.info("Registration successful - using address 0x%04X", current_address);
        } else {
            if (current_address != ADDRESS_UNREGISTERED) {
                log.warn("Registration failed - using saved address 0x%04X", current_address);
                messenger.setNodeAddress(current_address);
            } else {
                log.error("Registration failed and no saved address!");
            }
        }

        // Re-registration callback: hub doesn't recognize us, re-send registration
        auto reregistration_callback = [&messenger, device_id, variant]() {
            printf("Re-registering with hub...\n");
            messenger.sendRegistrationRequest(ADDRESS_HUB, device_id, variant.node_type,
                                              variant.capabilities, BRAMBLE_FIRMWARE_VERSION,
                                              variant.variant_name);
        };

// Create appropriate node mode
#ifdef HARDWARE_IRRIGATION
        log.info("Starting IRRIGATION mode");
        IrrigationMode mode(messenger, lora, led, nullptr, nullptr, &network_stats, false);
        mode.setReregistrationCallback(reregistration_callback);
        mode.run();
#elif HARDWARE_CONTROLLER
        // Controller hardware running as node (unusual but possible)
        log.warn("Controller hardware running as NODE!");
        log.info("Starting CONTROLLER NODE mode (using hub mode)");
        HubMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
        mode.setReregistrationCallback(reregistration_callback);
        mode.run();
#else
        // Generic node
        log.info("Starting GENERIC mode");
        ApplicationMode mode(messenger, lora, led, nullptr, nullptr, &network_stats);
        mode.setReregistrationCallback(reregistration_callback);
        mode.run();
#endif
#endif  // HARDWARE_SENSOR
    }

    return 0;  // Should never reach here
}

bool initializeHardware(SX1276 &lora, NeoPixel &led)
{
    Logger log("Hardware");

    // SPI initialization - 1MHz for reliable communication
    spi_init(SPI_PORT, 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    log.debug("System Clock: %d Hz, USB Clock: %d Hz", clock_get_hz(clk_sys),
              clock_get_hz(clk_usb));

    // Initialize LoRa module
    if (!lora.begin()) {
        log.error("Failed to initialize LoRa module!");
        return false;
    }

    // Configure LoRa parameters
    lora.setFrequency(915000000);  // 915 MHz for US
    lora.setTxPower(20);           // 17 dBm
    lora.setBandwidth(125000);     // 125 kHz
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

uint64_t getDeviceId()
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    // Convert 8-byte array to 64-bit value
    uint64_t id = 0;
    for (int i = 0; i < 8; i++) {
        id = (id << 8) | board_id.id[i];
    }

    return id;
}

bool attemptRegistration(ReliableMessenger &messenger, SX1276 &lora,
                         NodeConfigManager &config_manager, uint64_t device_id)
{
    Logger log("Registration");

    log.info("Attempting registration with hub...");
    log.info("Device ID: 0x%016llX", device_id);

    // Get variant info
    VariantInfo variant = getVariantInfo();

    // Send registration request and store sequence number
    uint8_t registration_seq = messenger.sendRegistrationRequest(
        ADDRESS_HUB, device_id, variant.node_type, variant.capabilities, BRAMBLE_FIRMWARE_VERSION,
        variant.variant_name);
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
                        // The registration succeeded, so we don't want it retrying in application
                        // mode
                        messenger.cancelPendingMessage(registration_seq);

                        // Save to flash with runtime-assigned fields only
                        // (node_type, capabilities, variant_name come from getVariantInfo())
                        NodeConfiguration save_config = {};
                        save_config.assigned_address = new_addr;
                        save_config.device_id = device_id;
                        save_config.firmware_version = BRAMBLE_FIRMWARE_VERSION;

                        if (config_manager.saveConfiguration(save_config)) {
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

void processIncomingMessage(uint8_t *rx_buffer, int rx_len, ReliableMessenger &messenger,
                            AddressManager *address_manager, HubRouter *hub_router,
                            uint32_t current_time, NetworkStats *network_stats, SX1276 *lora)
{
    Logger log("Message");

    // Process message with reliable messenger (handles ACKs, sensor data, etc.)
    messenger.processIncomingMessage(rx_buffer, rx_len);

    // Record statistics if available
    if (network_stats && lora && rx_len >= static_cast<int>(sizeof(MessageHeader))) {
        const MessageHeader *header = reinterpret_cast<const MessageHeader *>(rx_buffer);
        network_stats->recordMessageReceived(header->src_addr, lora->getRssi(), lora->getSnr(),
                                             false);
    }

    // If not a hub, we're done
    if (!hub_router || !address_manager || rx_len < static_cast<int>(sizeof(MessageHeader))) {
        return;
    }

    // Hub-specific processing
    const MessageHeader *header = reinterpret_cast<const MessageHeader *>(rx_buffer);
    uint16_t source_address = header->src_addr;

    // Update node activity tracking
    address_manager->updateLastSeen(source_address, current_time);
    hub_router->updateRouteOnline(source_address);

    // Handle registration requests
    if (header->type == MSG_TYPE_REGISTRATION) {
        const Message *msg = reinterpret_cast<const Message *>(rx_buffer);
        const RegistrationPayload *reg_payload =
            reinterpret_cast<const RegistrationPayload *>(msg->payload);

        // Register the node with AddressManager
        uint16_t assigned_addr = address_manager->registerNode(
            reg_payload->device_id, reg_payload->node_type, reg_payload->capabilities,
            reg_payload->firmware_ver, reg_payload->device_name);

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
        messenger.sendRegistrationResponse(header->src_addr,  // Send back to requesting node
                                           reg_payload->device_id, assigned_addr, status,
                                           30,                  // Retry interval in seconds
                                           current_time / 1000  // Network time in seconds
        );

        if (status == REG_SUCCESS) {
            log.info("Successfully registered node 0x%016llX with address 0x%04X",
                     reg_payload->device_id, assigned_addr);
        }
    }

    // Handle heartbeat messages with status logging
    if (header->type == MSG_TYPE_HEARTBEAT) {
        const HeartbeatPayload *heartbeat =
            reinterpret_cast<const HeartbeatPayload *>(rx_buffer + sizeof(MessageHeader));

        log.debug("Heartbeat from 0x%04X: uptime=%lus, battery=%u%%, signal=%u, sensors=0x%02X",
                  source_address, heartbeat->uptime_seconds, heartbeat->battery_level,
                  heartbeat->signal_strength, heartbeat->active_sensors);
    }

    // Handle CHECK_UPDATES from nodes
    if (header->type == MSG_TYPE_CHECK_UPDATES) {
        const Message *msg = reinterpret_cast<const Message *>(rx_buffer);
        const CheckUpdatesPayload *payload =
            reinterpret_cast<const CheckUpdatesPayload *>(msg->payload);

        log.debug("CHECK_UPDATES from 0x%04X (node_seq=%d)", source_address,
                  payload->node_sequence);

        hub_router->handleCheckUpdates(source_address, payload->node_sequence);
    }

    // Try to route the message if it's not for the hub
    hub_router->processMessage(rx_buffer, rx_len, source_address);
}
