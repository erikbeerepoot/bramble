#include "address_manager.h"
#include "../config/hub_config.h"
#include "../hal/logger.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <cstring>

static Logger logger("AddressManager");

AddressManager::AddressManager() : next_available_address_(ADDRESS_MIN_NODE) {
    // Initialize with first available node address
}

uint16_t AddressManager::registerNode(uint64_t device_id, uint8_t node_type, uint8_t capabilities,
                                      uint16_t firmware_version, const char* device_name) {
    // Check if device is already registered
    if (isDeviceRegistered(device_id)) {
        uint16_t existing_address = getDeviceAddress(device_id);
        printf("Device 0x%016llx already registered with address 0x%04x\n", 
               (unsigned long long)device_id, existing_address);
        
        // Update node info with potentially new capabilities/firmware
        NodeInfo* node = &node_registry_[existing_address];
        node->node_type = node_type;
        node->capabilities = capabilities;
        node->firmware_version = firmware_version;
        if (device_name) {
            strncpy(node->device_name, device_name, sizeof(node->device_name) - 1);
            node->device_name[sizeof(node->device_name) - 1] = '\0';
        }
        node->last_seen_time = getCurrentTime();
        node->is_active = true;
        
        return existing_address;
    }
    
    // Check if address space is full
    if (isAddressSpaceFull()) {
        printf("Address space full! Cannot register new device\n");
        return 0x0000;
    }
    
    // Find next available address
    uint16_t assigned_address = findNextAvailableAddress();
    if (assigned_address == 0x0000) {
        printf("No available addresses!\n");
        return 0x0000;
    }
    
    // Create new node info
    NodeInfo new_node;
    new_node.device_id = device_id;
    new_node.assigned_address = assigned_address;
    new_node.node_type = node_type;
    new_node.capabilities = capabilities;
    new_node.firmware_version = firmware_version;
    if (device_name) {
        strncpy(new_node.device_name, device_name, sizeof(new_node.device_name) - 1);
        new_node.device_name[sizeof(new_node.device_name) - 1] = '\0';
    } else {
        strcpy(new_node.device_name, "Unknown");
    }
    new_node.registration_time = getCurrentTime();
    new_node.last_seen_time = new_node.registration_time;
    new_node.is_active = true;
    
    // Register the node
    node_registry_[assigned_address] = new_node;
    device_to_address_[device_id] = assigned_address;
    
    printf("Registered device 0x%016llx as '%s' with address 0x%04x\n", 
           (unsigned long long)device_id, new_node.device_name, assigned_address);
    
    return assigned_address;
}

bool AddressManager::isDeviceRegistered(uint64_t device_id) {
    return device_to_address_.find(device_id) != device_to_address_.end();
}

uint16_t AddressManager::getDeviceAddress(uint64_t device_id) {
    auto it = device_to_address_.find(device_id);
    if (it != device_to_address_.end()) {
        return it->second;
    }
    return 0x0000;
}

const NodeInfo* AddressManager::getNodeInfo(uint16_t address) {
    auto it = node_registry_.find(address);
    if (it != node_registry_.end()) {
        return &it->second;
    }
    return nullptr;
}

const NodeInfo* AddressManager::getNodeInfoByDeviceId(uint64_t device_id) {
    uint16_t address = getDeviceAddress(device_id);
    if (address != 0x0000) {
        return getNodeInfo(address);
    }
    return nullptr;
}

void AddressManager::updateLastSeen(uint16_t address, uint32_t current_time) {
    auto it = node_registry_.find(address);
    if (it != node_registry_.end()) {
        it->second.last_seen_time = current_time;
        it->second.is_active = true;
    }
}

bool AddressManager::unregisterNode(uint16_t address) {
    auto it = node_registry_.find(address);
    if (it != node_registry_.end()) {
        uint64_t device_id = it->second.device_id;
        device_to_address_.erase(device_id);
        node_registry_.erase(it);
        
        printf("Unregistered node at address 0x%04x\n", address);
        return true;
    }
    return false;
}

std::vector<uint16_t> AddressManager::getActiveNodes() {
    std::vector<uint16_t> active_nodes;
    for (const auto& [address, node_info] : node_registry_) {
        if (node_info.is_active) {
            active_nodes.push_back(address);
        }
    }
    return active_nodes;
}

uint32_t AddressManager::checkForInactiveNodes(uint32_t current_time, uint32_t timeout_ms) {
    uint32_t inactive_count = 0;
    
    for (auto& [address, node_info] : node_registry_) {
        if (node_info.is_active && 
            (current_time - node_info.last_seen_time) > timeout_ms) {
            node_info.is_active = false;
            inactive_count++;
            printf("Node 0x%04x (%s) marked as inactive\n", 
                   address, node_info.device_name);
        }
    }
    
    return inactive_count;
}

uint32_t AddressManager::getActiveNodeCount() {
    uint32_t count = 0;
    for (const auto& [address, node_info] : node_registry_) {
        if (node_info.is_active) {
            count++;
        }
    }
    return count;
}

void AddressManager::printNetworkStatus() {
    printf("\n=== Network Status ===\n");
    printf("Registered nodes: %d\n", getRegisteredNodeCount());
    printf("Active nodes: %d\n", getActiveNodeCount());
    printf("Next available address: 0x%04x\n", next_available_address_);
    printf("\nNode List:\n");
    printf("Address  Device ID          Name              Type  Caps  Active  Last Seen\n");
    printf("-------  ----------------  ----------------  ----  ----  ------  ---------\n");
    
    for (const auto& [address, node] : node_registry_) {
        printf("0x%04x  0x%016llx  %-16s  0x%02x  0x%02x  %-6s  %d ms ago\n",
               address,
               (unsigned long long)node.device_id,
               node.device_name,
               node.node_type,
               node.capabilities,
               node.is_active ? "Yes" : "No",
               getCurrentTime() - node.last_seen_time);
    }
    printf("\n");
}

uint16_t AddressManager::findNextAvailableAddress() {
    // Start from next_available_address_ and find first free slot
    while (next_available_address_ <= ADDRESS_MAX_NODE) {
        if (node_registry_.find(next_available_address_) == node_registry_.end()) {
            return next_available_address_++;
        }
        next_available_address_++;
    }
    
    // If we got here, check if any addresses were freed up
    for (uint16_t address = ADDRESS_MIN_NODE; address <= ADDRESS_MAX_NODE; address++) {
        if (node_registry_.find(address) == node_registry_.end()) {
            next_available_address_ = address + 1;
            return address;
        }
    }
    
    return 0x0000; // No addresses available
}

uint32_t AddressManager::getCurrentTime() {
    return to_ms_since_boot(get_absolute_time());
}

bool AddressManager::persist(Flash& flash) {
    logger.info("Persisting address manager registry");
    
    // Create hub registry structure
    HubRegistry registry = {};
    registry.header.magic = 0xBEEF5678;
    registry.header.version = 1;
    registry.header.next_address = next_available_address_;
    registry.header.node_count = 0;
    registry.header.save_time = getCurrentTime();
    
    // Convert NodeInfo entries to RegistryNodeEntry format
    uint16_t node_index = 0;
    for (const auto& [address, node_info] : node_registry_) {
        if (node_index >= MAX_REGISTRY_NODES) {
            logger.error("Too many nodes to persist (%d max)", MAX_REGISTRY_NODES);
            break;
        }
        
        RegistryNodeEntry& entry = registry.nodes[node_index];
        entry.device_id = node_info.device_id;
        entry.assigned_address = node_info.assigned_address;
        entry.node_type = node_info.node_type;
        entry.capabilities = node_info.capabilities;
        entry.firmware_version = node_info.firmware_version;
        entry.registration_time = node_info.registration_time;
        entry.last_seen_time = node_info.last_seen_time;
        entry.is_active = node_info.is_active ? 1 : 0;
        
        strncpy(entry.device_name, node_info.device_name, sizeof(entry.device_name) - 1);
        entry.device_name[sizeof(entry.device_name) - 1] = '\0';
        
        node_index++;
    }
    
    registry.header.node_count = node_index;
    
    // Use HubConfigManager to save
    HubConfigManager config_manager(flash);
    bool success = config_manager.saveRegistry(registry);
    
    if (success) {
        logger.info("Successfully persisted %d nodes", node_index);
    } else {
        logger.error("Failed to persist registry");
    }
    
    return success;
}

bool AddressManager::load(Flash& flash) {
    logger.info("Loading address manager registry from persistent storage");
    
    // Use HubConfigManager to load
    HubConfigManager config_manager(flash);
    HubRegistry registry;
    
    if (!config_manager.loadRegistry(registry)) {
        logger.warn("No valid registry found in storage, starting with empty registry");
        return false;
    }
    
    // Clear existing registry
    node_registry_.clear();
    device_to_address_.clear();
    
    // Restore next available address
    next_available_address_ = registry.header.next_address;
    
    // Convert RegistryNodeEntry entries back to NodeInfo format
    for (uint16_t i = 0; i < registry.header.node_count; i++) {
        const RegistryNodeEntry& entry = registry.nodes[i];
        
        NodeInfo node_info;
        node_info.device_id = entry.device_id;
        node_info.assigned_address = entry.assigned_address;
        node_info.node_type = entry.node_type;
        node_info.capabilities = entry.capabilities;
        node_info.firmware_version = entry.firmware_version;
        node_info.registration_time = entry.registration_time;
        node_info.last_seen_time = entry.last_seen_time;
        node_info.is_active = (entry.is_active != 0);
        
        strncpy(node_info.device_name, entry.device_name, sizeof(node_info.device_name) - 1);
        node_info.device_name[sizeof(node_info.device_name) - 1] = '\0';
        
        // Add to registries
        node_registry_[node_info.assigned_address] = node_info;
        device_to_address_[node_info.device_id] = node_info.assigned_address;
    }
    
    logger.info("Successfully loaded %d nodes from persistent storage", registry.header.node_count);
    return true;
}