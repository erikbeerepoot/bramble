# Node Address Change/Reassignment Plan

## Overview
This document outlines the plan for implementing dynamic address reassignment capability, allowing the hub to change node addresses without physical access.

## Use Cases
1. **Address Conflicts**: When two nodes accidentally have the same address (e.g., from cloning flash)
2. **Network Reorganization**: Admin wants to reassign addresses for better organization
3. **Address Recovery**: Reclaim addresses from permanently offline nodes
4. **Migration**: Moving a node from one network segment to another

## Implementation Design

### 1. New Message Types
```cpp
// Add to message.h
MSG_TYPE_ADDR_CHANGE_REQ = 0x12,  // Hub requests node to change address
MSG_TYPE_ADDR_CHANGE_ACK = 0x13,  // Node acknowledges address change
```

### 2. Message Payload Structures
```cpp
// Address change request from hub to node
struct AddressChangeRequestPayload {
    uint64_t device_id;      // Target device
    uint16_t new_address;    // New address to assign
    uint16_t reason_code;    // Why the change is happening
};

// Address change acknowledgment from node to hub
struct AddressChangeAckPayload {
    uint64_t device_id;      
    uint16_t old_address;    // Previous address for confirmation
    uint16_t new_address;    // Accepted new address
    uint8_t status;          // Success/failure status
};

// Reason codes
enum AddressChangeReason {
    REASON_CONFLICT = 0x01,      // Address conflict detected
    REASON_REORGANIZE = 0x02,    // Network reorganization
    REASON_RECOVERY = 0x03,      // Recovering unused address
    REASON_MIGRATION = 0x04      // Node migration
};
```

### 3. Hub-Side Implementation

#### AddressManager Enhancement
```cpp
class AddressManager {
    // Existing methods...
    
    /**
     * Reassign a node to a new address
     * @param device_id Device to reassign
     * @param new_address New address to assign
     * @param reason Reason code for the change
     * @return true if reassignment successful
     */
    bool reassignNodeAddress(uint64_t device_id, uint16_t new_address, uint16_t reason);
    
private:
    // Track pending reassignments
    std::map<uint64_t, PendingReassignment> pending_reassignments_;
};

struct PendingReassignment {
    uint16_t old_address;
    uint16_t new_address;
    uint32_t request_time;
    uint16_t reason;
};
```

#### Implementation Steps
1. Verify device exists in registry
2. Check new address is available
3. Reserve new address to prevent conflicts
4. Send address change request via ReliableMessenger
5. Wait for acknowledgment (with timeout)
6. Update internal registry on success
7. Persist changes to flash
8. Rollback on failure

### 4. Node-Side Implementation

#### Message Processing
```cpp
// In processIncomingMessage for nodes
if (header->type == MSG_TYPE_ADDR_CHANGE_REQ) {
    const AddressChangeRequestPayload* change_req = 
        MessageHandler::getAddressChangeRequestPayload(&message);
    
    if (change_req && change_req->device_id == device_id) {
        // Validate request is from hub
        if (header->src_addr != ADDRESS_HUB) {
            main_logger.warn("Address change request from non-hub: 0x%04X", header->src_addr);
            return;
        }
        
        uint16_t old_address = messenger.getNodeAddress();
        
        // Update configuration
        NodeConfiguration config;
        if (config_manager.loadConfiguration(config)) {
            config.assigned_address = change_req->new_address;
            
            if (config_manager.saveConfiguration(config)) {
                // Update messenger
                messenger.updateNodeAddress(change_req->new_address);
                
                // Send acknowledgment
                messenger.sendAddressChangeAck(
                    ADDRESS_HUB,
                    device_id,
                    old_address,
                    change_req->new_address,
                    ACK_SUCCESS
                );
                
                main_logger.info("Address changed from 0x%04X to 0x%04X (reason: %d)",
                    old_address, change_req->new_address, change_req->reason_code);
            } else {
                // Flash write failed
                messenger.sendAddressChangeAck(
                    ADDRESS_HUB,
                    device_id,
                    old_address,
                    old_address,  // Still using old address
                    ACK_FLASH_ERROR
                );
            }
        }
    }
}
```

### 5. Safety and Reliability

#### Atomic Operations
- Address change must be all-or-nothing
- No partial states where node and hub disagree

#### Failure Handling
- Timeout on hub side if no acknowledgment
- Rollback reservation if reassignment fails
- Node continues with old address if flash write fails

#### Conflict Prevention
- Hub ensures new address isn't already assigned
- Temporary reservation prevents race conditions
- Validation of all messages to prevent spoofing

### 6. Administrative Interface

#### Serial Console Commands
```
# Manual address reassignment
reassign <device_id> <new_address>

# Show pending reassignments
show pending-reassignments

# Cancel pending reassignment
cancel reassignment <device_id>
```

#### Automatic Triggers
- Conflict detection during registration
- Periodic cleanup of unused addresses
- Load balancing across address ranges

### 7. Testing Strategy

1. **Basic Reassignment**: Single node address change
2. **Conflict Resolution**: Two nodes with same address
3. **Failure Cases**: 
   - Network loss during reassignment
   - Flash write failure
   - Invalid new address
4. **Concurrent Operations**: Multiple reassignments
5. **Edge Cases**:
   - Reassigning hub address (should fail)
   - Reassigning to broadcast address (should fail)
   - Reassigning non-existent device

### 8. Future Enhancements

1. **Batch Reassignment**: Change multiple nodes at once
2. **Address Ranges**: Assign nodes to specific ranges based on type
3. **Automatic Optimization**: Reorganize addresses for routing efficiency
4. **History Tracking**: Log all address changes for audit

## Implementation Priority
MEDIUM - This is a nice-to-have feature that improves network management but isn't critical for basic operation. Should be implemented after core functionality is stable.