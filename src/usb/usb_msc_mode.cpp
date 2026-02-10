#include "usb_msc_mode.h"

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

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

void enter_usb_msc_mode(NeoPixel &led)
{
    // Purple LED = USB MSC mode
    led.setPixel(0, 128, 0, 255);
    led.show();

    // Initialize SPI1 for external flash access
    spi_init(spi1, 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Initialize external flash
    ExternalFlash flash;
    if (!flash.init()) {
        // Red blink = flash init failed
        while (true) {
            led.setPixel(0, 255, 0, 0);
            led.show();
            sleep_ms(200);
            led.setPixel(0, 0, 0, 0);
            led.show();
            sleep_ms(200);
        }
    }

    // Initialize log flash buffer
    LogFlashBuffer log_buffer(flash);
    if (!log_buffer.init()) {
        // Orange blink = log buffer init failed
        while (true) {
            led.setPixel(0, 255, 100, 0);
            led.show();
            sleep_ms(200);
            led.setPixel(0, 0, 0, 0);
            led.show();
            sleep_ms(200);
        }
    }

    // Set up the MSC disk backend
    msc_disk_init(&log_buffer, &flash);

    // Initialize TinyUSB
    tusb_init();

    // Solid purple = ready
    led.setPixel(0, 128, 0, 255);
    led.show();

    // Main loop - just service USB
    while (true) {
        tud_task();
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
