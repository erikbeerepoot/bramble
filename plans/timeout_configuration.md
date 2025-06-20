# Timeout Configuration Analysis

## Current Timeouts in Bramble

### Node Operation Timeouts
1. **Heartbeat Interval**: 60 seconds (60000ms)
   - Nodes send heartbeat every minute
   - Location: `bramble.cpp:52`

2. **Sensor Data Interval**: 30 seconds (30000ms)  
   - Sensor readings sent every 30 seconds
   - Location: `bramble.cpp:51`

3. **Node Inactive Timeout**: 5 minutes (300000ms)
   - After missing heartbeats for 5 minutes, node marked inactive
   - Location: `address_manager.h:97` (default parameter)
   - This means: 5 missed heartbeats before marking inactive

### Message Delivery Timeouts
1. **ACK Timeout**: 5 seconds (5000ms)
   - Time to wait for acknowledgment before retry
   - Location: `reliable_messenger.h:10`

2. **Retry Base Delay**: 1 second (1000ms)
   - Base delay for exponential backoff
   - Location: `reliable_messenger.h:11`

3. **Max Retries**: 3 attempts
   - Non-critical messages give up after 3 retries
   - Critical messages keep retrying indefinitely
   - Location: `reliable_messenger.h:9`

### Registration Timeouts
1. **Registration Timeout**: 10 seconds (10000ms)
   - Time to wait for registration response per attempt
   - Location: `bramble.cpp:482`

2. **Registration Retry Delay**: 5 seconds (5000ms)
   - Delay between registration attempts
   - Location: `bramble.cpp:483`

3. **Max Registration Attempts**: 3
   - Give up after 3 failed attempts
   - Location: `bramble.cpp:481`

### Hub Maintenance
1. **Hub Maintenance Interval**: 5 minutes (300000ms)
   - Hub performs cleanup tasks every 5 minutes
   - Location: `bramble.cpp:241`

## Analysis & Recommendations

### Current Issues
1. **Node Inactive Detection is Too Slow**
   - 5 minutes = 5 missed heartbeats before detection
   - For critical applications, this is too long

2. **Fixed Timeouts**
   - All timeouts are hardcoded
   - Different nodes might need different intervals

### Recommended Improvements

#### 1. Faster Inactive Detection
```cpp
// Add to address_manager.h
static constexpr uint32_t NODE_INACTIVE_TIMEOUT_MS = 180000;  // 3 minutes (3 missed heartbeats)
static constexpr uint32_t NODE_DEAD_TIMEOUT_MS = 600000;      // 10 minutes (consider for removal)
```

#### 2. Configurable Heartbeat Grace Period
```cpp
// Allow 1.5x heartbeat interval before marking inactive
uint32_t inactive_timeout = heartbeat_interval_ms * 1.5;
```

#### 3. Add Warning State
```cpp
enum NodeState {
    NODE_ACTIVE,      // Recent communication
    NODE_WARNING,     // 1-2 missed heartbeats  
    NODE_INACTIVE,    // 3+ missed heartbeats
    NODE_DEAD         // No communication for extended period
};
```

#### 4. Timeout Configuration Structure
```cpp
struct NetworkTimeouts {
    uint32_t heartbeat_interval_ms = 60000;
    uint32_t sensor_interval_ms = 30000;
    uint32_t inactive_timeout_ms = 180000;  // 3 minutes
    uint32_t dead_timeout_ms = 600000;      // 10 minutes
    uint32_t ack_timeout_ms = 5000;
    uint8_t max_retries = 3;
};
```

## Practical Timeout Values

### For Production Farm Use
- **Heartbeat**: 60 seconds (current is good)
- **Inactive**: 3 minutes (3 missed heartbeats)
- **Dead/Remove**: 24 hours (for address recycling)
- **ACK Timeout**: 5 seconds (current is good)

### For Demo/Testing
- **Heartbeat**: 15 seconds (faster feedback)
- **Inactive**: 45 seconds (3 missed heartbeats)
- **Dead/Remove**: 5 minutes
- **ACK Timeout**: 2 seconds

## Implementation Priority
1. **HIGH**: Add automatic deregistration after extended inactivity (e.g., 24 hours)
2. **MEDIUM**: Add configurable timeouts based on mode
3. **LOW**: Implement node state transitions (warning/inactive/dead)
4. **FUTURE**: Per-node timeout configuration

## Proposed Deregistration Logic

### Two-Stage Timeout System
1. **Stage 1: Mark Inactive** (5 minutes)
   - Node marked as inactive but keeps its address
   - Can immediately reactivate on any message
   - No impact on network operations

2. **Stage 2: Auto-Deregister** (24 hours) 
   - Node has been inactive for 24 hours
   - Address is freed for reuse
   - Node removed from registry
   - If node returns, it must re-register (gets new address)

### Benefits
- Addresses aren't wasted on permanently offline nodes
- Seasonal equipment naturally cleaned up
- Failed nodes don't consume address space forever
- But gives plenty of time for temporary issues

### Implementation Approach
Add a method to AddressManager:
```cpp
uint32_t deregisterInactiveNodes(uint32_t current_time, uint32_t deregister_timeout_ms = 86400000); // 24 hours
```

Call this during hub maintenance (every 5 minutes) to check for nodes that have been inactive > 24 hours.