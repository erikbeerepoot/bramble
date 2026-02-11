#include "usb_msc_mode.h"

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

#include "../hal/external_flash.h"
#include "../hal/neopixel.h"
#include "../storage/log_flash_buffer.h"
#include "msc_disk.h"
#include "tusb.h"

// SPI1 pins (shared with LoRa, but LoRa not active in MSC mode)
static constexpr uint PIN_MISO = 8;
static constexpr uint PIN_SCK = 14;
static constexpr uint PIN_MOSI = 15;
static constexpr uint PIN_NEOPIXEL = 4;

// PMU UART configuration
static constexpr uint PMU_UART_TX_PIN = 0;
static constexpr uint PMU_UART_RX_PIN = 1;
static constexpr uint PMU_UART_BAUD = 9600;

// PMU protocol constants
static constexpr uint8_t PMU_START_BYTE = 0xAA;
static constexpr uint8_t PMU_END_BYTE = 0x55;
static constexpr uint8_t PMU_CMD_KEEP_AWAKE = 0x15;
static constexpr uint8_t PMU_CMD_CLEAR_TO_SEND = 0x19;

// Keep-awake interval (send every 60 seconds to stay awake indefinitely)
static constexpr uint32_t KEEP_AWAKE_INTERVAL_MS = 60000;
static constexpr uint16_t KEEP_AWAKE_SECONDS = 120;  // Request 2 minutes each time

static uint8_t pmu_seq_num = 1;

/**
 * @brief Send a PMU protocol message
 */
static void pmu_send_message(uint8_t command, const uint8_t *data, uint8_t data_len)
{
    uint8_t msg[16];
    uint8_t idx = 0;

    msg[idx++] = PMU_START_BYTE;
    msg[idx++] = 1 + 1 + data_len;  // length = seq + cmd + data
    msg[idx++] = pmu_seq_num++;

    // Wrap sequence number in valid range (1-127)
    if (pmu_seq_num > 127) {
        pmu_seq_num = 1;
    }

    msg[idx++] = command;

    for (uint8_t i = 0; i < data_len; i++) {
        msg[idx++] = data[i];
    }

    // Calculate checksum (XOR of length through data)
    uint8_t checksum = 0;
    for (uint8_t i = 1; i < idx; i++) {
        checksum ^= msg[i];
    }
    msg[idx++] = checksum;
    msg[idx++] = PMU_END_BYTE;

    uart_write_blocking(uart0, msg, idx);
}

/**
 * @brief Send KeepAwake command to PMU
 */
static void pmu_send_keep_awake(uint16_t seconds)
{
    uint8_t data[2] = {static_cast<uint8_t>(seconds & 0xFF),
                       static_cast<uint8_t>((seconds >> 8) & 0xFF)};
    pmu_send_message(PMU_CMD_KEEP_AWAKE, data, 2);
}

/**
 * @brief Send ClearToSend command to PMU (signals we're ready)
 */
static void pmu_send_clear_to_send()
{
    pmu_send_message(PMU_CMD_CLEAR_TO_SEND, nullptr, 0);
}

void enter_usb_msc_mode(NeoPixel &led)
{
    // Initialize PMU UART first so we can send ClearToSend quickly
    uart_init(uart0, PMU_UART_BAUD);
    gpio_set_function(PMU_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PMU_UART_RX_PIN, GPIO_FUNC_UART);

    // Send ClearToSend to PMU so it knows we're awake
    pmu_send_clear_to_send();

    // Send initial KeepAwake to prevent early sleep
    pmu_send_keep_awake(KEEP_AWAKE_SECONDS);

    // Initialize TinyUSB - USB host expects quick enumeration
    tusb_init();

    // Purple LED = USB MSC mode (initializing)
    led.setPixel(0, 128, 0, 255);
    led.show();

    // Service USB while doing slow flash init
    for (int i = 0; i < 10; i++) {
        tud_task();
        sleep_ms(1);
    }

    // Initialize SPI1 for external flash access
    spi_init(spi1, 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Service USB periodically during flash init
    tud_task();

    // Initialize external flash
    ExternalFlash flash;
    if (!flash.init()) {
        // Red blink = flash init failed
        while (true) {
            tud_task();
            led.setPixel(0, 255, 0, 0);
            led.show();
            sleep_ms(200);
            tud_task();
            led.setPixel(0, 0, 0, 0);
            led.show();
            sleep_ms(200);
        }
    }

    tud_task();

    // Initialize log flash buffer
    LogFlashBuffer log_buffer(flash);
    if (!log_buffer.init()) {
        // Orange blink = log buffer init failed
        while (true) {
            tud_task();
            led.setPixel(0, 255, 100, 0);
            led.show();
            sleep_ms(200);
            tud_task();
            led.setPixel(0, 0, 0, 0);
            led.show();
            sleep_ms(200);
        }
    }

    tud_task();

    // Set up the MSC disk backend
    msc_disk_init(&log_buffer, &flash);

    // Solid purple = ready
    led.setPixel(0, 128, 0, 255);
    led.show();

    // Main loop - service USB and send periodic KeepAwake to PMU
    uint32_t last_keep_awake = to_ms_since_boot(get_absolute_time());

    while (true) {
        tud_task();

        // Send KeepAwake periodically to prevent PMU from sleeping
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_keep_awake >= KEEP_AWAKE_INTERVAL_MS) {
            pmu_send_keep_awake(KEEP_AWAKE_SECONDS);
            last_keep_awake = now;
        }
    }
}

/**
 * @brief Standalone entry point for USB MSC log retrieval firmware
 *
 * Flash this binary to download logs. Re-flash normal firmware when done.
 */
int main()
{
    // Minimal stdlib init (no USB stdio - we handle USB ourselves)
    stdio_init_all();

    NeoPixel led(PIN_NEOPIXEL, 1);
    enter_usb_msc_mode(led);

    return 0;  // Never reached
}
