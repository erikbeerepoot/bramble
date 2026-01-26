#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hardware/spi.h"

#include "logger.h"

/**
 * @brief Pin configuration for MT25QL external flash
 *
 * Flash shares SPI1 bus with LoRa module (MISO=GPIO8, SCK=GPIO14, MOSI=GPIO15).
 * Only CS and RESET pins are flash-specific.
 */
struct ExternalFlashPins {
    uint8_t cs;     // Chip select (GPIO6)
    uint8_t reset;  // Reset pin (GPIO7)
};

// Default pin configuration for Bramble board v3
// Flash shares SPI1 with LoRa, so only CS and RST are specified here
constexpr ExternalFlashPins BRAMBLE_FLASH_PINS = {.cs = 6, .reset = 7};

/**
 * @brief MT25QL flash commands
 */
namespace MT25QLCommands {
constexpr uint8_t READ_ID = 0x9F;
constexpr uint8_t READ_STATUS = 0x05;
constexpr uint8_t WRITE_ENABLE = 0x06;
constexpr uint8_t WRITE_DISABLE = 0x04;
constexpr uint8_t READ_DATA = 0x03;
constexpr uint8_t FAST_READ = 0x0B;
constexpr uint8_t PAGE_PROGRAM = 0x02;
constexpr uint8_t SECTOR_ERASE = 0x20;     // 4KB sector
constexpr uint8_t BLOCK_ERASE_32K = 0x52;  // 32KB block
constexpr uint8_t BLOCK_ERASE_64K = 0xD8;  // 64KB block
constexpr uint8_t CHIP_ERASE = 0xC7;
constexpr uint8_t RESET_ENABLE = 0x66;
constexpr uint8_t RESET_DEVICE = 0x99;
constexpr uint8_t POWER_DOWN = 0xB9;
constexpr uint8_t RELEASE_POWER_DOWN = 0xAB;
}  // namespace MT25QLCommands

/**
 * @brief Status register bits
 */
namespace MT25QLStatus {
constexpr uint8_t BUSY = 0x01;           // Write in progress
constexpr uint8_t WRITE_ENABLED = 0x02;  // Write enable latch
}  // namespace MT25QLStatus

/**
 * @brief Flash operation result codes
 */
enum class ExternalFlashResult : uint8_t {
    Success = 0,
    ErrorTimeout,
    ErrorBusy,
    ErrorInvalidParam,
    ErrorVerifyFailed,
    ErrorNotInitialized,
    ErrorHardware
};

/**
 * @brief MT25QL01GBBB external flash driver using hardware SPI
 *
 * This driver uses hardware SPI1 (shared with LoRa module).
 * SPI must be initialized before using this driver (done in main.cpp).
 *
 * Flash specs:
 * - Size: 1Gbit (128MB)
 * - Page size: 256 bytes
 * - Sector size: 4KB (subsector), 32KB, 64KB
 * - Max SPI clock: 133MHz
 */
class ExternalFlash {
public:
    // Flash constants
    static constexpr size_t PAGE_SIZE = 256;
    static constexpr size_t SECTOR_SIZE = 4096;  // 4KB subsector
    static constexpr size_t BLOCK_SIZE_32K = 32768;
    static constexpr size_t BLOCK_SIZE_64K = 65536;
    static constexpr size_t TOTAL_SIZE = 128 * 1024 * 1024;  // 128MB

    /**
     * @brief Construct external flash driver
     * @param spi SPI instance to use (must be already initialized)
     * @param pins Pin configuration (CS and RESET only)
     */
    ExternalFlash(spi_inst_t *spi = spi1, const ExternalFlashPins &pins = BRAMBLE_FLASH_PINS);

    ~ExternalFlash();

    /**
     * @brief Initialize the flash driver and verify communication
     * @return true if flash responds correctly
     */
    bool init();

    /**
     * @brief Read the JEDEC ID (manufacturer + device ID)
     * @param manufacturer Output: manufacturer ID (should be 0x20 for Micron)
     * @param memory_type Output: memory type
     * @param capacity Output: capacity code
     * @return ExternalFlashResult
     */
    ExternalFlashResult readId(uint8_t &manufacturer, uint8_t &memory_type, uint8_t &capacity);

    /**
     * @brief Read data from flash
     * @param address Flash address
     * @param buffer Output buffer
     * @param length Number of bytes to read
     * @return ExternalFlashResult
     */
    ExternalFlashResult read(uint32_t address, uint8_t *buffer, size_t length);

    /**
     * @brief Write data to flash (must be erased first)
     * @param address Flash address (should be page-aligned for best performance)
     * @param data Data to write
     * @param length Number of bytes to write
     * @return ExternalFlashResult
     */
    ExternalFlashResult write(uint32_t address, const uint8_t *data, size_t length);

    /**
     * @brief Erase a 4KB sector
     * @param address Address within sector to erase
     * @return ExternalFlashResult
     */
    ExternalFlashResult eraseSector(uint32_t address);

    /**
     * @brief Erase a 64KB block
     * @param address Address within block to erase
     * @return ExternalFlashResult
     */
    ExternalFlashResult eraseBlock(uint32_t address);

    /**
     * @brief Erase entire chip (takes a long time!)
     * @return ExternalFlashResult
     */
    ExternalFlashResult eraseChip();

    /**
     * @brief Check if flash is busy
     * @return true if busy
     */
    bool isBusy();

    /**
     * @brief Wait for flash to be ready
     * @param timeout_ms Timeout in milliseconds
     * @return ExternalFlashResult
     */
    ExternalFlashResult waitReady(uint32_t timeout_ms = 1000);

    /**
     * @brief Put flash into low-power mode
     */
    void powerDown();

    /**
     * @brief Wake flash from low-power mode
     */
    void wakeUp();

    /**
     * @brief Reset the flash device
     */
    void reset();

    /**
     * @brief Check if driver is initialized
     */
    bool isInitialized() const { return initialized_; }

private:
    spi_inst_t *spi_;
    ExternalFlashPins pins_;
    bool initialized_;
    Logger logger_;

    // Chip select control
    void csSelect();
    void csDeselect();

    // SPI operations using hardware SPI
    void spiWrite(const uint8_t *data, size_t length);
    void spiRead(uint8_t *data, size_t length);
    void spiWriteByte(uint8_t byte);
    uint8_t spiReadByte();

    // Flash control
    ExternalFlashResult writeEnable();
    uint8_t readStatus();

    // Write a single page (max 256 bytes)
    ExternalFlashResult writePage(uint32_t address, const uint8_t *data, size_t length);

    // Initialize GPIO pins (CS and RESET only)
    bool initGpio();

    // Hardware reset via GPIO reset pin
    void hardwareReset();
};
