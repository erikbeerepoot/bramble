#include "external_flash.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <cstring>

// Simple bit-banged SPI using PIO
// We'll use a minimal approach first - can optimize with proper PIO program later

ExternalFlash::ExternalFlash(const ExternalFlashPins& pins, PIO pio)
    : pins_(pins)
    , pio_(pio)
    , sm_(0)
    , offset_(0)
    , initialized_(false)
    , logger_("EXTFLASH")
{
}

ExternalFlash::~ExternalFlash() {
    if (initialized_) {
        powerDown();
    }
}

bool ExternalFlash::initPio() {
    // For simplicity, we'll bit-bang SPI using GPIO
    // This is slower but works on any pins
    // Can be optimized with PIO later if needed

    // Configure CS as output, active low
    gpio_init(pins_.cs);
    gpio_set_dir(pins_.cs, GPIO_OUT);
    gpio_put(pins_.cs, 1);  // Deselect

    // Configure SCK as output
    gpio_init(pins_.sck);
    gpio_set_dir(pins_.sck, GPIO_OUT);
    gpio_put(pins_.sck, 0);  // Clock idle low (SPI mode 0)

    // Configure MOSI (DQ0) as output
    gpio_init(pins_.dq0);
    gpio_set_dir(pins_.dq0, GPIO_OUT);
    gpio_put(pins_.dq0, 0);

    // Configure MISO (DQ1) as input
    gpio_init(pins_.dq1);
    gpio_set_dir(pins_.dq1, GPIO_IN);

    // Configure reset as output, active low
    gpio_init(pins_.reset);
    gpio_set_dir(pins_.reset, GPIO_OUT);
    gpio_put(pins_.reset, 1);  // Not in reset

    // DQ2 and DQ3 configured as outputs for now (hold/WP disabled)
    gpio_init(pins_.dq2);
    gpio_set_dir(pins_.dq2, GPIO_OUT);
    gpio_put(pins_.dq2, 1);  // WP# high (write protect disabled)

    gpio_init(pins_.dq3);
    gpio_set_dir(pins_.dq3, GPIO_OUT);
    gpio_put(pins_.dq3, 1);  // HOLD# high (hold disabled)

    return true;
}

bool ExternalFlash::init() {
    logger_.info("Initializing external flash...");
    logger_.info("  Pins: SCK=%d, CS=%d, DQ0=%d, DQ1=%d, DQ2=%d, DQ3=%d, RST=%d",
                 pins_.sck, pins_.cs, pins_.dq0, pins_.dq1, pins_.dq2, pins_.dq3, pins_.reset);

    if (!initPio()) {
        logger_.error("Failed to initialize PIO");
        return false;
    }

    // Debug: verify GPIO state
    logger_.info("  GPIO state after init: CS=%d, SCK=%d, DQ1(MISO)=%d",
                 gpio_get(pins_.cs), gpio_get(pins_.sck), gpio_get(pins_.dq1));

    // Reset the flash
    reset();

    // Small delay after reset
    sleep_ms(10);

    // Wake up in case it's in power-down mode
    wakeUp();
    sleep_us(50);  // tRES1 = 3us typical

    // Debug: check MISO state
    logger_.info("  DQ1(MISO) state after wake: %d", gpio_get(pins_.dq1));

    // Read and verify JEDEC ID
    uint8_t manufacturer, memory_type, capacity;
    if (readId(manufacturer, memory_type, capacity) != ExternalFlashResult::Success) {
        logger_.error("Failed to read flash ID");
        return false;
    }

    logger_.info("Flash ID: Manufacturer=0x%02X, Type=0x%02X, Capacity=0x%02X",
                 manufacturer, memory_type, capacity);

    // Verify it's a Micron flash (manufacturer ID 0x20)
    if (manufacturer != 0x20) {
        logger_.warn("Unexpected manufacturer ID (expected 0x20 for Micron)");
        // Continue anyway - might still work
    }

    initialized_ = true;
    logger_.info("External flash initialized successfully");
    return true;
}

void ExternalFlash::csSelect() {
    gpio_put(pins_.cs, 0);
    // Small delay for CS setup time
    __asm volatile("nop\nnop\nnop\nnop");
}

void ExternalFlash::csDeselect() {
    // Small delay for CS hold time
    __asm volatile("nop\nnop\nnop\nnop");
    gpio_put(pins_.cs, 1);
    // CS high time before next select
    __asm volatile("nop\nnop\nnop\nnop");
}

void ExternalFlash::spiWriteByte(uint8_t byte) {
    // Bit-bang SPI mode 0: CPOL=0, CPHA=0
    // Data sampled on rising edge, shifted on falling edge
    for (int i = 7; i >= 0; i--) {
        // Set MOSI
        gpio_put(pins_.dq0, (byte >> i) & 1);

        // Small delay
        __asm volatile("nop\nnop");

        // Rising edge - data sampled by slave
        gpio_put(pins_.sck, 1);

        // Small delay
        __asm volatile("nop\nnop\nnop\nnop");

        // Falling edge
        gpio_put(pins_.sck, 0);

        // Small delay
        __asm volatile("nop\nnop");
    }
}

uint8_t ExternalFlash::spiReadByte() {
    uint8_t byte = 0;

    for (int i = 7; i >= 0; i--) {
        // Rising edge
        gpio_put(pins_.sck, 1);

        // Small delay before sampling
        __asm volatile("nop\nnop\nnop\nnop");

        // Sample MISO
        if (gpio_get(pins_.dq1)) {
            byte |= (1 << i);
        }

        // Falling edge
        gpio_put(pins_.sck, 0);

        // Small delay
        __asm volatile("nop\nnop");
    }

    return byte;
}

void ExternalFlash::spiWrite(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        spiWriteByte(data[i]);
    }
}

void ExternalFlash::spiRead(uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        data[i] = spiReadByte();
    }
}

void ExternalFlash::spiTransfer(const uint8_t* tx, uint8_t* rx, size_t length) {
    for (size_t i = 0; i < length; i++) {
        uint8_t tx_byte = tx ? tx[i] : 0xFF;
        uint8_t rx_byte = 0;

        for (int bit = 7; bit >= 0; bit--) {
            // Set MOSI
            gpio_put(pins_.dq0, (tx_byte >> bit) & 1);
            __asm volatile("nop\nnop");

            // Rising edge
            gpio_put(pins_.sck, 1);
            __asm volatile("nop\nnop\nnop\nnop");

            // Sample MISO
            if (gpio_get(pins_.dq1)) {
                rx_byte |= (1 << bit);
            }

            // Falling edge
            gpio_put(pins_.sck, 0);
            __asm volatile("nop\nnop");
        }

        if (rx) {
            rx[i] = rx_byte;
        }
    }
}

ExternalFlashResult ExternalFlash::readId(uint8_t& manufacturer, uint8_t& memory_type, uint8_t& capacity) {
    csSelect();

    spiWriteByte(MT25QLCommands::READ_ID);
    manufacturer = spiReadByte();
    memory_type = spiReadByte();
    capacity = spiReadByte();

    csDeselect();

    return ExternalFlashResult::Success;
}

uint8_t ExternalFlash::readStatus() {
    csSelect();
    spiWriteByte(MT25QLCommands::READ_STATUS);
    uint8_t status = spiReadByte();
    csDeselect();
    return status;
}

bool ExternalFlash::isBusy() {
    return (readStatus() & MT25QLStatus::BUSY) != 0;
}

ExternalFlashResult ExternalFlash::waitReady(uint32_t timeout_ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (isBusy()) {
        if ((to_ms_since_boot(get_absolute_time()) - start) > timeout_ms) {
            logger_.error("Timeout waiting for flash ready");
            return ExternalFlashResult::ErrorTimeout;
        }
        sleep_us(100);
    }

    return ExternalFlashResult::Success;
}

ExternalFlashResult ExternalFlash::writeEnable() {
    csSelect();
    spiWriteByte(MT25QLCommands::WRITE_ENABLE);
    csDeselect();

    // Verify write enable latch is set
    uint8_t status = readStatus();
    if (!(status & MT25QLStatus::WRITE_ENABLED)) {
        logger_.error("Write enable failed");
        return ExternalFlashResult::ErrorHardware;
    }

    return ExternalFlashResult::Success;
}

ExternalFlashResult ExternalFlash::read(uint32_t address, uint8_t* buffer, size_t length) {
    if (!initialized_) {
        return ExternalFlashResult::ErrorNotInitialized;
    }

    if (!buffer || length == 0) {
        return ExternalFlashResult::ErrorInvalidParam;
    }

    if (address + length > TOTAL_SIZE) {
        return ExternalFlashResult::ErrorInvalidParam;
    }

    // Wait for any previous operation
    ExternalFlashResult result = waitReady(100);
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    csSelect();

    // Send read command with 24-bit address
    spiWriteByte(MT25QLCommands::READ_DATA);
    spiWriteByte((address >> 16) & 0xFF);
    spiWriteByte((address >> 8) & 0xFF);
    spiWriteByte(address & 0xFF);

    // Read data
    spiRead(buffer, length);

    csDeselect();

    return ExternalFlashResult::Success;
}

ExternalFlashResult ExternalFlash::writePage(uint32_t address, const uint8_t* data, size_t length) {
    if (length > PAGE_SIZE) {
        length = PAGE_SIZE;
    }

    // Enable write
    ExternalFlashResult result = writeEnable();
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    csSelect();

    // Send page program command with 24-bit address
    spiWriteByte(MT25QLCommands::PAGE_PROGRAM);
    spiWriteByte((address >> 16) & 0xFF);
    spiWriteByte((address >> 8) & 0xFF);
    spiWriteByte(address & 0xFF);

    // Write data
    spiWrite(data, length);

    csDeselect();

    // Wait for program to complete (typical 0.7ms, max 5ms per page)
    return waitReady(10);
}

ExternalFlashResult ExternalFlash::write(uint32_t address, const uint8_t* data, size_t length) {
    if (!initialized_) {
        return ExternalFlashResult::ErrorNotInitialized;
    }

    if (!data || length == 0) {
        return ExternalFlashResult::ErrorInvalidParam;
    }

    if (address + length > TOTAL_SIZE) {
        return ExternalFlashResult::ErrorInvalidParam;
    }

    size_t written = 0;

    while (written < length) {
        // Calculate how much we can write in this page
        size_t page_offset = (address + written) % PAGE_SIZE;
        size_t page_remaining = PAGE_SIZE - page_offset;
        size_t to_write = length - written;
        if (to_write > page_remaining) {
            to_write = page_remaining;
        }

        ExternalFlashResult result = writePage(address + written, data + written, to_write);
        if (result != ExternalFlashResult::Success) {
            logger_.error("Page write failed at address 0x%08lX", address + written);
            return result;
        }

        written += to_write;
    }

    return ExternalFlashResult::Success;
}

ExternalFlashResult ExternalFlash::eraseSector(uint32_t address) {
    if (!initialized_) {
        return ExternalFlashResult::ErrorNotInitialized;
    }

    if (address >= TOTAL_SIZE) {
        return ExternalFlashResult::ErrorInvalidParam;
    }

    // Wait for ready
    ExternalFlashResult result = waitReady(100);
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    // Enable write
    result = writeEnable();
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    csSelect();

    // Send sector erase command with 24-bit address
    spiWriteByte(MT25QLCommands::SECTOR_ERASE);
    spiWriteByte((address >> 16) & 0xFF);
    spiWriteByte((address >> 8) & 0xFF);
    spiWriteByte(address & 0xFF);

    csDeselect();

    // Wait for erase to complete (typical 50ms, max 400ms)
    return waitReady(500);
}

ExternalFlashResult ExternalFlash::eraseBlock(uint32_t address) {
    if (!initialized_) {
        return ExternalFlashResult::ErrorNotInitialized;
    }

    if (address >= TOTAL_SIZE) {
        return ExternalFlashResult::ErrorInvalidParam;
    }

    // Wait for ready
    ExternalFlashResult result = waitReady(100);
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    // Enable write
    result = writeEnable();
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    csSelect();

    // Send 64KB block erase command with 24-bit address
    spiWriteByte(MT25QLCommands::BLOCK_ERASE_64K);
    spiWriteByte((address >> 16) & 0xFF);
    spiWriteByte((address >> 8) & 0xFF);
    spiWriteByte(address & 0xFF);

    csDeselect();

    // Wait for erase to complete (typical 150ms, max 2000ms)
    return waitReady(3000);
}

ExternalFlashResult ExternalFlash::eraseChip() {
    if (!initialized_) {
        return ExternalFlashResult::ErrorNotInitialized;
    }

    logger_.warn("Chip erase requested - this will take several minutes!");

    // Wait for ready
    ExternalFlashResult result = waitReady(100);
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    // Enable write
    result = writeEnable();
    if (result != ExternalFlashResult::Success) {
        return result;
    }

    csSelect();
    spiWriteByte(MT25QLCommands::CHIP_ERASE);
    csDeselect();

    // Wait for chip erase (can take 4+ minutes for 1Gbit!)
    return waitReady(300000);  // 5 minute timeout
}

void ExternalFlash::powerDown() {
    csSelect();
    spiWriteByte(MT25QLCommands::POWER_DOWN);
    csDeselect();
}

void ExternalFlash::wakeUp() {
    csSelect();
    spiWriteByte(MT25QLCommands::RELEASE_POWER_DOWN);
    csDeselect();
}

void ExternalFlash::reset() {
    // Software reset sequence
    csSelect();
    spiWriteByte(MT25QLCommands::RESET_ENABLE);
    csDeselect();

    sleep_us(1);

    csSelect();
    spiWriteByte(MT25QLCommands::RESET_DEVICE);
    csDeselect();

    // Wait for reset to complete (tRST = 30us typical)
    sleep_us(50);
}
