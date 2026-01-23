#include "hub_config.h"

#include <stdio.h>
#include <string.h>

#include <cstddef>

#include "../hal/flash.h"
#include "../hal/logger.h"

static Logger logger("HubConfig");

bool HubConfigManager::saveRegistry(const HubRegistry &registry)
{
    logger.info("Saving hub registry to flash (nodes=%d)", registry.header.node_count);

    // Calculate CRC32 (excluding the CRC field itself)
    size_t data_size = sizeof(HubRegistryHeader) - sizeof(uint32_t) +
                       sizeof(RegistryNodeEntry) * registry.header.node_count;

    HubRegistry temp_registry = registry;
    temp_registry.header.crc32 = calculateCRC32((const uint8_t *)&temp_registry, data_size);

    // Use base class save method
    if (!saveConfig(temp_registry, sizeof(HubRegistry))) {
        logger.error("Failed to save registry");
        return false;
    }

    logger.info("Hub registry saved successfully");
    return true;
}

bool HubConfigManager::loadRegistry(HubRegistry &registry)
{
    logger.info("Loading hub registry from flash");

    // Use base class load method
    if (!loadConfig(registry, sizeof(HubRegistry))) {
        logger.error("Failed to read registry from flash");
        return false;
    }

    // Validate magic number
    if (registry.header.magic != REGISTRY_MAGIC) {
        logger.warn("Invalid registry magic (0x%08X), expected 0x%08X", registry.header.magic,
                    REGISTRY_MAGIC);
        return false;
    }

    // Validate version
    if (registry.header.version != REGISTRY_VERSION) {
        logger.warn("Registry version mismatch (%d), expected %d", registry.header.version,
                    REGISTRY_VERSION);
        return false;
    }

    // Validate node count
    if (registry.header.node_count > MAX_REGISTRY_NODES) {
        logger.error("Invalid node count: %d (max %d)", registry.header.node_count,
                     MAX_REGISTRY_NODES);
        return false;
    }

    // Verify CRC32
    size_t data_size = sizeof(HubRegistryHeader) - sizeof(uint32_t) +
                       sizeof(RegistryNodeEntry) * registry.header.node_count;
    uint32_t saved_crc = registry.header.crc32;
    registry.header.crc32 = 0;
    uint32_t calc_crc = calculateCRC32((const uint8_t *)&registry, data_size);

    if (saved_crc != calc_crc) {
        logger.error("Registry CRC mismatch (saved=0x%08X, calc=0x%08X)", saved_crc, calc_crc);
        return false;
    }

    registry.header.crc32 = saved_crc;
    logger.info("Hub registry loaded successfully (nodes=%d)", registry.header.node_count);
    return true;
}

bool HubConfigManager::clearRegistry()
{
    logger.info("Clearing hub registry");

    // Create empty registry
    HubRegistry empty_registry = {};
    empty_registry.header.magic = REGISTRY_MAGIC;
    empty_registry.header.version = REGISTRY_VERSION;
    empty_registry.header.next_address = ADDRESS_MIN_NODE;
    empty_registry.header.node_count = 0;
    empty_registry.header.save_time = 0;

    return saveRegistry(empty_registry);
}