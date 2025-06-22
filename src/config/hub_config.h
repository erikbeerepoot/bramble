#pragma once

#include <stdint.h>
#include "config_base.h"
#include "../hal/flash.h"
#include "../lora/message.h"

/**
 * @brief Hub registry data stored in flash
 * 
 * This structure stores the hub's node registry for persistence
 * across reboots. It's designed to fit in flash sectors.
 */

// Maximum number of nodes to store in registry
constexpr uint32_t MAX_REGISTRY_NODES = 100;

// Registry entry for a single node
struct __attribute__((packed)) RegistryNodeEntry {
    uint64_t device_id;           // Device unique ID
    uint16_t assigned_address;    // Assigned network address
    uint8_t node_type;            // Node type
    uint8_t capabilities;         // Node capabilities
    uint16_t firmware_version;    // Firmware version
    uint32_t registration_time;   // DEPRECATED - kept for flash compatibility, always 0
    uint32_t last_seen_time;      // Last communication time (since boot)
    uint32_t inactive_duration_ms; // Accumulated inactive time (survives reboots)
    char device_name[16];         // Device name
    uint8_t is_active;            // Active status
    uint8_t reserved[3];          // Padding for alignment
};

// Hub registry header
struct __attribute__((packed)) HubRegistryHeader {
    uint32_t magic;               // Magic number (0xBEEF5678)
    uint32_t version;             // Registry format version
    uint16_t next_address;        // Next available address to assign
    uint16_t node_count;          // Number of nodes in registry
    uint32_t save_time;           // When registry was saved
    uint32_t crc32;               // CRC32 of registry data
};

// Complete hub registry structure
struct __attribute__((packed)) HubRegistry {
    HubRegistryHeader header;
    RegistryNodeEntry nodes[MAX_REGISTRY_NODES];
    uint8_t padding[256];         // Padding to ensure flash sector alignment
};

// Configuration management for hub
class HubConfigManager : public ConfigurationBase {
public:
    explicit HubConfigManager(Flash& flash) 
        : ConfigurationBase(flash, 0x1F0000) {}  // 1.9MB offset
    
    /**
     * @brief Save hub registry to flash
     * @param registry Registry data to save
     * @return true if saved successfully
     */
    bool saveRegistry(const HubRegistry& registry);
    
    /**
     * @brief Load hub registry from flash
     * @param registry Registry data to load into
     * @return true if loaded successfully
     */
    bool loadRegistry(HubRegistry& registry);
    
    /**
     * @brief Clear saved registry
     * @return true if cleared successfully
     */
    bool clearRegistry();
    
private:
    static constexpr uint32_t REGISTRY_MAGIC = 0xBEEF5678;
    static constexpr uint32_t REGISTRY_VERSION = 1;
};