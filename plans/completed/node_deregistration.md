# Node Auto-Deregistration Plan

## Overview
Implement automatic address cleanup for nodes that have been inactive for extended periods, preventing address exhaustion in long-running systems.

## Primary Goals
1. **Address Space Management** - Reclaim addresses from permanently offline nodes
2. **Automatic Cleanup** - No manual intervention needed for failed/retired nodes
3. **Graceful Handling** - Give nodes adequate time before deregistration

## Two-Stage Timeout System

### Stage 1: Mark Inactive (5 minutes)
- Node missed ~5 heartbeats (heartbeat every 60s)
- Status changed from ACTIVE to INACTIVE
- Address retained, can reactivate immediately
- Hub stops attempting to route messages to node

### Stage 2: Auto-Deregister (24 hours)
- Node has been inactive for 24 hours
- Address freed for reuse by new nodes
- Node removed from hub registry completely
- Node must re-register if it returns

## Critical Issue: Stale Flash Addresses

### The Problem
1. Node saves assigned address to flash after registration
2. Node boots and loads address from flash
3. Node starts using address without checking if still valid
4. **Result**: Node uses deregistered address, causing conflicts

### Solution: Address Validation on Boot

#### Node Boot Sequence
```
1. Load saved configuration from flash
2. If address exists (!= UNREGISTERED):
   a. Send ADDR_VALIDATION_REQUEST to hub
   b. Wait for VALIDATION_RESPONSE (timeout: 5s)
   c. If valid: Use saved address
   d. If invalid/timeout: Clear address, re-register
3. If no saved address: Register as new node
```

#### New Message Types

**Address Validation Request (Node → Hub)**
```
Type: MSG_TYPE_ADDR_VALIDATION (0x10)
Payload:
- device_id: uint64_t
- claimed_address: uint16_t
```

**Address Validation Response (Hub → Node)**
```
Type: MSG_TYPE_ADDR_VALID_RESP (0x11)
Payload:
- device_id: uint64_t
- claimed_address: uint16_t
- is_valid: uint8_t (1=valid, 0=invalid)
```

## Implementation Details

### 1. Hub Side Implementation

#### Add to AddressManager
```cpp
// Auto-deregistration method
uint32_t deregisterInactiveNodes(uint32_t current_time, 
                                 uint32_t timeout_ms = 86400000); // 24 hours

// Address validation method
bool validateNodeAddress(uint64_t device_id, uint16_t claimed_address);
```

#### Hub Maintenance Loop
```cpp
// Called every 5 minutes
void performMaintenance() {
    // Stage 1: Mark nodes inactive after 5 minutes
    checkForInactiveNodes(current_time, 300000);
    
    // Stage 2: Deregister nodes inactive > 24 hours
    uint32_t deregistered = deregisterInactiveNodes(current_time);
    if (deregistered > 0) {
        printf("Deregistered %u nodes\n", deregistered);
        // Persist updated registry to flash
        address_manager.persist(flash);
    }
}
```

### 2. Node Side Implementation

#### Update Registration Flow
```cpp
bool validateOrRegister() {
    NodeConfiguration config;
    if (config_manager.loadConfiguration(config) && 
        config.assigned_address != ADDRESS_UNREGISTERED) {
        
        // Validate saved address with hub
        if (messenger.sendAddressValidation(HUB_ADDRESS, 
                                           config.device_id,
                                           config.assigned_address)) {
            // Wait for validation response
            if (waitForValidationResponse(5000)) {
                if (validation_valid) {
                    // Address still valid, use it
                    messenger.updateNodeAddress(config.assigned_address);
                    return true;
                }
            }
        }
        
        // Validation failed - clear saved address
        config.assigned_address = ADDRESS_UNREGISTERED;
        config_manager.saveConfiguration(config);
    }
    
    // Proceed with normal registration
    return attemptRegistration();
}
```

## Benefits

1. **No Manual Cleanup** - System self-maintains address space
2. **Handles Seasonal Equipment** - Winter shutdown nodes auto-removed
3. **Prevents Address Exhaustion** - Failed nodes don't waste addresses
4. **Conflict Prevention** - Nodes can't use stale addresses
5. **Simple Recovery** - Nodes just re-register when returning

## Testing Strategy

1. **Timeout Testing**
   - Verify 5-minute inactive marking
   - Verify 24-hour deregistration
   - Test edge cases around timeout boundaries

2. **Address Validation**
   - Valid address acceptance
   - Invalid address rejection
   - Timeout handling

3. **Persistence Testing**
   - Registry survives hub restart
   - Deregistered addresses available for reuse

4. **Conflict Testing**
   - Two nodes claiming same address
   - Rapid register/deregister cycles

## Implementation Order

1. Add address validation messages to protocol
2. Implement hub-side validation method
3. Implement hub-side auto-deregistration
4. Update node boot sequence for validation
5. Test complete flow
6. Add persistence after deregistration

## Configuration Options

```cpp
// Timeout constants (can be made runtime configurable later)
constexpr uint32_t NODE_INACTIVE_TIMEOUT_MS = 300000;    // 5 minutes
constexpr uint32_t NODE_DEREGISTER_TIMEOUT_MS = 86400000; // 24 hours
constexpr uint32_t ADDR_VALIDATION_TIMEOUT_MS = 5000;     // 5 seconds
```