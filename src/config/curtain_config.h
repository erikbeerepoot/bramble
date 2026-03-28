#pragma once

#include <cstddef>
#include <cstdint>

#include "config_base.h"
#include "hal/flash.h"
#include "hal/logger.h"

/**
 * @brief Curtain configuration stored in flash
 *
 * Persists calibrated travel time across power cycles.
 * Stored at sector-3 from end of flash, backup at sector-4.
 */
struct __attribute__((packed)) CurtainConfiguration {
    uint32_t magic;           // Validation marker (0xCAFE5678)
    uint32_t travel_time_ms;  // Calibrated travel time in milliseconds
    uint32_t crc32;           // CRC32 of fields above
    uint8_t padding[244];     // Pad to 256 bytes (1 flash page)
};

static_assert(sizeof(CurtainConfiguration) == FLASH_PAGE_SIZE,
              "CurtainConfiguration must be exactly one flash page");

constexpr uint32_t CURTAIN_CONFIG_MAGIC = 0xCAFE5678;

/**
 * @brief Manager for curtain configuration persistence
 */
class CurtainConfigManager : public ConfigurationBase {
public:
    explicit CurtainConfigManager(Flash &flash_hal)
        : ConfigurationBase(flash_hal, flash_hal.getFlashSize() - 3 * FLASH_SECTOR_SIZE),
          logger_("CurtCfg")
    {
    }

    /**
     * @brief Save calibrated travel time to flash
     * @param travel_time_ms Travel time in milliseconds
     * @return true if save successful
     */
    bool saveTravelTime(uint32_t travel_time_ms)
    {
        CurtainConfiguration config = {};
        config.magic = CURTAIN_CONFIG_MAGIC;
        config.travel_time_ms = travel_time_ms;
        config.crc32 = calculateStructCRC(config, offsetof(CurtainConfiguration, crc32));

        bool result = saveConfig(config, sizeof(config), true);
        if (result) {
            logger_.info("Saved travel time: %lu ms", travel_time_ms);
        } else {
            logger_.error("Failed to save travel time");
        }
        return result;
    }

    /**
     * @brief Load calibrated travel time from flash
     * @param travel_time_ms Output travel time
     * @return true if valid config found
     */
    bool loadTravelTime(uint32_t &travel_time_ms)
    {
        CurtainConfiguration config = {};
        if (!loadConfig(config, sizeof(config), true)) {
            logger_.info("No curtain config in flash");
            return false;
        }

        if (config.magic != CURTAIN_CONFIG_MAGIC) {
            logger_.info("No valid curtain config (magic mismatch)");
            return false;
        }

        if (!verifyCRC(config, config.crc32, offsetof(CurtainConfiguration, crc32))) {
            logger_.warn("Curtain config CRC mismatch");
            return false;
        }

        travel_time_ms = config.travel_time_ms;
        logger_.info("Loaded travel time: %lu ms", travel_time_ms);
        return true;
    }

private:
    Logger logger_;
};
