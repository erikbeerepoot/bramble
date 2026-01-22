# C++ Style Guide

This document defines the C++ coding standards for the bramble project.

## Quick Reference

| Element | Style | Example |
|---------|-------|---------|
| Classes/Structs | CamelCase | `MessageHandler`, `NeoPixel` |
| Public methods | camelCase | `sendMessage()`, `setPixel()` |
| Private members | snake_case_ | `flash_size_`, `logger_` |
| Local variables | snake_case | `buffer_size`, `retry_count` |
| Constants | UPPER_CASE | `MAX_RETRIES`, `FLASH_PAGE_SIZE` |
| Macros | PREFIX_UPPER | `SX1276_REG_FIFO` |
| Enum types | CamelCase | `enum class WorkType` |
| Enum values | CamelCase | `WorkType::BacklogTransmit` |
| Files | snake_case | `message_handler.cpp`, `message_handler.h` |

## File Organization

### Header Files

```cpp
#pragma once

#include <cstdint>           // System headers first
#include <vector>

#include "pico/stdlib.h"     // SDK headers second
#include "hardware/spi.h"

#include "hal/logger.h"      // Project headers last
#include "lora/message.h"
```

- Use `#pragma once` for header guards
- Group includes: system, SDK, project (separated by blank lines)
- Use `#include <header>` for system/SDK headers
- Use `#include "path/header.h"` for project headers

### Source Files

```cpp
#include "my_class.h"        // Own header first

#include <cstring>           // Then system headers

#include "pico/stdlib.h"     // Then SDK headers

#include "other/dependency.h" // Then other project headers
```

## Formatting

### Indentation and Spacing

- **Indentation:** 4 spaces (no tabs)
- **Line length:** 100 characters maximum
- **Blank lines:** Single blank line between logical sections

### Braces

Classes and structs - opening brace on same line:
```cpp
class MessageHandler {
public:
    void sendMessage();

private:
    Logger logger_;
};
```

Functions - opening brace on new line:
```cpp
void MessageHandler::sendMessage()
{
    // implementation
}
```

Control statements - always use braces:
```cpp
// Good
if (condition) {
    doSomething();
}

// Bad - no braces
if (condition)
    doSomething();
```

Short inline methods may be on one line:
```cpp
uint getCount() const { return count_; }
```

### Pointers and References

Pointer/reference binds to the type:
```cpp
const uint8_t *data;    // Pointer
const Config &config;   // Reference
```

## Naming Conventions

### Classes and Structs

Use CamelCase:
```cpp
class MessageHandler { };
struct SensorReading { };
```

### Methods and Functions

Public methods use camelCase:
```cpp
void sendMessage();
bool isValid() const;
size_t getBufferSize();
```

### Variables

Local variables use snake_case:
```cpp
size_t buffer_size = 256;
uint32_t retry_count = 0;
```

Private member variables use snake_case with trailing underscore:
```cpp
class Example {
private:
    uint32_t flash_size_;
    Logger logger_;
    bool initialized_;
};
```

### Constants and Macros

Use UPPER_CASE:
```cpp
constexpr uint16_t MESSAGE_MAGIC = 0xBEEF;
constexpr size_t MAX_PAYLOAD_SIZE = 247;

#define SX1276_REG_FIFO 0x00
```

### Enums

Use `enum class` with CamelCase:
```cpp
enum class WorkType : uint8_t {
    BacklogTransmit,
    ScheduledTransmit,
    ImmediateTransmit
};

enum class FlashResult {
    Success,
    InvalidAddress,
    WriteError
};
```

## Documentation

Use Doxygen-style comments for public APIs:

```cpp
/**
 * @brief Send a message to the specified destination
 *
 * @param destination Target node address
 * @param data Pointer to message payload
 * @param length Length of payload in bytes
 * @return true if message was queued successfully
 *
 * @note Message delivery is not guaranteed for best-effort messages
 */
bool sendMessage(uint16_t destination, const uint8_t *data, size_t length);
```

Common Doxygen tags:
- `@brief` - Short description (required)
- `@param` - Parameter description (required for each parameter)
- `@return` - Return value description (required if non-void)
- `@note` - Important usage notes
- `@warning` - Warnings about dangerous usage
- `@see` - Cross-references to related functions

For file headers:
```cpp
/**
 * @file message_handler.cpp
 * @brief Implementation of the LoRa message handling system
 */
```

## Modern C++ Guidelines

### Prefer enum class

```cpp
// Good - type safe
enum class State { Idle, Running, Error };

// Avoid - pollutes namespace
enum State { STATE_IDLE, STATE_RUNNING, STATE_ERROR };
```

### Use constexpr for compile-time constants

```cpp
// Good
constexpr size_t BUFFER_SIZE = 256;
constexpr uint32_t TIMEOUT_MS = 1000;

// Avoid
#define BUFFER_SIZE 256
```

### Use smart pointers

```cpp
// Good - clear ownership
std::unique_ptr<uint8_t[]> buffer;

// Avoid when ownership is unclear
uint8_t *buffer;
```

### Use auto judiciously

```cpp
// Good - type is obvious
auto handler = std::make_unique<MessageHandler>();
for (const auto &item : collection) { }

// Avoid - type is not obvious
auto result = process();  // What type is result?
```

### Apply const liberally

```cpp
// Good
const Config &getConfig() const;
void process(const uint8_t *data, size_t length);
```

### Use override for virtual methods

```cpp
class Derived : public Base {
public:
    void process() override;  // Compiler checks this actually overrides
};
```

## Embedded-Specific Guidelines

### Memory Management

- Avoid dynamic allocation in interrupt handlers and hot paths
- Prefer stack allocation for small, short-lived objects
- Pre-allocate buffers where possible
- Document memory requirements for critical functions

### Struct Packing

Use explicit packing for wire protocols:
```cpp
struct __attribute__((packed)) MessageHeader {
    uint16_t magic;
    uint8_t type;
    uint16_t src_addr;
    uint16_t dst_addr;
    uint8_t seq_num;
};

// Verify size at compile time
static_assert(sizeof(MessageHeader) == 8, "MessageHeader must be 8 bytes");
```

### Performance Considerations

- Use `inline` for small, frequently-called functions
- Prefer references over copies for large objects
- Use `constexpr` to move computation to compile time
- Be mindful of flash vs RAM usage

## Tooling

### Automatic Formatting

Format code using clang-format:
```bash
# Format a single file
clang-format -i src/path/to/file.cpp

# Check formatting without modifying
clang-format --dry-run --Werror src/path/to/file.cpp
```

### Static Analysis

Run clang-tidy for static analysis:
```bash
clang-tidy src/path/to/file.cpp
```

### Editor Integration

Configure your editor to:
- Use 4 spaces for indentation
- Trim trailing whitespace
- Add final newline
- Run clang-format on save (optional)

See `.editorconfig` for portable editor settings.
