#pragma once

#include <cstddef>
#include <cstdint>

#include "../hal/flash.h"

/**
 * @brief Base class for flash-based configuration management
 *
 * Provides common CRC32 calculation and flash operations for configuration
 * classes, reducing code duplication between NodeConfig and HubConfig.
 */
class ConfigurationBase {
protected:
    Flash &flash_;
    uint32_t flash_offset_;

    /**
     * @brief CRC32 lookup table (shared between all config classes)
     */
    static const uint32_t crc32_table[256];

public:
    ConfigurationBase(Flash &flash, uint32_t offset) : flash_(flash), flash_offset_(offset) {}

    virtual ~ConfigurationBase() = default;

    /**
     * @brief Calculate CRC32 for data buffer
     * @param data Data buffer
     * @param length Length of data
     * @return CRC32 checksum
     */
    static uint32_t calculateCRC32(const uint8_t *data, size_t length);

    /**
     * @brief Template method for calculating CRC32 of a structure
     * @tparam T Structure type
     * @param structure Structure to calculate CRC for (excluding CRC field)
     * @param crc_offset Offset of CRC field in structure
     * @return CRC32 checksum
     */
    template <typename T>
    static uint32_t calculateStructCRC(const T &structure, size_t crc_offset)
    {
        return calculateCRC32(reinterpret_cast<const uint8_t *>(&structure), crc_offset);
    }

    /**
     * @brief Verify CRC32 of a structure
     * @tparam T Structure type with a crc32 field
     * @param structure Structure to verify
     * @param crc_offset Offset of CRC field in structure
     * @return true if CRC is valid
     */
    template <typename T>
    static bool verifyCRC(const T &structure, uint32_t stored_crc, size_t crc_offset)
    {
        uint32_t calculated = calculateStructCRC(structure, crc_offset);
        return calculated == stored_crc;
    }

    /**
     * @brief Get backup configuration offset
     * @param sector_offset Sectors from end of flash for backup
     * @return Backup offset in flash
     */
    uint32_t getBackupOffset(uint32_t sector_offset = 2) const
    {
        return flash_.getFlashSize() - (sector_offset * FLASH_SECTOR_SIZE);
    }

    /**
     * @brief Template method for saving configuration with retry and backup
     * @tparam T Configuration structure type
     * @param config Configuration to save
     * @param size Size of configuration
     * @param use_backup Whether to attempt backup save on failure
     * @return true if save successful
     */
    template <typename T>
    bool saveConfig(const T &config, size_t size, bool use_backup = true)
    {
        // Erase with retry
        FlashResult result = flash_.erase(
            flash_offset_, ((size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE,
            3);
        if (result != FlashResult::Success) {
            if (use_backup) {
                return saveConfigToBackup(config, size);
            }
            return false;
        }

        // Write with retry
        result = flash_.write(flash_offset_, reinterpret_cast<const uint8_t *>(&config), size, 3);
        if (result != FlashResult::Success) {
            if (use_backup) {
                return saveConfigToBackup(config, size);
            }
            return false;
        }

        return true;
    }

    /**
     * @brief Template method for loading configuration with backup recovery
     * @tparam T Configuration structure type
     * @param config Configuration to load into
     * @param size Size of configuration
     * @param use_backup Whether to attempt backup recovery on failure
     * @return true if load successful
     */
    template <typename T>
    bool loadConfig(T &config, size_t size, bool use_backup = true)
    {
        FlashResult result = flash_.read(flash_offset_, reinterpret_cast<uint8_t *>(&config), size);
        if (result != FlashResult::Success) {
            if (use_backup) {
                return loadConfigFromBackup(config, size);
            }
            return false;
        }
        return true;
    }

    /**
     * @brief Clear configuration by erasing flash sector
     * @return true if clear successful
     */
    bool clearConfig(size_t size)
    {
        size_t erase_size =
            ((size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
        return flash_.erase(flash_offset_, erase_size, 3) == FlashResult::Success;
    }

private:
    template <typename T>
    bool saveConfigToBackup(const T &config, size_t size)
    {
        uint32_t backup_offset = getBackupOffset();
        FlashResult result = flash_.erase(
            backup_offset, ((size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE,
            2);
        if (result != FlashResult::Success)
            return false;

        result = flash_.write(backup_offset, reinterpret_cast<const uint8_t *>(&config), size, 2);
        return result == FlashResult::Success;
    }

    template <typename T>
    bool loadConfigFromBackup(T &config, size_t size)
    {
        uint32_t backup_offset = getBackupOffset();
        FlashResult result = flash_.read(backup_offset, reinterpret_cast<uint8_t *>(&config), size);
        return result == FlashResult::Success;
    }
};