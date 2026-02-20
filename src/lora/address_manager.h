#pragma once

#include <map>
#include <vector>

#include "../hal/flash.h"
#include "message.h"

/**
 * @brief Node registration information
 */
struct NodeInfo {
    uint64_t device_id;             // Unique device identifier
    uint16_t assigned_address;      // Assigned network address
    uint8_t node_type;              // Node type (sensor/actuator/hybrid)
    uint8_t capabilities;           // Node capabilities flags
    uint32_t firmware_version;      // Firmware version
    char device_name[16];           // Human readable name
    uint32_t last_seen_time;        // Last communication time (ms since boot)
    uint32_t last_check_time;       // Last time we checked/updated inactive duration
    uint32_t inactive_duration_ms;  // Total accumulated time inactive (survives reboots)
    bool is_active;                 // Node is currently active
};

/**
 * @brief Address allocation and node management for hub
 * Manages the network address space and tracks active nodes
 */
class AddressManager {
public:
    AddressManager();

    /**
     * @brief Register a new node and assign an address
     * @param device_id Unique device identifier
     * @param node_type Node type
     * @param capabilities Node capabilities
     * @param firmware_version Firmware version
     * @param device_name Device name
     * @return Assigned address (0x0000 if registration failed)
     */
    uint16_t registerNode(uint64_t device_id, uint8_t node_type, uint8_t capabilities,
                          uint32_t firmware_version, const char *device_name);

    /**
     * @brief Check if a device is already registered
     * @param device_id Device identifier to check
     * @return true if device is already registered
     */
    bool isDeviceRegistered(uint64_t device_id);

    /**
     * @brief Get assigned address for a device
     * @param device_id Device identifier
     * @return Assigned address (0x0000 if not found)
     */
    uint16_t getDeviceAddress(uint64_t device_id);

    /**
     * @brief Get node information by address
     * @param address Node address
     * @return Pointer to node info (nullptr if not found)
     */
    const NodeInfo *getNodeInfo(uint16_t address);

    /**
     * @brief Get node information by device ID
     * @param device_id Device identifier
     * @return Pointer to node info (nullptr if not found)
     */
    const NodeInfo *getNodeInfoByDeviceId(uint64_t device_id);

    /**
     * @brief Update last seen time for a node
     * @param address Node address
     * @param current_time Current time in milliseconds
     */
    void updateLastSeen(uint16_t address, uint32_t current_time);

    /**
     * @brief Unregister a node (free its address)
     * @param address Node address to unregister
     * @return true if node was unregistered successfully
     */
    bool unregisterNode(uint16_t address);

    /**
     * @brief Get list of all active nodes
     * @return Vector of active node addresses
     */
    std::vector<uint16_t> getActiveNodes();

    /**
     * @brief Get list of all registered node addresses
     * @return Vector of registered node addresses
     */
    std::vector<uint16_t> getRegisteredAddresses() const;

    /**
     * @brief Check for inactive nodes and mark them as such
     * @param current_time Current time in milliseconds
     * @param timeout_ms Timeout in milliseconds for considering node inactive
     * @return Number of nodes marked as inactive
     */
    uint32_t checkForInactiveNodes(uint32_t current_time,
                                   uint32_t timeout_ms = 1200000);  // 20 minutes default

    /**
     * @brief Deregister nodes that have been inactive for extended period
     * @param current_time Current time in milliseconds
     * @param deregister_timeout_ms Timeout for deregistering inactive nodes
     * @return Number of nodes deregistered
     */
    uint32_t deregisterInactiveNodes(
        uint32_t current_time, uint32_t deregister_timeout_ms = 86400000);  // 24 hours default

    /**
     * @brief Get total number of registered nodes
     */
    uint32_t getRegisteredNodeCount() const { return node_registry_.size(); }

    /**
     * @brief Get number of active nodes
     */
    uint32_t getActiveNodeCount();

    /**
     * @brief Check if address space is full
     */
    bool isAddressSpaceFull() const { return next_available_address_ > ADDRESS_MAX_NODE; }

    /**
     * @brief Print network status for debugging
     */
    void printNetworkStatus();

    /**
     * @brief Save registry for persistence
     * @param flash Flash interface to use
     * @return true if saved successfully
     */
    bool persist(Flash &flash);

    /**
     * @brief Load registry from persistent storage
     * @param flash Flash interface to use
     * @return true if loaded successfully
     */
    bool load(Flash &flash);

private:
    std::map<uint16_t, NodeInfo> node_registry_;      // Address -> NodeInfo mapping
    std::map<uint64_t, uint16_t> device_to_address_;  // DeviceID -> Address mapping
    uint16_t next_available_address_;                 // Next address to assign

    /**
     * @brief Find next available address
     * @return Next available address (0x0000 if none available)
     */
    uint16_t findNextAvailableAddress();

    /**
     * @brief Get current time in milliseconds
     */
    uint32_t getCurrentTime();
};