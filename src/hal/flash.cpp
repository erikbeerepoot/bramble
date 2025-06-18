#include "flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <cstring>
#include <cstdio>

// Flash memory starts at XIP_BASE in RP2040 memory map
extern "C" char __flash_binary_end;

Flash::Flash() {
    // Feather RP2040 LoRa has 8MB external QSPI flash
    flash_size_ = 8 * 1024 * 1024;  // 8MB external QSPI flash
    
    // Initialize statistics
    resetStats();
}

uint32_t Flash::getFlashSize() const {
    return flash_size_;
}

const uint8_t* Flash::getFlashBase() const {
    return (const uint8_t*)XIP_BASE;
}

FlashResult Flash::read(uint32_t offset, uint8_t* buffer, size_t length) {
    stats_.reads_attempted++;
    
    // Validate parameters
    FlashResult validation = validateParams(offset, length);
    if (validation != FLASH_SUCCESS) {
        return validation;
    }
    
    if (!buffer) {
        return FLASH_ERROR_INVALID_PARAM;
    }
    
    // Direct memory read from QSPI flash via XIP (Execute In Place)
    const uint8_t* flash_address = getFlashBase() + offset;
    memcpy(buffer, flash_address, length);
    
    stats_.reads_successful++;
    return FLASH_SUCCESS;
}

FlashResult Flash::write(uint32_t offset, const uint8_t* data, size_t length, uint32_t max_retries) {
    stats_.writes_attempted++;
    
    // Validate parameters
    FlashResult validation = validateParams(offset, length, true, false);
    if (validation != FLASH_SUCCESS) {
        return validation;
    }
    
    if (!data) {
        return FLASH_ERROR_INVALID_PARAM;
    }
    
    // Ensure we're not writing to the program area
    uint32_t program_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    if (offset < program_end) {
        return FLASH_ERROR_WRITE_PROTECTED;
    }
    
    // Retry loop for write operation
    for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            stats_.retry_count++;
            printf("Flash write retry %lu/%lu at offset 0x%08lx\n", attempt, max_retries, offset);
        }
        
        // Perform the write operation
        FlashResult write_result = performWrite(offset, data, length);
        if (write_result != FLASH_SUCCESS) {
            continue;  // Try again
        }
        
        // Verify the write was successful
        FlashResult verify_result = verifyData(offset, data, length);
        if (verify_result == FLASH_SUCCESS) {
            stats_.writes_successful++;
            return FLASH_SUCCESS;
        }
        
        stats_.verify_failures++;
        printf("Flash write verification failed at offset 0x%08lx\n", offset);
    }
    
    printf("Flash write failed after %lu attempts at offset 0x%08lx\n", max_retries + 1, offset);
    return FLASH_ERROR_VERIFY_FAILED;
}

FlashResult Flash::erase(uint32_t offset, size_t length, uint32_t max_retries) {
    stats_.erases_attempted++;
    
    // Validate parameters
    FlashResult validation = validateParams(offset, length, false, true);
    if (validation != FLASH_SUCCESS) {
        return validation;
    }
    
    // Ensure we're not erasing the program area
    uint32_t program_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    if (offset < program_end) {
        return FLASH_ERROR_WRITE_PROTECTED;
    }
    
    // Retry loop for erase operation
    for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            stats_.retry_count++;
            printf("Flash erase retry %lu/%lu at offset 0x%08lx\n", attempt, max_retries, offset);
        }
        
        // Perform the erase operation
        FlashResult erase_result = performErase(offset, length);
        if (erase_result != FLASH_SUCCESS) {
            continue;  // Try again
        }
        
        // Verify the erase was successful (should be all 0xFF)
        FlashResult verify_result = verifyData(offset, nullptr, length);
        if (verify_result == FLASH_SUCCESS) {
            stats_.erases_successful++;
            return FLASH_SUCCESS;
        }
        
        stats_.verify_failures++;
        printf("Flash erase verification failed at offset 0x%08lx\n", offset);
    }
    
    printf("Flash erase failed after %lu attempts at offset 0x%08lx\n", max_retries + 1, offset);
    return FLASH_ERROR_ERASE_FAILED;
}

FlashResult Flash::verifyData(uint32_t offset, const uint8_t* expected_data, size_t length) {
    // Allocate buffer for verification read
    uint8_t* verify_buffer = new uint8_t[length];
    if (!verify_buffer) {
        return FLASH_ERROR_HARDWARE;
    }
    
    // Read data from flash
    const uint8_t* flash_address = getFlashBase() + offset;
    memcpy(verify_buffer, flash_address, length);
    
    bool verification_passed = true;
    
    if (expected_data) {
        // Verify against expected data (for write verification)
        verification_passed = (memcmp(verify_buffer, expected_data, length) == 0);
    } else {
        // Verify erase (should be all 0xFF)
        for (size_t i = 0; i < length; i++) {
            if (verify_buffer[i] != 0xFF) {
                verification_passed = false;
                break;
            }
        }
    }
    
    delete[] verify_buffer;
    return verification_passed ? FLASH_SUCCESS : FLASH_ERROR_VERIFY_FAILED;
}

FlashResult Flash::performWrite(uint32_t offset, const uint8_t* data, size_t length) {
    // Disable interrupts during QSPI flash write
    uint32_t interrupt_status = save_and_disable_interrupts();
    
    // Perform QSPI flash write
    // Note: flash_range_program expects offset from XIP_BASE, not absolute address
    flash_range_program(offset, data, length);
    
    // Re-enable interrupts
    restore_interrupts(interrupt_status);
    
    return FLASH_SUCCESS;
}

FlashResult Flash::performErase(uint32_t offset, size_t length) {
    // Disable interrupts during QSPI flash erase
    uint32_t interrupt_status = save_and_disable_interrupts();
    
    // Perform QSPI flash erase
    // Note: flash_range_erase expects offset from XIP_BASE, not absolute address
    flash_range_erase(offset, length);
    
    // Re-enable interrupts
    restore_interrupts(interrupt_status);
    
    return FLASH_SUCCESS;
}

FlashResult Flash::validateParams(uint32_t offset, size_t length, 
                                 bool require_page_align, bool require_sector_align) {
    // Check bounds
    if (offset + length > flash_size_) {
        return FLASH_ERROR_BOUNDS;
    }
    
    // Check alignment requirements
    if (require_page_align && !isPageAligned(offset)) {
        return FLASH_ERROR_ALIGNMENT;
    }
    
    if (require_page_align && !isPageMultiple(length)) {
        return FLASH_ERROR_ALIGNMENT;
    }
    
    if (require_sector_align && !isSectorAligned(offset)) {
        return FLASH_ERROR_ALIGNMENT;
    }
    
    if (require_sector_align && !isSectorMultiple(length)) {
        return FLASH_ERROR_ALIGNMENT;
    }
    
    return FLASH_SUCCESS;
}

bool Flash::isSectorErased(uint32_t offset, size_t length) {
    const uint8_t* flash_address = getFlashBase() + offset;
    
    for (size_t i = 0; i < length; i++) {
        if (flash_address[i] != 0xFF) {
            return false;
        }
    }
    
    return true;
}

void Flash::resetStats() {
    stats_.reads_attempted = 0;
    stats_.reads_successful = 0;
    stats_.writes_attempted = 0;
    stats_.writes_successful = 0;
    stats_.erases_attempted = 0;
    stats_.erases_successful = 0;
    stats_.verify_failures = 0;
    stats_.retry_count = 0;
}

const char* Flash::resultToString(FlashResult result) {
    switch (result) {
        case FLASH_SUCCESS:             return "Success";
        case FLASH_ERROR_INVALID_PARAM: return "Invalid parameter";
        case FLASH_ERROR_ALIGNMENT:     return "Alignment error";
        case FLASH_ERROR_BOUNDS:        return "Out of bounds";
        case FLASH_ERROR_WRITE_PROTECTED: return "Write protected";
        case FLASH_ERROR_VERIFY_FAILED: return "Verification failed";
        case FLASH_ERROR_ERASE_FAILED:  return "Erase failed";
        case FLASH_ERROR_TIMEOUT:       return "Timeout";
        case FLASH_ERROR_HARDWARE:      return "Hardware error";
        default:                        return "Unknown error";
    }
}