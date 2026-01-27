#include "node_config.h"

#include <cstddef>
#include <cstring>

#include "pico/stdlib.h"

#include "hal/logger.h"
#include "lora/message.h"  // For ADDRESS_UNREGISTERED

NodeConfigManager::NodeConfigManager(Flash &flash_hal)
    : ConfigurationBase(flash_hal, flash_hal.getLastSectorOffset()), logger_("CONFIG")
{
    logger_.debug("Using config offset 0x%08lx", flash_offset_);
}

bool NodeConfigManager::hasValidConfiguration()
{
    NodeConfiguration config;
    return loadConfiguration(config);
}

bool NodeConfigManager::loadConfiguration(NodeConfiguration &config)
{
    // Try to read configuration from primary location
    FlashResult result = flash_.read(flash_offset_, (uint8_t *)&config, sizeof(config));
    if (result != FlashResult::Success) {
        logger_.error("Failed to read configuration from primary location: %s",
                      Flash::resultToString(result));

        // Attempt backup recovery
        return attemptBackupRecovery(config);
    }

    // Check magic number
    if (config.magic != NODE_CONFIG_MAGIC) {
        logger_.warn("Primary config magic mismatch (found 0x%08lx, expected 0x%08x)", config.magic,
                     NODE_CONFIG_MAGIC);

        // Attempt backup recovery
        return attemptBackupRecovery(config);
    }

    // Verify CRC
    if (!verifyCRC(config)) {
        logger_.warn("Primary configuration CRC verification failed");

        // Attempt backup recovery
        return attemptBackupRecovery(config);
    }

    logger_.info("Configuration loaded");
    return true;
}

bool NodeConfigManager::saveConfiguration(const NodeConfiguration &config)
{
    // Create a copy to calculate CRC
    NodeConfiguration config_copy = config;
    config_copy.magic = NODE_CONFIG_MAGIC;
    config_copy.crc32 = calculateStructCRC(config_copy, offsetof(NodeConfiguration, crc32));

    logger_.info("Saving configuration to flash at offset 0x%08lx", flash_offset_);

    // Use base class save method
    if (!saveConfig(config_copy, sizeof(config_copy))) {
        return attemptBackupSave(config_copy);
    }

    // Verify the save was successful by reading it back
    NodeConfiguration verify_config;
    if (!loadConfiguration(verify_config)) {
        logger_.error("Configuration save verification failed");
        return attemptBackupSave(config_copy);
    }

    // Compare key fields to ensure integrity
    if (verify_config.device_id != config_copy.device_id ||
        verify_config.assigned_address != config_copy.assigned_address ||
        verify_config.node_type != config_copy.node_type) {
        logger_.error("Configuration save verification failed - data mismatch");
        return attemptBackupSave(config_copy);
    }

    logger_.info("Configuration saved and verified successfully at offset 0x%08lx", flash_offset_);
    return true;
}

bool NodeConfigManager::clearConfiguration()
{
    logger_.info("Clearing configuration from flash");

    // Use base class clear method
    if (!clearConfig(sizeof(NodeConfiguration))) {
        logger_.error("Failed to clear primary configuration sector");

        // Also try to clear backup sector if it exists
        uint32_t backup_offset = getBackupOffset();
        if (backup_offset != flash_offset_) {
            FlashResult backup_result = flash_.erase(backup_offset, FLASH_SECTOR_SIZE, 3);
            if (backup_result != FlashResult::Success) {
                logger_.error("Failed to clear backup configuration sector: %s",
                              Flash::resultToString(backup_result));
                return false;
            }
            logger_.info("Backup configuration sector cleared");
        }
        return false;
    }

    // Also clear backup sector for complete cleanup
    uint32_t backup_offset = getBackupOffset();
    if (backup_offset != flash_offset_) {
        FlashResult backup_result = flash_.erase(backup_offset, FLASH_SECTOR_SIZE, 1);
        if (backup_result == FlashResult::Success) {
            logger_.debug("Backup configuration sector also cleared");
        }
    }

    logger_.info("Configuration cleared from flash");
    return true;
}

NodeConfiguration NodeConfigManager::createDefaultConfiguration(uint64_t device_id,
                                                                const char *device_name,
                                                                uint8_t node_type,
                                                                uint8_t capabilities,
                                                                uint32_t firmware_version)
{
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

bool NodeConfigManager::verifyCRC(const NodeConfiguration &config)
{
    return ConfigurationBase::verifyCRC(config, config.crc32, offsetof(NodeConfiguration, crc32));
}

bool NodeConfigManager::attemptBackupSave(const NodeConfiguration &config)
{
    logger_.info("Attempting to save configuration to backup location");

    uint32_t backup_offset = getBackupOffset();

    // Erase backup sector
    FlashResult erase_result = flash_.erase(backup_offset, FLASH_SECTOR_SIZE, 2);
    if (erase_result != FlashResult::Success) {
        logger_.error("Failed to erase backup configuration sector: %s",
                      Flash::resultToString(erase_result));
        return false;
    }

    // Write to backup location
    FlashResult write_result =
        flash_.write(backup_offset, (const uint8_t *)&config, sizeof(config), 2);
    if (write_result != FlashResult::Success) {
        logger_.error("Failed to write backup configuration: %s",
                      Flash::resultToString(write_result));
        return false;
    }

    logger_.info("Configuration saved to backup location at offset 0x%08lx", backup_offset);
    return true;
}

bool NodeConfigManager::attemptBackupRecovery(NodeConfiguration &config)
{
    logger_.info("Attempting to recover configuration from backup location");

    uint32_t backup_offset = getBackupOffset();

    // Try to read from backup location
    FlashResult result = flash_.read(backup_offset, (uint8_t *)&config, sizeof(config));
    if (result != FlashResult::Success) {
        logger_.error("Failed to read backup configuration: %s", Flash::resultToString(result));
        return false;
    }

    // Verify backup configuration
    if (config.magic != NODE_CONFIG_MAGIC) {
        logger_.error("Backup configuration magic number invalid");
        return false;
    }

    if (!verifyCRC(config)) {
        logger_.error("Backup configuration CRC verification failed");
        return false;
    }

    logger_.info("Configuration successfully recovered from backup");

    // Try to restore to primary location
    uint32_t primary_offset = flash_offset_;
    FlashResult erase_result = flash_.erase(primary_offset, FLASH_SECTOR_SIZE, 2);
    if (erase_result == FlashResult::Success) {
        FlashResult write_result =
            flash_.write(primary_offset, (const uint8_t *)&config, sizeof(config), 2);
        if (write_result == FlashResult::Success) {
            logger_.info("Configuration restored to primary location");
        } else {
            logger_.warn("Could not restore configuration to primary location");
        }
    }

    return true;
}