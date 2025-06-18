#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Hardware Abstraction Layer for RP2040 flash memory operations
 * 
 * Provides low-level flash read/write/erase operations
 * Flash memory layout:
 * - RP2040 has 2MB flash by default (varies by board)
 * - Sector size: 4096 bytes
 * - Page size: 256 bytes
 * - Minimum erase size: 1 sector (4096 bytes)
 * - Minimum write size: 1 page (256 bytes)
 */

// Flash memory constants for RP2040
#define FLASH_SECTOR_SIZE        4096
#define FLASH_PAGE_SIZE          256
#define FLASH_BLOCK_SIZE         65536

/**
 * @brief Low-level flash memory interface
 */
class Flash {
public:
    Flash();
    
    /**
     * @brief Get total flash size in bytes
     * @return Flash size in bytes
     */
    uint32_t getFlashSize() const;
    
    /**
     * @brief Read data from flash
     * @param offset Offset from start of flash (must be aligned to 4 bytes)
     * @param buffer Buffer to read into
     * @param length Number of bytes to read
     * @return true if read successful
     */
    bool read(uint32_t offset, uint8_t* buffer, size_t length);
    
    /**
     * @brief Write data to flash
     * @param offset Offset from start of flash (must be aligned to page boundary)
     * @param data Data to write
     * @param length Number of bytes to write (must be multiple of page size)
     * @return true if write successful
     * 
     * @note Flash must be erased before writing
     * @note This operation will disable interrupts during write
     */
    bool write(uint32_t offset, const uint8_t* data, size_t length);
    
    /**
     * @brief Erase flash sectors
     * @param offset Offset from start of flash (must be aligned to sector boundary)
     * @param length Number of bytes to erase (must be multiple of sector size)
     * @return true if erase successful
     * 
     * @note This operation will disable interrupts during erase
     */
    bool erase(uint32_t offset, size_t length);
    
    /**
     * @brief Check if offset is aligned to page boundary
     * @param offset Offset to check
     * @return true if aligned to page boundary
     */
    bool isPageAligned(uint32_t offset) const {
        return (offset % FLASH_PAGE_SIZE) == 0;
    }
    
    /**
     * @brief Check if offset is aligned to sector boundary
     * @param offset Offset to check
     * @return true if aligned to sector boundary
     */
    bool isSectorAligned(uint32_t offset) const {
        return (offset % FLASH_SECTOR_SIZE) == 0;
    }
    
    /**
     * @brief Check if length is multiple of page size
     * @param length Length to check
     * @return true if multiple of page size
     */
    bool isPageMultiple(size_t length) const {
        return (length % FLASH_PAGE_SIZE) == 0;
    }
    
    /**
     * @brief Check if length is multiple of sector size
     * @param length Length to check
     * @return true if multiple of sector size
     */
    bool isSectorMultiple(size_t length) const {
        return (length % FLASH_SECTOR_SIZE) == 0;
    }
    
    /**
     * @brief Get offset of last sector (useful for storing configuration)
     * @return Offset of last sector in flash
     */
    uint32_t getLastSectorOffset() const {
        return getFlashSize() - FLASH_SECTOR_SIZE;
    }
    
private:
    uint32_t flash_size_;        // Total flash size in bytes
    
    /**
     * @brief Get flash base address in memory map
     * @return Flash base address
     */
    const uint8_t* getFlashBase() const;
};