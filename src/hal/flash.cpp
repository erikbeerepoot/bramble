#include "flash.h"

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

// Flash memory starts at XIP_BASE in RP2040 memory map
extern "C" char __flash_binary_end;

Flash::Flash() : logger_("FLASH")
{
    // Feather RP2040 LoRa has 8MB external QSPI flash
    flash_size_ = 8 * 1024 * 1024;  // 8MB external QSPI flash

    // Initialize statistics
    resetStats();

    logger_.debug("Initialized with %lu MB flash", flash_size_ / (1024 * 1024));
}

uint32_t Flash::getFlashSize() const
{
    return flash_size_;
}

const uint8_t *Flash::getFlashBase() const
{
    return (const uint8_t *)XIP_BASE;
}

FlashResult Flash::read(uint32_t offset, uint8_t *buffer, size_t length)
{
    stats_.reads_attempted++;

    // Validate parameters
    FlashResult validation = validateParams(offset, length);
    if (validation != FlashResult::Success) {
        return validation;
    }

    if (!buffer) {
        return FlashResult::ErrorInvalidParam;
    }

    // Direct memory read from QSPI flash via XIP (Execute In Place)
    const uint8_t *flash_address = getFlashBase() + offset;
    memcpy(buffer, flash_address, length);

    stats_.reads_successful++;
    return FlashResult::Success;
}

FlashResult Flash::write(uint32_t offset, const uint8_t *data, size_t length, uint32_t max_retries)
{
    stats_.writes_attempted++;

    // Validate parameters
    FlashResult validation = validateParams(offset, length, true, false);
    if (validation != FlashResult::Success) {
        return validation;
    }

    if (!data) {
        return FlashResult::ErrorInvalidParam;
    }

    // Ensure we're not writing to the program area
    uint32_t program_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    if (offset < program_end) {
        return FlashResult::ErrorWriteProtected;
    }

    return retryOperation(
        [this, offset, data, length]() -> FlashResult {
            // Perform write
            FlashResult write_result = performWrite(offset, data, length);
            if (write_result != FlashResult::Success) {
                return write_result;
            }

            // Verify write
            if (!verifyData(offset, data, length)) {
                stats_.verify_failures++;
                return FlashResult::ErrorVerifyFailed;
            }

            stats_.writes_successful++;
            logger_.info("Write successful at offset 0x%08lx", offset);
            return FlashResult::Success;
        },
        max_retries, "Write");

    logger_.error("Write failed after %lu attempts at offset 0x%08lx", max_retries + 1, offset);
    return FlashResult::ErrorVerifyFailed;
}

FlashResult Flash::erase(uint32_t offset, size_t length, uint32_t max_retries)
{
    // Validate parameters
    FlashResult validation = validateParams(offset, length, false, true);
    if (validation != FlashResult::Success) {
        return validation;
    }

    // Ensure we're not erasing the program area
    uint32_t program_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    if (offset < program_end) {
        return FlashResult::ErrorWriteProtected;
    }

    // Retry loop for erase operation
    // Round up to sector boundary
    size_t erase_length =
        ((length + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    // Update statistics
    stats_.erases_attempted++;

    // Use retry template for the erase operation
    return retryOperation(
        [this, offset, erase_length]() -> FlashResult {
            // Perform erase
            FlashResult erase_result = performErase(offset, erase_length);
            if (erase_result != FlashResult::Success) {
                return erase_result;
            }

            // Verify erase (all bytes should be 0xFF)
            uint8_t sample;
            for (size_t i = 0; i < erase_length; i += FLASH_PAGE_SIZE) {
                if (read(offset + i, &sample, 1) != FlashResult::Success || sample != 0xFF) {
                    stats_.verify_failures++;
                    return FlashResult::ErrorVerifyFailed;
                }
            }
            stats_.erases_successful++;
            logger_.info("Erase successful at offset 0x%08lx", offset);

            return FlashResult::Success;
        },
        max_retries, "Erase");

    logger_.error("Erase failed after %lu attempts at offset 0x%08lx", max_retries + 1, offset);
    return FlashResult::ErrorEraseFailed;
}

bool Flash::verifyData(uint32_t offset, const uint8_t *expected_data, size_t length)
{
    if (!expected_data || length == 0) {
        return false;
    }

    // For QSPI flash, we can read directly from memory-mapped region
    const uint8_t *flash_ptr = getFlashBase() + offset;

    // Direct memory comparison
    bool match = (memcmp(flash_ptr, expected_data, length) == 0);

    if (!match) {
        logger_.error("Verification failed at offset 0x%08lx", offset);

        // Log first few mismatched bytes for debugging
        for (size_t i = 0; i < length && i < 16; i++) {
            if (flash_ptr[i] != expected_data[i]) {
                logger_.debug("  Offset 0x%08lx: expected 0x%02X, got 0x%02X", offset + i,
                              expected_data[i], flash_ptr[i]);
            }
        }
    }

    return match;
}

FlashResult Flash::performWrite(uint32_t offset, const uint8_t *data, size_t length)
{
    // Disable interrupts during QSPI flash write
    uint32_t interrupt_status = save_and_disable_interrupts();

    // Perform QSPI flash write
    // Note: flash_range_program expects offset from XIP_BASE, not absolute address
    flash_range_program(offset, data, length);

    // Re-enable interrupts
    restore_interrupts(interrupt_status);

    return FlashResult::Success;
}

FlashResult Flash::performErase(uint32_t offset, size_t length)
{
    // Disable interrupts during QSPI flash erase
    uint32_t interrupt_status = save_and_disable_interrupts();

    // Perform QSPI flash erase
    // Note: flash_range_erase expects offset from XIP_BASE, not absolute address
    flash_range_erase(offset, length);

    // Re-enable interrupts
    restore_interrupts(interrupt_status);

    return FlashResult::Success;
}

FlashResult Flash::validateParams(uint32_t offset, size_t length, bool require_page_align,
                                  bool require_sector_align)
{
    // Check bounds
    if (offset + length > flash_size_) {
        return FlashResult::ErrorBounds;
    }

    // Check alignment requirements
    if (require_page_align && !isPageAligned(offset)) {
        return FlashResult::ErrorAlignment;
    }

    if (require_page_align && !isPageMultiple(length)) {
        return FlashResult::ErrorAlignment;
    }

    if (require_sector_align && !isSectorAligned(offset)) {
        return FlashResult::ErrorAlignment;
    }

    if (require_sector_align && !isSectorMultiple(length)) {
        return FlashResult::ErrorAlignment;
    }

    return FlashResult::Success;
}

bool Flash::isSectorErased(uint32_t offset, size_t length)
{
    const uint8_t *flash_address = getFlashBase() + offset;

    for (size_t i = 0; i < length; i++) {
        if (flash_address[i] != 0xFF) {
            return false;
        }
    }

    return true;
}

void Flash::resetStats()
{
    stats_.reads_attempted = 0;
    stats_.reads_successful = 0;
    stats_.writes_attempted = 0;
    stats_.writes_successful = 0;
    stats_.erases_attempted = 0;
    stats_.erases_successful = 0;
    stats_.verify_failures = 0;
    stats_.retry_count = 0;
}

const char *Flash::resultToString(FlashResult result)
{
    switch (result) {
        case FlashResult::Success:
            return "Success";
        case FlashResult::ErrorInvalidParam:
            return "Invalid parameter";
        case FlashResult::ErrorAlignment:
            return "Alignment error";
        case FlashResult::ErrorBounds:
            return "Out of bounds";
        case FlashResult::ErrorWriteProtected:
            return "Write protected";
        case FlashResult::ErrorVerifyFailed:
            return "Verification failed";
        case FlashResult::ErrorEraseFailed:
            return "Erase failed";
        case FlashResult::ErrorTimeout:
            return "Timeout";
        case FlashResult::ErrorHardware:
            return "Hardware error";
        case FlashResult::ErrorUnknown:
            return "Unknown error";
        default:
            return "Unknown error";
    }
}