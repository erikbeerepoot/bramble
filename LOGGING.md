# Bramble Logging System

## Quick Start

### 1. Add Logger to Your Class
```cpp
// In your .h file
#include "hal/logger.h"

class MyClass {
private:
    Logger logger_;
public:
    MyClass() : logger_("MODULE_NAME") {}
};
```

### 2. Use Logging Methods
```cpp
// Replace old printf statements:
printf("Error: failed to connect\n");
logger_.error("Failed to connect");

// Different log levels:
logger_.error("Critical error occurred");     // Always shown (unless LOG_NONE)
logger_.warn("Something suspicious");         // Warnings and above
logger_.info("General information");          // Info level and above  
logger_.debug("Detailed debug info");         // Debug level only
```

### 3. Set Log Levels
```cpp
// In main():
Logger::setLogLevel(LOG_DEBUG);   // Show everything (demo mode)
Logger::setLogLevel(LOG_WARN);    // Production: warnings/errors only
Logger::setLogLevel(LOG_ERROR);   // Critical errors only
Logger::setLogLevel(LOG_NONE);    // No logging (ultra low power)
```

## Output Format
```
INFO [FLASH]: Initialized with 8 MB flash
WARN [FLASH]: Write retry 2/3 at offset 0x007FF000
ERR [CONFIG]: Primary config magic mismatch (found 0x12345678, expected 0xBEEF1234)
DBG [LORA]: Message sent to 0x0001
```

## Power Optimization

### Battery-Powered Nodes
```cpp
// In main() for battery nodes:
Logger::setLogLevel(LOG_ERROR);        // Minimal logging
Logger::setUsbCheckEnabled(true);      // Only log when USB connected
```

### Development/Demo
```cpp
// In main() for development:
Logger::setLogLevel(LOG_DEBUG);        // Full verbosity
Logger::setUsbCheckEnabled(false);     // Always log
```

## Migration Guide

### Old Code:
```cpp
printf("Flash write failed at offset 0x%08x\n", offset);
```

### New Code:
```cpp
class Flash {
private:
    Logger logger_;
public:
    Flash() : logger_("FLASH") {}
    
    void someMethod() {
        logger_.error("Write failed at offset 0x%08lx", offset);
    }
};
```

## Recommended Module Names
- `"MAIN"` - Main application
- `"FLASH"` - Flash operations  
- `"CONFIG"` - Configuration management
- `"LORA"` - LoRa radio operations
- `"NETWORK"` - Network/routing
- `"SENSOR"` - Sensor readings
- `"ACTUATOR"` - Actuator control

## Best Practices

1. **Use appropriate levels**:
   - `error()`: System failures, critical issues
   - `warn()`: Retry attempts, recoverable issues  
   - `info()`: Normal operations, status updates
   - `debug()`: Detailed internal state

2. **Keep module names short** (8 chars max for clean formatting)

3. **No newlines needed** (logger adds them automatically)

4. **Production deployment**:
   ```cpp
   #ifdef PRODUCTION_BUILD
   Logger::setLogLevel(LOG_ERROR);
   #else  
   Logger::setLogLevel(LOG_DEBUG);
   #endif
   ```

## Power Impact

**LOG_DEBUG**: ~2-5mA extra power consumption  
**LOG_INFO**: ~1-2mA extra power consumption  
**LOG_WARN**: ~0.5mA extra power consumption  
**LOG_ERROR**: ~0.1mA extra power consumption  
**LOG_NONE**: Zero logging overhead

For **battery-powered farm sensors**, use `LOG_ERROR` in production!