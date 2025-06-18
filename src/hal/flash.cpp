#include "flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <cstring>

// Flash memory starts at XIP_BASE in RP2040 memory map
extern "C" char __flash_binary_end;

Flash::Flash() {
    // Feather RP2040 LoRa has 8MB external QSPI flash
    flash_size_ = 8 * 1024 * 1024;  // 8MB external QSPI flash
}

uint32_t Flash::getFlashSize() const {
    return flash_size_;
}

const uint8_t* Flash::getFlashBase() const {
    return (const uint8_t*)XIP_BASE;
}

bool Flash::read(uint32_t offset, uint8_t* buffer, size_t length) {
    if (!buffer || offset + length > flash_size_) {
        return false;
    }
    
    // Direct memory read from QSPI flash via XIP (Execute In Place)
    const uint8_t* flash_address = getFlashBase() + offset;
    memcpy(buffer, flash_address, length);
    
    return true;
}

bool Flash::write(uint32_t offset, const uint8_t* data, size_t length) {
    if (!data || offset + length > flash_size_) {
        return false;
    }
    
    // Verify alignment requirements for QSPI flash
    if (!isPageAligned(offset) || !isPageMultiple(length)) {
        return false;
    }
    
    // Ensure we're not writing to the program area
    uint32_t program_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    if (offset < program_end) {
        return false;  // Attempting to overwrite program code
    }
    
    // Disable interrupts during QSPI flash write
    uint32_t interrupt_status = save_and_disable_interrupts();
    
    // Perform QSPI flash write
    // Note: flash_range_program expects offset from XIP_BASE, not absolute address
    flash_range_program(offset, data, length);
    
    // Re-enable interrupts
    restore_interrupts(interrupt_status);
    
    return true;
}

bool Flash::erase(uint32_t offset, size_t length) {
    if (offset + length > flash_size_) {
        return false;
    }
    
    // Verify alignment requirements for QSPI flash
    if (!isSectorAligned(offset) || !isSectorMultiple(length)) {
        return false;
    }
    
    // Ensure we're not erasing the program area
    uint32_t program_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    if (offset < program_end) {
        return false;  // Attempting to erase program code
    }
    
    // Disable interrupts during QSPI flash erase
    uint32_t interrupt_status = save_and_disable_interrupts();
    
    // Perform QSPI flash erase
    // Note: flash_range_erase expects offset from XIP_BASE, not absolute address
    flash_range_erase(offset, length);
    
    // Re-enable interrupts
    restore_interrupts(interrupt_status);
    
    return true;
}