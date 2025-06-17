#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "lora/sx1276.h"
#include "lora/message.h"
#include "hal/neopixel.h"

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

int64_t alarm_callback(alarm_id_t id, void *user_data) {
    // Put your timeout handler code in here
    return 0;
}




int main()
{
    stdio_init_all();
    sleep_ms(2000); // Give USB time to enumerate
    
    printf("=== Bramble Starting ===\n");

    // Initialize NeoPixel LED
    NeoPixel led(PIN_NEOPIXEL, 1);
    if (!led.begin()) {
        printf("Failed to initialize NeoPixel!\n");
    } else {
        printf("NeoPixel initialized successfully!\n");
    }

    // SPI initialisation. This example will use SPI at 1MHz.
    spi_init(SPI_PORT, 1000*1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    
    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    
    printf("System Clock Frequency is %d Hz\n", clock_get_hz(clk_sys));
    printf("USB Clock Frequency is %d Hz\n", clock_get_hz(clk_usb));
    
    // Initialize SX1276 LoRa module
    printf("Initializing SX1276 LoRa module...\n");
    SX1276 lora(SPI_PORT, PIN_CS, PIN_RST, PIN_DIO0);
    
    if (!lora.begin()) {
        printf("Failed to initialize SX1276!\n");
        while(true) {
            sleep_ms(1000);
        }
    }
    
    printf("SX1276 initialized successfully!\n");
    
    // Set higher transmit power for better signal strength
    lora.setTxPower(20);  // 20 dBm = 100mW (maximum)
    printf("Set transmit power to 20 dBm\n");
    
    printf("Starting LoRa communication test...\n");
    
    // Test message creation and transmission
    uint8_t msg_buffer[MESSAGE_MAX_SIZE];
    uint8_t test_data[] = {0x12, 0x34, 0x56, 0x78};
    uint8_t seq_num = 0;
    
    static uint32_t last_tx_time = 0;
    static bool rx_mode = true;
    
    // Start in receive mode
    lora.startReceive();
    printf("Starting in receive mode...\n");
    
    while (true) {
        // Status LED - cycle through colors during operation
        static uint8_t hue = 0;
        led.setPixelColor(0, NeoPixel::colorHSV(hue * 256, 255, 50));
        led.show();
        hue = (hue + 1) % 64;
        
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Send message every 5 seconds
        if (current_time - last_tx_time >= 5000) {
            rx_mode = false;
            
            // Create a test sensor message
            size_t msg_len = MessageHandler::createSensorMessage(
                0x0001,  // Source: Node 1
                0x0000,  // Destination: Hub
                seq_num++,
                SENSOR_TEMPERATURE,
                test_data,
                sizeof(test_data),
                msg_buffer
            );
            
            if (msg_len > 0) {
                printf("Sending message (seq=%d, len=%d)...\n", seq_num-1, msg_len);
                
                if (lora.send(msg_buffer, msg_len)) {
                    // Wait for transmission to complete
                    while (!lora.isTxDone()) {
                        sleep_ms(10);
                    }
                    printf("Message sent successfully!\n");
                } else {
                    printf("Failed to send message!\n");
                }
            }
            
            last_tx_time = current_time;
            
            // Return to receive mode
            lora.startReceive();
            rx_mode = true;
            printf("Back to receive mode\n");
        }
        
        // Check for incoming messages (only when in RX mode)
        if (rx_mode) {
            uint8_t rx_buffer[MESSAGE_MAX_SIZE];
            int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
            
            if (rx_len > 0) {
                printf("Received message (len=%d, RSSI=%d dBm, SNR=%.1f dB)\n", 
                       rx_len, lora.getRssi(), lora.getSnr());
                
                // Parse the message
                Message parsed_msg;
                if (MessageHandler::parseMessage(rx_buffer, rx_len, &parsed_msg)) {
                    printf("  From: 0x%04X, To: 0x%04X, Type: %d, Seq: %d\n",
                           parsed_msg.header.src_addr, parsed_msg.header.dst_addr,
                           parsed_msg.header.type, parsed_msg.header.seq_num);
                }
                
                // Stay in receive mode after receiving
                lora.startReceive();
            } else if (rx_len < 0) {
                printf("Receive error (CRC or buffer issue)\n");
                // Restart receive mode on error
                lora.startReceive();
            }
        }
        
        sleep_ms(100);  // Small delay for main loop
    }
}
