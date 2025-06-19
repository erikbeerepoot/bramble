#include "hub_config.h"
#include "../hal/logger.h"
#include "../hal/flash.h"
#include <string.h>
#include <stdio.h>

static Logger logger("HubConfig");

// Simple CRC32 implementation for data integrity
static uint32_t calculateCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    
    return ~crc;
}

bool HubConfigManager::saveRegistry(const HubRegistry& registry) {
    logger.info("Saving hub registry to flash (nodes=%d)", registry.header.node_count);
    
    // Calculate CRC32 (excluding the CRC field itself)
    size_t data_size = sizeof(HubRegistryHeader) - sizeof(uint32_t) + 
                      sizeof(RegistryNodeEntry) * registry.header.node_count;
    
    HubRegistry temp_registry = registry;
    temp_registry.header.crc32 = calculateCRC32((const uint8_t*)&temp_registry, data_size);
    
    // Erase flash sectors for registry
    size_t erase_size = ((sizeof(HubRegistry) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
    if (flash_.erase(REGISTRY_FLASH_OFFSET, erase_size) != FLASH_SUCCESS) {
        logger.error("Failed to erase flash for registry");
        return false;
    }
    
    // Write registry data
    size_t total_size = sizeof(HubRegistry);
    if (flash_.write(REGISTRY_FLASH_OFFSET, (const uint8_t*)&temp_registry, total_size) != FLASH_SUCCESS) {
        logger.error("Failed to write registry to flash");
        return false;
    }
    
    logger.info("Hub registry saved successfully");
    return true;
}

bool HubConfigManager::loadRegistry(HubRegistry& registry) {
    logger.info("Loading hub registry from flash");
    
    // Read registry from flash
    size_t total_size = sizeof(HubRegistry);
    if (flash_.read(REGISTRY_FLASH_OFFSET, (uint8_t*)&registry, total_size) != FLASH_SUCCESS) {
        logger.error("Failed to read registry from flash");
        return false;
    }
    
    // Validate magic number
    if (registry.header.magic != REGISTRY_MAGIC) {
        logger.warn("Invalid registry magic (0x%08X), expected 0x%08X", 
                    registry.header.magic, REGISTRY_MAGIC);
        return false;
    }
    
    // Validate version
    if (registry.header.version != REGISTRY_VERSION) {
        logger.warn("Registry version mismatch (%d), expected %d", 
                    registry.header.version, REGISTRY_VERSION);
        return false;
    }
    
    // Validate node count
    if (registry.header.node_count > MAX_REGISTRY_NODES) {
        logger.error("Invalid node count: %d (max %d)", 
                     registry.header.node_count, MAX_REGISTRY_NODES);
        return false;
    }
    
    // Verify CRC32
    size_t data_size = sizeof(HubRegistryHeader) - sizeof(uint32_t) + 
                      sizeof(RegistryNodeEntry) * registry.header.node_count;
    uint32_t saved_crc = registry.header.crc32;
    registry.header.crc32 = 0;
    uint32_t calc_crc = calculateCRC32((const uint8_t*)&registry, data_size);
    
    if (saved_crc != calc_crc) {
        logger.error("Registry CRC mismatch (saved=0x%08X, calc=0x%08X)", 
                     saved_crc, calc_crc);
        return false;
    }
    
    registry.header.crc32 = saved_crc;
    logger.info("Hub registry loaded successfully (nodes=%d)", registry.header.node_count);
    return true;
}

bool HubConfigManager::clearRegistry() {
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