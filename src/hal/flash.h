#pragma once

#include <stdint.h>
#include <stddef.h>
#include "logger.h"

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
#define FLASH_SECTOR_SIZE 4096
#define FLASH_PAGE_SIZE 256
#define FLASH_BLOCK_SIZE 65536

/**
 * @brief Flash operation result codes
 */
enum FlashResult
{
    FLASH_SUCCESS = 0,           // Operation successful
    FLASH_ERROR_INVALID_PARAM,   // Invalid parameters
    FLASH_ERROR_ALIGNMENT,       // Alignment error
    FLASH_ERROR_BOUNDS,          // Out of bounds access
    FLASH_ERROR_WRITE_PROTECTED, // Write to protected area
    FLASH_ERROR_VERIFY_FAILED,   // Write verification failed
    FLASH_ERROR_ERASE_FAILED,    // Erase operation failed
    FLASH_ERROR_TIMEOUT,         // Operation timed out
    FLASH_ERROR_HARDWARE,        // Hardware failure
    FLASH_ERROR_UNKNOWN = 255    // Unknown error -- default error case
};

/**
 * @brief Flash operation statistics
 */
struct FlashStats
{
    uint32_t reads_attempted;
    uint32_t reads_successful;
    uint32_t writes_attempted;
    uint32_t writes_successful;
    uint32_t erases_attempted;
    uint32_t erases_successful;
    uint32_t verify_failures;
    uint32_t retry_count;
};

/**
 * @brief Low-level flash memory interface with comprehensive error handling
 */
class Flash
{
public:
    Flash();

    /**
     * @brief Get total flash size in bytes
     * @return Flash size in bytes
     */
    uint32_t getFlashSize() const;

    /**
     * @brief Read data from flash with error handling
     * @param offset Offset from start of flash
     * @param buffer Buffer to read into
     * @param length Number of bytes to read
     * @return FlashResult indicating success or failure type
     */
    FlashResult read(uint32_t offset, uint8_t *buffer, size_t length);

    /**
     * @brief Write data to flash with verification and retry
     * @param offset Offset from start of flash (must be aligned to page boundary)
     * @param data Data to write
     * @param length Number of bytes to write (must be multiple of page size)
     * @param max_retries Maximum retry attempts (default: 3)
     * @return FlashResult indicating success or failure type
     *
     * @note Flash must be erased before writing
     * @note This operation will disable interrupts during write
     * @note Automatically verifies write and retries on failure
     */
    FlashResult write(uint32_t offset, const uint8_t *data, size_t length, uint32_t max_retries = 3);

    /**
     * @brief Erase flash sectors with verification and retry
     * @param offset Offset from start of flash (must be aligned to sector boundary)
     * @param length Number of bytes to erase (must be multiple of sector size)
     * @param max_retries Maximum retry attempts (default: 3)
     * @return FlashResult indicating success or failure type
     *
     * @note This operation will disable interrupts during erase
     * @note Automatically verifies erase and retries on failure
     */
    FlashResult erase(uint32_t offset, size_t length, uint32_t max_retries = 3);

    /**
     * @brief Generic alignment check
     * @tparam Boundary The alignment boundary to check against
     * @param value The value to check
     * @return true if aligned to boundary
     */
    template <uint32_t Boundary>
    inline bool isAligned(uint32_t value) const
    {
        return (value % Boundary) == 0;
    }

    // Convenience methods using the generic template
    inline bool isPageAligned(uint32_t offset) const
    {
        return isAligned<FLASH_PAGE_SIZE>(offset);
    }

    inline bool isSectorAligned(uint32_t offset) const
    {
        return isAligned<FLASH_SECTOR_SIZE>(offset);
    }

    inline bool isPageMultiple(size_t length) const
    {
        return isAligned<FLASH_PAGE_SIZE>(static_cast<uint32_t>(length));
    }

    inline bool isSectorMultiple(size_t length) const
    {
        return isAligned<FLASH_SECTOR_SIZE>(static_cast<uint32_t>(length));
    }

    /**
     * @brief Get offset of last sector (useful for storing configuration)
     * @return Offset of last sector in flash
     */
    uint32_t getLastSectorOffset() const
    {
        return getFlashSize() - FLASH_SECTOR_SIZE;
    }

    /**
     * @brief Verify data integrity after write/erase
     * @param offset Offset to verify
     * @param expected_data Expected data (nullptr for erase verification)
     * @param length Length to verify
     * @return FlashResult indicating verification result
     */
    bool verifyData(uint32_t offset, const uint8_t *expected_data, size_t length);

    /**
     * @brief Get error statistics
     * @return Flash operation statistics
     */
    const FlashStats &getStats() const { return stats_; }

    /**
     * @brief Reset error statistics
     */
    void resetStats();

    /**
     * @brief Convert FlashResult to human-readable string
     * @param result Flash result code
     * @return String description of result
     */
    static const char *resultToString(FlashResult result);

    /**
     * @brief Check if flash sector is erased (all 0xFF)
     * @param offset Sector offset
     * @param length Sector length
     * @return true if sector is erased
     */
    bool isSectorErased(uint32_t offset, size_t length);

private:
    uint32_t flash_size_; // Total flash size in bytes
    FlashStats stats_;    // Operation statistics
    Logger logger_;       // Module logger

    /**
     * @brief Generic retry logic for flash operations
     * @tparam Operation Callable that returns FlashResult
     * @param op The operation to perform
     * @param max_retries Maximum number of retries
     * @param op_name Operation name for logging
     * @return Result of the operation
     */
    template <typename Operation>
    FlashResult retryOperation(Operation op, uint32_t max_retries, const char *op_name)
    {
        FlashResult result = FLASH_ERROR_UNKNOWN;

        for (uint32_t attempt = 0; attempt <= max_retries; attempt++)
        {
            if (attempt > 0)
            {
                stats_.retry_count++;
                logger_.warn("%s retry %lu/%lu", op_name, attempt, max_retries);
            }

            result = op();

            if (result == FLASH_SUCCESS)
            {
                return FLASH_SUCCESS;
            }

            logger_.error("%s failed on attempt %lu: %s", op_name, attempt + 1,
                          resultToString(result));
        }

        return result;
    }

    /**
     * @brief Get flash base address in memory map
     * @return Flash base address
     */
    const uint8_t *getFlashBase() const;

    /**
     * @brief Perform low-level write operation
     * @param offset Flash offset
     * @param data Data to write
     * @param length Data length
     * @return FlashResult indicating success/failure
     */
    FlashResult performWrite(uint32_t offset, const uint8_t *data, size_t length);

    /**
     * @brief Perform low-level erase operation
     * @param offset Flash offset
     * @param length Erase length
     * @return FlashResult indicating success/failure
     */
    FlashResult performErase(uint32_t offset, size_t length);

    /**
     * @brief Validate operation parameters
     * @param offset Flash offset
     * @param length Operation length
     * @param require_page_align Require page alignment
     * @param require_sector_align Require sector alignment
     * @return FlashResult indicating validation result
     */
    FlashResult validateParams(uint32_t offset, size_t length,
                               bool require_page_align = false,
                               bool require_sector_align = false);
};