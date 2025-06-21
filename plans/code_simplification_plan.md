e# Code Simplification Plan

## Overview
This plan outlines specific opportunities to simplify the Bramble codebase, reducing complexity and improving maintainability. Based on a comprehensive analysis, these changes could reduce the codebase by 20-25% (approximately 1,200-1,500 lines) while improving type safety, testability, and code quality.

## 1. HAL Layer Simplifications (Highest Impact)

### Flash Class Improvements
- **Extract Retry Logic** (~50 lines saved)
  ```cpp
  // Create a generic retry template:
  template<typename Operation>
  FlashResult retryOperation(Operation op, uint32_t max_retries, const char* op_name);
  ```
  
- **Consolidate Alignment Functions** (~20 lines saved)
  ```cpp
  // Replace 4 functions with:
  template<uint32_t Boundary>
  bool isAligned(uint32_t value) const { return (value % Boundary) == 0; }
  ```

- **Remove Dynamic Allocation in Verification** (~30 lines saved)
  - Read directly from flash memory without allocating verify buffer
  - Use direct memory comparison

### NeoPixel Class Improvements
- **Replace malloc with std::vector** (~20 lines saved)
  ```cpp
  std::vector<neopixel_color_t> pixels;  // Instead of manual memory management
  ```

- **Fix setBrightness Bug**
  - Store brightness separately
  - Apply during show() to preserve original color values

- **Simplify HSV Conversion** (~40 lines saved)
  - Use lookup table or simplified algorithm
  - Remove deeply nested conditionals

### Logger Class Improvements
- **Template Log Level Functions** (~40 lines saved)
  ```cpp
  template<LogLevel level>
  void log(const char* fmt, ...) const;
  ```

## 2. LoRa Component Simplifications (High Impact)

### Message Creation Consolidation
- **Template for Message Creation** (~150 lines saved)
  ```cpp
  template<typename PayloadType>
  size_t createMessage(MessageType type, uint8_t flags, uint16_t src, uint16_t dst, 
                      uint8_t seq, uint8_t* buffer, 
                      std::function<void(PayloadType*)> setupPayload);
  ```

- **Consolidate Payload Getters** (~40 lines saved)
  ```cpp
  template<MessageType type, typename PayloadType>
  const PayloadType* getPayload(const Message* message);
  ```

### Validation Logic Improvements
- **Extract Common Address Validation** (~50 lines saved)
  ```cpp
  bool isValidAddress(uint16_t addr);
  bool isValidNodeAddress(uint16_t addr);
  ```

- **Create Validation Lookup Table** (~30 lines saved)
  ```cpp
  static constexpr size_t PAYLOAD_SIZES[] = {
      [MSG_TYPE_SENSOR_DATA] = sizeof(SensorPayload),
      [MSG_TYPE_ACTUATOR_CMD] = 3,  // min size
      // ...
  };
  ```

### Retry Logic Refactoring
- **Policy-Based Retry System** (~100 lines saved)
  ```cpp
  class RetryPolicy {
      virtual bool shouldRetry(int attempts, bool isCritical) = 0;
      virtual uint32_t getNextDelay(int attempts) = 0;
  };
  ```

## 3. Main Application Refactoring (Medium Impact)

### Extract Common Loop Framework
- **Base Loop Handler** (~200 lines saved)
  ```cpp
  class ApplicationMode {
      virtual void updateLED() = 0;
      virtual void handlePeriodicTasks(uint32_t current_time) = 0;
      virtual void processMessage(const Message& msg) = 0;
  };
  ```

- **LED Pattern Classes** (~50 lines saved)
  ```cpp
  class LEDPattern {
      virtual Color getColor(uint32_t time) = 0;
  };
  
  class BreathingPattern : public LEDPattern { /* Hub mode */ };
  class HeartbeatPattern : public LEDPattern { /* Node modes */ };
  class ColorCyclePattern : public LEDPattern { /* Demo mode */ };
  ```

- **Periodic Task Manager** (~80 lines saved)
  ```cpp
  class PeriodicTaskManager {
      void addTask(std::function<void()> task, uint32_t interval_ms);
      void update(uint32_t current_time);
  };
  ```

### Message Processing Improvements
- **Strategy Pattern for Message Handling** (~60 lines saved)
  ```cpp
  using MessageHandler = std::function<void(const Message&)>;
  std::map<MessageType, MessageHandler> handlers;
  ```

## 4. Configuration Management (Low Impact)

### Common Base Class
- **Shared CRC Calculation** (~30 lines saved)
  ```cpp
  class ConfigurationBase {
      static uint32_t calculateCRC32(const uint8_t* data, size_t length);
      virtual bool validate() const = 0;
  };
  ```

- **Template Save/Load Operations** (~50 lines saved)
  ```cpp
  template<typename ConfigType>
  class FlashConfigManager {
      bool save(const ConfigType& config);
      bool load(ConfigType& config);
  };
  ```

## 5. Cross-Cutting Improvements

### Modern C++ Features
- Replace `#define` with `constexpr`:
  ```cpp
  // Before:
  #define FLASH_SECTOR_SIZE 4096
  
  // After:
  static constexpr uint32_t FLASH_SECTOR_SIZE = 4096;
  ```

- Use `enum class` instead of plain `enum`:
  ```cpp
  // Before:
  enum FlashResult { FLASH_SUCCESS = 0, ... };
  
  // After:
  enum class FlashResult { Success = 0, ... };
  ```

### Consistent Error Handling
- Create error handling framework:
  ```cpp
  template<typename T>
  class Result {
      bool success;
      T value;
      std::string error;
  };
  ```

## Implementation Plan

### Phase 1: HAL Simplifications (Week 1)
1. Refactor Flash class (extract retry logic, consolidate alignment)
2. Modernize NeoPixel class (fix brightness bug, use std::vector)
3. Simplify Logger class (template functions)

### Phase 2: Main Application (Week 2)
1. Extract common loop framework
2. Implement LED pattern classes
3. Create periodic task manager

### Phase 3: LoRa Components (Week 3)
1. Template message creation functions
2. Consolidate validation logic
3. Implement policy-based retry system

### Phase 4: Configuration & Polish (Week 4)
1. Create configuration base class
2. Apply cross-cutting improvements
3. Update documentation

## Expected Outcomes

- **Code Reduction**: 1,200-1,500 lines (20-25%)
- **Improved Maintainability**: Significantly reduced duplication
- **Better Testability**: Extracted components easier to unit test
- **Enhanced Type Safety**: Modern C++ features throughout
- **Performance**: Slight improvements from removing dynamic allocations

## Risk Mitigation

- Each phase is independent and can be tested separately
- Changes maintain existing functionality
- Incremental approach allows for easy rollback
- Comprehensive testing after each phase