#include "hub_mode.h"
#include <cstdio>
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"
#include "lora/sx1276.h"
#include "hal/flash.h"

constexpr uint32_t STATS_INTERVAL_MS = 30000;      // 30 seconds
constexpr uint32_t MAINTENANCE_INTERVAL_MS = 300000; // 5 minutes

void HubMode::onStart() {
    printf("=== HUB MODE ACTIVE ===\n");
    printf("- Managing node registrations\n");
    printf("- Routing node-to-node messages\n");
    printf("- Blue LED indicates hub status\n");
    
    // Hub always uses blue breathing pattern
    led_pattern_ = std::make_unique<BreathingPattern>(led_, 0, 0, 255);
    
    // Add periodic stats reporting
    task_manager_.addTask(
        [this](uint32_t time) {
            uint32_t routed, queued, dropped;
            hub_router_->getRoutingStats(routed, queued, dropped);
            printf("Hub stats - Routed: %lu, Queued: %lu, Dropped: %lu\n", 
                   routed, queued, dropped);
            
            printf("Registered nodes: %u\n", address_manager_->getRegisteredNodeCount());
            
            // Print network statistics if available
            if (network_stats_) {
                // Update node counts
                network_stats_->updateNodeCounts(
                    address_manager_->getRegisteredNodeCount(),
                    address_manager_->getActiveNodeCount(),
                    address_manager_->getRegisteredNodeCount() - address_manager_->getActiveNodeCount()
                );
                network_stats_->printSummary();
            }
        },
        STATS_INTERVAL_MS,
        "Stats Reporting"
    );
    
    // Add periodic maintenance
    task_manager_.addTask(
        [this](uint32_t time) {
            printf("Performing hub maintenance...\n");
            hub_router_->clearOldRoutes(time);
            hub_router_->processQueuedMessages();
            
            // Check for inactive nodes and update network status
            uint32_t inactive_count = address_manager_->checkForInactiveNodes(time);
            if (inactive_count > 0) {
                printf("Marked %lu nodes as inactive\n", inactive_count);
            }
            
            // Deregister nodes that have been inactive for extended period
            uint32_t deregistered_count = address_manager_->deregisterInactiveNodes(time);
            if (deregistered_count > 0) {
                printf("Deregistered %lu nodes (inactive > %lu hours)\n", 
                       deregistered_count, 86400000UL / 3600000UL);
                // Persist the updated registry to flash
                Flash flash_hal;
                address_manager_->persist(flash_hal);
            }
        },
        MAINTENANCE_INTERVAL_MS,
        "Hub Maintenance"
    );
}

void HubMode::processIncomingMessage(uint8_t* rx_buffer, int rx_len, uint32_t current_time) {
    printf("Hub received message (len=%d, RSSI=%d dBm)\n", 
           rx_len, lora_.getRssi());
    
    // Call base class implementation which handles common processing
    ApplicationMode::processIncomingMessage(rx_buffer, rx_len, current_time);
}