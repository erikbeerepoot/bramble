#include "external_flash.h"

#include <cstring>

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

ExternalFlash::ExternalFlash(spi_inst_t *spi, const ExternalFlashPins &pins)
    : spi_(spi), pins_(pins), initialized_(false), logger_("EXTFLASH")
{
}

ExternalFlash::~ExternalFlash()
{
    if (initialized_) {
        powerDown();
    }
}

bool ExternalFlash::initGpio()
{
    // Configure CS as output, active low (directly controlled, not SPI function)
    // Set output value before enabling driver — gpio_init() clears the output
    // register to 0, so enabling the driver first causes a brief LOW glitch.
    gpio_init(pins_.cs);
    gpio_put(pins_.cs, 1);              // Deselect (before enabling driver)
    gpio_set_dir(pins_.cs, GPIO_OUT);

    // Configure reset as output, active low
    gpio_init(pins_.reset);
    gpio_put(pins_.reset, 1);           // Not in reset (before enabling driver)
    gpio_set_dir(pins_.reset, GPIO_OUT);

    return true;
}

bool ExternalFlash::init()
{
    logger_.debug("Initializing external flash (hardware SPI)...");
    logger_.debug("  Pins: CS=%d, RST=%d ", pins_.cs, pins_.reset);

    if (!initGpio()) {
        logger_.error("Failed to initialize GPIO");
        return false;
    }

    // Wait for flash power-on reset to complete before any SPI commands.
    // MT25QL tVSL (VCC min to first command) can be up to 100ms with slow
    // supply ramp from DCDC. Empirically needs ~155ms on V4 board.
    sleep_ms(150);

    constexpr int MAX_ATTEMPTS = 3;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            uint32_t delay_ms = 40 * attempt;
            logger_.debug("Waiting %lu ms before retry", delay_ms);
            sleep_ms(delay_ms);
        }

        // Abort any stuck SPI command from a prior interrupted transaction.
        // The flash may be mid-command (waiting for more clocks), so toggling
        // CS high forces it to deselect and abandon the current operation.
        csDeselect();
        sleep_us(10);

        // Software reset clears internal state machines even if the flash is
        // stuck in a write or erase operation that survived the power cycle.
        reset();

        // Hardware reset brings flash to normal operating mode (also exits
        // deep power-down). Do NOT send wakeUp() (0xAB) here — after reset
        // the flash isn't in power-down, and 0xAB triggers "Read Electronic
        // Signature" mode which can desync the SPI protocol.
        hardwareReset();

        // Wait for any in-progress operation (e.g. interrupted erase) to complete
        waitReady(100);

        // Read and verify JEDEC ID
        uint8_t manufacturer, memory_type, capacity;
        if (readId(manufacturer, memory_type, capacity) != ExternalFlashResult::Success) {
            logger_.error("Failed to read flash ID (attempt %d/%d)", attempt + 1, MAX_ATTEMPTS);
            continue;
        }

        logger_.debug("Flash ID: Manufacturer=0x%02X, Type=0x%02X, Capacity=0x%02X", manufacturer,
                      memory_type, capacity);

        // If manufacturer ID is 0x00, flash is unresponsive - retry
        if (manufacturer == 0x00) {
            logger_.warn("Flash unresponsive (manufacturer=0x00), retrying (%d/%d)", attempt + 1,
                         MAX_ATTEMPTS);
            continue;
        }

        // Verify it's a Micron flash (manufacturer ID 0x20)
        if (manufacturer != 0x20) {
            logger_.warn("Unexpected manufacturer ID (expected 0x20 for Micron)");
            // Continue anyway - might still work
        }

        initialized_ = true;
        logger_.info("External flash initialized successfully");
        return true;
    }

    logger_.error("External flash init failed after %d attempts", MAX_ATTEMPTS);
    return false;
}

void ExternalFlash::hardwareReset()
{
    gpio_put(pins_.reset, 0);
    sleep_ms(1);  // Conservative pulse width (MT25QL spec: ~10us)
    gpio_put(pins_.reset, 1);
    sleep_ms(50);  // Device recovery time (tPUW max 10ms for MT25QL; use 50ms for margin)
}

void ExternalFlash::drainSpiFifo()
{
    while (spi_is_readable(spi_)) {
        (void)spi_get_hw(spi_)->dr;
    }
}

void ExternalFlash::csSelect()
{
    drainSpiFifo();
    gpio_put(pins_.cs, 0);
    // Small delay for CS setup time
    __asm volatile("nop\nnop\nnop\nnop");
}

void ExternalFlash::csDeselect()
{
    // Small delay for CS hold time (tCHSH)
    __asm volatile("nop\nnop\nnop\nnop");
    gpio_put(pins_.cs, 1);
    // CS high time (tSHSL) - MT25QL requires >= 30-50ns.
    sleep_us(1);
}

void ExternalFlash::spiWriteByte(uint8_t byte)
{
    spi_write_blocking(spi_, &byte, 1);
}

uint8_t ExternalFlash::spiReadByte()
{
    uint8_t byte;
    spi_read_blocking(spi_, 0xFF, &byte, 1);
    return byte;
}

void ExternalFlash::spiWrite(const uint8_t *data, size_t length)
{
    spi_write_blocking(spi_, data, length);
}

void ExternalFlash::spiRead(uint8_t *data, size_t length)
{
    spi_read_blocking(spi_, 0xFF, data, length);
}

ExternalFlashResult ExternalFlash::readId(uint8_t &manufacturer, uint8_t &memory_type,
                                          uint8_t &capacity)
{
    csSelect();

    spiWriteByte(MT25QLCommands::READ_ID);
    manufacturer = spiReadByte();
    memory_type = spiReadByte();
    capacity = spiReadByte();

    csDeselect();

    return ExternalFlashResult::Success;
}

uint8_t ExternalFlash::readStatus()
{
    csSelect();
    spiWriteByte(MT25QLCommands::READ_STATUS);
    uint8_t status = spiReadByte();
    csDeselect();
    return status;
}

bool ExternalFlash::isBusy()
{
    return (readStatus() & MT25QLStatus::BUSY) != 0;
}

ExternalFlashResult ExternalFlash::waitReady(uint32_t timeout_ms)
{
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

ExternalFlashResult ExternalFlash::writeEnable()
{
    constexpr int MAX_ATTEMPTS = 3;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            sleep_us(100 * attempt);  // Escalating delay between retries
        }

        drainSpiFifo();

        csSelect();
        spiWriteByte(MT25QLCommands::WRITE_ENABLE);
        csDeselect();

        // Allow flash to process WREN and update status register.
        sleep_us(10);

        uint8_t status = readStatus();
        if (status & MT25QLStatus::WRITE_ENABLED) {
            if (attempt > 0) {
                logger_.debug("Write enable succeeded on attempt %d", attempt + 1);
            }
            return ExternalFlashResult::Success;
        }

        if (attempt == 0) {
            logger_.error("Write enable failed (status=0x%02X)", status);
            if (status == 0xFF || status == 0x00) {
                logger_.error("  -> Flash not responding (SPI bus issue or not powered)");
            } else if (status & 0x01) {
                logger_.error("  -> Flash busy with previous operation, WREN ignored");
            } else if (status & 0x3C) {
                logger_.error("  -> Block protection enabled (BP=0x%02X), regions may be locked",
                              (status >> 2) & 0x0F);
            } else {
                logger_.error("  -> WREN command not received");
            }
        }
    }

    logger_.error("Write enable failed after %d attempts", MAX_ATTEMPTS);
    return ExternalFlashResult::ErrorHardware;
}

ExternalFlashResult ExternalFlash::read(uint32_t address, uint8_t *buffer, size_t length)
{
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

ExternalFlashResult ExternalFlash::writePage(uint32_t address, const uint8_t *data, size_t length)
{
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

    // Allow flash to begin internal program operation before polling status.
    // Without this delay, the first status read can race with the flash's
    // internal state transition, returning stale (not-busy) status.
    sleep_us(50);

    // Wait for program to complete (typical 0.7ms, max 5ms per page)
    return waitReady(10);
}

ExternalFlashResult ExternalFlash::write(uint32_t address, const uint8_t *data, size_t length)
{
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

ExternalFlashResult ExternalFlash::eraseSector(uint32_t address)
{
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

    // Allow flash to begin internal erase operation before polling status
    sleep_us(50);

    // Wait for erase to complete (typical 50ms, max 400ms)
    return waitReady(500);
}

ExternalFlashResult ExternalFlash::eraseBlock(uint32_t address)
{
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

    // Allow flash to begin internal erase operation before polling status
    sleep_us(50);

    // Wait for erase to complete (typical 150ms, max 2000ms)
    return waitReady(3000);
}

ExternalFlashResult ExternalFlash::eraseChip()
{
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

    // Allow flash to begin internal erase operation before polling status
    sleep_us(50);

    // Wait for chip erase (can take 4+ minutes for 1Gbit!)
    return waitReady(300000);  // 5 minute timeout
}

void ExternalFlash::powerDown()
{
    csSelect();
    spiWriteByte(MT25QLCommands::POWER_DOWN);
    csDeselect();
    // tDP: time to enter deep power-down (~3us typical)
    sleep_us(10);
}

void ExternalFlash::wakeUp()
{
    csSelect();
    spiWriteByte(MT25QLCommands::RELEASE_POWER_DOWN);
    csDeselect();
    // tRES1: time to exit deep power-down (~3-30us typical depending on variant)
    sleep_us(50);
}

void ExternalFlash::reset()
{
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
