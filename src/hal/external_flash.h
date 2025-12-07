#pragma once

#include <stdint.h>
#include <stddef.h>
#include "logger.h"
#include "hardware/pio.h"

/**
 * @brief Pin configuration for MT25QL external flash
 *
 * Connected via GPIO (not hardware SPI), so we use PIO for SPI
 */
struct ExternalFlashPins {
    uint8_t sck;     // Clock
    uint8_t cs;      // Chip select
    uint8_t dq0;     // Data 0 (MOSI in single SPI)
    uint8_t dq1;     // Data 1 (MISO in single SPI)
    uint8_t dq2;     // Data 2 (QSPI mode)
    uint8_t dq3;     // Data 3 (QSPI mode)
    uint8_t reset;   // Reset pin
};

// Default pin configuration for Bramble board
constexpr ExternalFlashPins BRAMBLE_FLASH_PINS = {
    .sck = 8,
    .cs = 6,
    .dq0 = 10,   // MOSI
    .dq1 = 5,    // MISO
    .dq2 = 4,
    .dq3 = 9,
    .reset = 7
};

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
    constexpr uint8_t SECTOR_ERASE = 0x20;      // 4KB sector
    constexpr uint8_t BLOCK_ERASE_32K = 0x52;   // 32KB block
    constexpr uint8_t BLOCK_ERASE_64K = 0xD8;   // 64KB block
    constexpr uint8_t CHIP_ERASE = 0xC7;
    constexpr uint8_t RESET_ENABLE = 0x66;
    constexpr uint8_t RESET_DEVICE = 0x99;
    constexpr uint8_t POWER_DOWN = 0xB9;
    constexpr uint8_t RELEASE_POWER_DOWN = 0xAB;
}

/**
 * @brief Status register bits
 */
namespace MT25QLStatus {
    constexpr uint8_t BUSY = 0x01;          // Write in progress
    constexpr uint8_t WRITE_ENABLED = 0x02; // Write enable latch
}

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
 * @brief MT25QL01GBBB external flash driver using PIO-based SPI
 *
 * This driver uses PIO to bit-bang SPI since the flash is connected
 * to GPIOs that don't map to hardware SPI peripherals.
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
    static constexpr size_t SECTOR_SIZE = 4096;        // 4KB subsector
    static constexpr size_t BLOCK_SIZE_32K = 32768;
    static constexpr size_t BLOCK_SIZE_64K = 65536;
    static constexpr size_t TOTAL_SIZE = 128 * 1024 * 1024;  // 128MB

    /**
     * @brief Construct external flash driver
     * @param pins Pin configuration
     * @param pio PIO instance to use (pio0 or pio1)
     */
    ExternalFlash(const ExternalFlashPins& pins = BRAMBLE_FLASH_PINS,
                  PIO pio = pio0);

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
    ExternalFlashResult readId(uint8_t& manufacturer, uint8_t& memory_type, uint8_t& capacity);

    /**
     * @brief Read data from flash
     * @param address Flash address
     * @param buffer Output buffer
     * @param length Number of bytes to read
     * @return ExternalFlashResult
     */
    ExternalFlashResult read(uint32_t address, uint8_t* buffer, size_t length);

    /**
     * @brief Write data to flash (must be erased first)
     * @param address Flash address (should be page-aligned for best performance)
     * @param data Data to write
     * @param length Number of bytes to write
     * @return ExternalFlashResult
     */
    ExternalFlashResult write(uint32_t address, const uint8_t* data, size_t length);

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
    ExternalFlashPins pins_;
    PIO pio_;
    uint sm_;           // State machine number
    uint offset_;       // PIO program offset
    bool initialized_;
    Logger logger_;

    // Low-level SPI operations
    void csSelect();
    void csDeselect();
    void spiWrite(const uint8_t* data, size_t length);
    void spiRead(uint8_t* data, size_t length);
    void spiTransfer(const uint8_t* tx, uint8_t* rx, size_t length);

    // Send single byte
    void spiWriteByte(uint8_t byte);
    uint8_t spiReadByte();

    // Flash control
    ExternalFlashResult writeEnable();
    uint8_t readStatus();

    // Write a single page (max 256 bytes)
    ExternalFlashResult writePage(uint32_t address, const uint8_t* data, size_t length);

    // Initialize PIO for SPI
    bool initPio();
};
