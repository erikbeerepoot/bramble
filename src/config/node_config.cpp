#include "node_config.h"
#include "lora/message.h"  // For ADDRESS_UNREGISTERED
#include "hal/logger.h"
#include "pico/stdlib.h"
#include <cstring>
#include <stdio.h>

// Simple CRC32 implementation for configuration validation
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

NodeConfigManager::NodeConfigManager(Flash& flash_hal) 
    : flash_(flash_hal), logger_("CONFIG") {
    // Use last sector of flash for configuration
    config_offset_ = flash_.getLastSectorOffset();
    logger_.debug("Using config offset 0x%08lx", config_offset_);
}

bool NodeConfigManager::hasValidConfiguration() {
    NodeConfiguration config;
    return loadConfiguration(config);
}

bool NodeConfigManager::loadConfiguration(NodeConfiguration& config) {
    // Try to read configuration from primary location
    FlashResult result = flash_.read(config_offset_, (uint8_t*)&config, sizeof(config));
    if (result != FLASH_SUCCESS) {
        printf("Failed to read configuration from primary location: %s\n", Flash::resultToString(result));
        
        // Attempt backup recovery
        return attemptBackupRecovery(config);
    }
    
    // Check magic number
    if (config.magic != NODE_CONFIG_MAGIC) {
        logger_.warn("Primary config magic mismatch (found 0x%08lx, expected 0x%08x)", 
                    config.magic, NODE_CONFIG_MAGIC);
        
        // Attempt backup recovery
        return attemptBackupRecovery(config);
    }
    
    // Verify CRC
    if (!verifyCRC(config)) {
        logger_.warn("Primary configuration CRC verification failed");
        
        // Attempt backup recovery
        return attemptBackupRecovery(config);
    }
    
    logger_.info("Configuration loaded successfully from primary location");
    return true;
}

bool NodeConfigManager::saveConfiguration(const NodeConfiguration& config) {
    // Create a copy to calculate CRC
    NodeConfiguration config_copy = config;
    config_copy.magic = NODE_CONFIG_MAGIC;
    config_copy.crc32 = calculateCRC32(config_copy);
    
    printf("Saving configuration to flash at offset 0x%08lx\n", config_offset_);
    
    // Erase the sector first with retry
    FlashResult erase_result = flash_.erase(config_offset_, FLASH_SECTOR_SIZE, 3);
    if (erase_result != FLASH_SUCCESS) {
        printf("Failed to erase configuration sector: %s\n", Flash::resultToString(erase_result));
        
        // Attempt error recovery - try alternate backup location if available
        return attemptBackupSave(config_copy);
    }
    
    // Write configuration with retry
    FlashResult write_result = flash_.write(config_offset_, (const uint8_t*)&config_copy, sizeof(config_copy), 3);
    if (write_result != FLASH_SUCCESS) {
        printf("Failed to write configuration: %s\n", Flash::resultToString(write_result));
        
        // Attempt error recovery - try alternate backup location if available
        return attemptBackupSave(config_copy);
    }
    
    // Verify the save was successful by reading it back
    NodeConfiguration verify_config;
    if (!loadConfiguration(verify_config)) {
        printf("Configuration save verification failed\n");
        return attemptBackupSave(config_copy);
    }
    
    // Compare key fields to ensure integrity
    if (verify_config.device_id != config_copy.device_id ||
        verify_config.assigned_address != config_copy.assigned_address ||
        verify_config.node_type != config_copy.node_type) {
        printf("Configuration save verification failed - data mismatch\n");
        return attemptBackupSave(config_copy);
    }
    
    printf("Configuration saved and verified successfully at offset 0x%08lx\n", config_offset_);
    return true;
}

bool NodeConfigManager::clearConfiguration() {
    printf("Clearing configuration from flash\n");
    
    // Erase the primary configuration sector with retry
    FlashResult result = flash_.erase(config_offset_, FLASH_SECTOR_SIZE, 3);
    if (result != FLASH_SUCCESS) {
        printf("Failed to clear primary configuration sector: %s\n", Flash::resultToString(result));
        
        // Also try to clear backup sector if it exists
        uint32_t backup_offset = getBackupConfigOffset();
        if (backup_offset != config_offset_) {
            FlashResult backup_result = flash_.erase(backup_offset, FLASH_SECTOR_SIZE, 3);
            if (backup_result != FLASH_SUCCESS) {
                printf("Failed to clear backup configuration sector: %s\n", Flash::resultToString(backup_result));
                return false;
            }
            printf("Backup configuration sector cleared\n");
        }
        return false;
    }
    
    // Also clear backup sector for complete cleanup
    uint32_t backup_offset = getBackupConfigOffset();
    if (backup_offset != config_offset_) {
        FlashResult backup_result = flash_.erase(backup_offset, FLASH_SECTOR_SIZE, 1);
        if (backup_result == FLASH_SUCCESS) {
            printf("Backup configuration sector also cleared\n");
        }
    }
    
    printf("Configuration cleared from flash\n");
    return true;
}

NodeConfiguration NodeConfigManager::createDefaultConfiguration(
    uint64_t device_id, 
    const char* device_name,
    uint8_t node_type,
    uint8_t capabilities,
    uint16_t firmware_version) {
    
    NodeConfiguration config = {};  // Zero-initialize
    
    config.magic = NODE_CONFIG_MAGIC;
    config.assigned_address = ADDRESS_UNREGISTERED;  // Will be assigned by hub
    config.device_id = device_id;
    config.registration_time = to_ms_since_boot(get_absolute_time());
    config.node_type = node_type;
    config.capabilities = capabilities;
    config.firmware_version = firmware_version;
    
    // Copy device name
    if (device_name) {
        strncpy(config.device_name, device_name, sizeof(config.device_name) - 1);
        config.device_name[sizeof(config.device_name) - 1] = '\0';
    } else {
        strcpy(config.device_name, "Unknown");
    }
    
    // CRC will be calculated when saving
    config.crc32 = 0;
    
    return config;
}

uint32_t NodeConfigManager::calculateCRC32(const NodeConfiguration& config) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* data = (const uint8_t*)&config;
    
    // Calculate CRC for all fields except the CRC field itself
    size_t length = offsetof(NodeConfiguration, crc32);
    
    for (size_t i = 0; i < length; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    
    return crc ^ 0xFFFFFFFF;
}

bool NodeConfigManager::verifyCRC(const NodeConfiguration& config) {
    uint32_t calculated_crc = calculateCRC32(config);
    return calculated_crc == config.crc32;
}

uint32_t NodeConfigManager::getBackupConfigOffset() const {
    // Use second-to-last sector for backup configuration
    return flash_.getFlashSize() - (2 * FLASH_SECTOR_SIZE);
}

bool NodeConfigManager::attemptBackupSave(const NodeConfiguration& config) {
    printf("Attempting to save configuration to backup location\n");
    
    uint32_t backup_offset = getBackupConfigOffset();
    
    // Erase backup sector
    FlashResult erase_result = flash_.erase(backup_offset, FLASH_SECTOR_SIZE, 2);
    if (erase_result != FLASH_SUCCESS) {
        printf("Failed to erase backup configuration sector: %s\n", Flash::resultToString(erase_result));
        return false;
    }
    
    // Write to backup location
    FlashResult write_result = flash_.write(backup_offset, (const uint8_t*)&config, sizeof(config), 2);
    if (write_result != FLASH_SUCCESS) {
        printf("Failed to write backup configuration: %s\n", Flash::resultToString(write_result));
        return false;
    }
    
    printf("Configuration saved to backup location at offset 0x%08lx\n", backup_offset);
    return true;
}

bool NodeConfigManager::attemptBackupRecovery(NodeConfiguration& config) {
    printf("Attempting to recover configuration from backup location\n");
    
    uint32_t backup_offset = getBackupConfigOffset();
    
    // Try to read from backup location
    FlashResult result = flash_.read(backup_offset, (uint8_t*)&config, sizeof(config));
    if (result != FLASH_SUCCESS) {
        printf("Failed to read backup configuration: %s\n", Flash::resultToString(result));
        return false;
    }
    
    // Verify backup configuration
    if (config.magic != NODE_CONFIG_MAGIC) {
        printf("Backup configuration magic number invalid\n");
        return false;
    }
    
    if (!verifyCRC(config)) {
        printf("Backup configuration CRC verification failed\n");
        return false;
    }
    
    printf("Configuration successfully recovered from backup\n");
    
    // Try to restore to primary location
    uint32_t primary_offset = config_offset_;
    FlashResult erase_result = flash_.erase(primary_offset, FLASH_SECTOR_SIZE, 2);
    if (erase_result == FLASH_SUCCESS) {
        FlashResult write_result = flash_.write(primary_offset, (const uint8_t*)&config, sizeof(config), 2);
        if (write_result == FLASH_SUCCESS) {
            printf("Configuration restored to primary location\n");
        } else {
            printf("Warning: Could not restore configuration to primary location\n");
        }
    }
    
    return true;
}