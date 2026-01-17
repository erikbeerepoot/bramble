#include "external_flash.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <cstring>

ExternalFlash::ExternalFlash(spi_inst_t* spi, const ExternalFlashPins& pins)
    : spi_(spi)
    , pins_(pins)
    , initialized_(false)
    , logger_("EXTFLASH")
{
}

ExternalFlash::~ExternalFlash() {
    if (initialized_) {
        powerDown();
    }
}

bool ExternalFlash::initGpio() {
    // Configure CS as output, active low (directly controlled, not SPI function)
    gpio_init(pins_.cs);
    gpio_set_dir(pins_.cs, GPIO_OUT);
    gpio_put(pins_.cs, 1);  // Deselect

    // Configure reset as output, active low
    gpio_init(pins_.reset);
    gpio_set_dir(pins_.reset, GPIO_OUT);
    gpio_put(pins_.reset, 1);  // Not in reset

    return true;
}

bool ExternalFlash::init() {
    logger_.info("Initializing external flash (hardware SPI)...");
    logger_.info("  Pins: CS=%d, RST=%d (SPI shared with LoRa)", pins_.cs, pins_.reset);

    if (!initGpio()) {
        logger_.error("Failed to initialize GPIO");
        return false;
    }

    // Reset the flash
    reset();

    // Small delay after reset
    sleep_ms(10);

    // Wake up in case it's in power-down mode
    wakeUp();
    sleep_us(50);  // tRES1 = 3us typical

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
    spi_write_blocking(spi_, &byte, 1);
}

uint8_t ExternalFlash::spiReadByte() {
    uint8_t byte;
    spi_read_blocking(spi_, 0xFF, &byte, 1);
    return byte;
}

void ExternalFlash::spiWrite(const uint8_t* data, size_t length) {
    spi_write_blocking(spi_, data, length);
}

void ExternalFlash::spiRead(uint8_t* data, size_t length) {
    spi_read_blocking(spi_, 0xFF, data, length);
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
