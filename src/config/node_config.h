#pragma once

#include <stdint.h>

#include <cstddef>

#include "config_base.h"
#include "hal/flash.h"
#include "hal/logger.h"

/**
 * @brief Node configuration data stored in flash
 *
 * High-level abstraction for storing node configuration
 * in the last sector of QSPI flash memory
 */

// Configuration data structure (exactly one flash page)
struct __attribute__((packed)) NodeConfiguration {
    uint32_t magic;              // Magic number to validate data (0xBEEF1234)
    uint16_t assigned_address;   // Assigned network address
    uint64_t device_id;          // Device unique ID
    uint32_t registration_time;  // DEPRECATED - kept for flash compatibility, always 0
    char device_name[16];        // Device name
    uint8_t node_type;           // Node type
    uint8_t capabilities;        // Node capabilities
    uint16_t firmware_version;   // Firmware version at registration
    uint32_t crc32;              // CRC32 of all above fields
    uint8_t padding[214];        // Pad to 256 bytes (1 flash page)
};

static_assert(sizeof(NodeConfiguration) == FLASH_PAGE_SIZE,
              "NodeConfiguration must be exactly one flash page");

// Magic number for valid configuration
constexpr uint32_t NODE_CONFIG_MAGIC = 0xBEEF1234;

/**
 * @brief High-level interface for node configuration persistence
 *
 * Manages loading/saving node configuration to QSPI flash
 * Uses the last sector of flash for configuration storage
 */
class NodeConfigManager : public ConfigurationBase {
public:
    explicit NodeConfigManager(Flash &flash_hal);

    /**
     * @brief Check if valid configuration exists in flash
     * @return true if valid configuration found
     */
    bool hasValidConfiguration();

    /**
     * @brief Load configuration from flash
     * @param config Output configuration structure
     * @return true if load successful and valid
     */
    bool loadConfiguration(NodeConfiguration &config);

    /**
     * @brief Save configuration to flash
     * @param config Configuration to save
     * @return true if save successful
     */
    bool saveConfiguration(const NodeConfiguration &config);

    /**
     * @brief Clear configuration from flash
     * @return true if clear successful
     */
    bool clearConfiguration();

    /**
     * @brief Create default configuration
     * @param device_id Device unique ID
     * @param device_name Device name
     * @param node_type Node type
     * @param capabilities Node capabilities
     * @param firmware_version Firmware version
     * @return Default configuration structure
     */
    static NodeConfiguration createDefaultConfiguration(uint64_t device_id, const char *device_name,
                                                        uint8_t node_type, uint8_t capabilities,
                                                        uint16_t firmware_version);

private:
    Logger logger_;  // Module logger

    /**
     * @brief Verify configuration CRC
     * @param config Configuration to verify
     * @return true if CRC matches
     */
    bool verifyCRC(const NodeConfiguration &config);

    /**
     * @brief Attempt to save configuration to backup location
     * @param config Configuration to save
     * @return true if backup save successful
     */
    bool attemptBackupSave(const NodeConfiguration &config);

    /**
     * @brief Try to recover configuration from backup location
     * @param config Output configuration
     * @return true if recovery successful
     */
    bool attemptBackupRecovery(NodeConfiguration &config);
};