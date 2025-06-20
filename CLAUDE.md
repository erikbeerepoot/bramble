# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Raspberry Pi Pico project using the Pico SDK. This project will use a LORA wireless link and interface with various sensors and actuators used on a flower farm, with the first use cases being a temperature and humidity monitor and an irrigation valve switcher.

## Build System

- **Build tool**: CMake with Ninja generator
- **Target platform**: Raspberry Pi Pico (RP2040 microcontroller) on LoRA feather rp2040 board (for now).
- **Language standards**: C11, C++17 with exceptions and RTTI enabled
- **Output**: UF2 file for flashing to Pico

### Common Commands

#### Production Build
```bash
# Configure build (run from project root)
cmake -B build

# Build the project
cmake --build build

# Build with verbose output
cmake --build build --verbose

# Clean build
rm -rf build && cmake -B build && cmake --build build
```

#### Test Build
```bash
# Configure test build
cmake -B build -DBUILD_TESTS=ON

# Build test version
cmake --build build

# Clean test build
rm -rf build && cmake -B build -DBUILD_TESTS=ON && cmake --build build
```

The build outputs are in the `build/` directory. The main executable will be `bramble.uf2` which can be copied to the Pico when it's in bootloader mode.

## Hardware Configuration

- **SPI Configuration**: 
  - Port: SPI0 at 1MHz
  - MISO: GPIO 16, CS: GPIO 17, SCK: GPIO 18, MOSI: GPIO 19
- **Communication**: USB stdio enabled, UART stdio disabled
- **Libraries used**: hardware_spi, hardware_timer, hardware_clocks

## Code Architecture

The project follows a modular design with HAL abstraction:
- `bramble.cpp`: Main application with LoRa communication and reliability testing
- `src/hal/`: Hardware abstraction layer (NeoPixel, sensors, actuators)
- `src/lora/`: LoRa communication (SX1276 driver, message protocol, reliability)
- `src/tests/`: Integration test framework with mock hardware
- `CMakeLists.txt`: Build configuration supporting both production and test builds
- `pico_sdk_import.cmake`: SDK import script

### Current Features
- **NeoPixel LED Control**: WS2812 support via PIO
- **LoRa Communication**: SX1276 driver with proper pin configuration
- **Message Protocol**: Structured messages with ACK/retry mechanism  
- **Reliability System**: Criticality-based delivery (BEST_EFFORT, RELIABLE, CRITICAL)
- **Integration Tests**: Mock-based testing framework for controlled reliability testing

### Testing
- Production build: Standard firmware for hardware deployment
- Test build: Includes integration test suite with MockSX1276 for reliability validation
- Build with `-DBUILD_TESTS=ON` to compile test version

### Production Configuration
The main application has completely separate demo and production modes for clarity:

**Demo Mode** (`DEMO_MODE = true` in bramble.cpp):
- Runs `runDemoMode()` function with its own main loop
- Colorful LED cycling for visual feedback
- Sends test messages every 15 seconds
- Verbose debug output to console
- Ideal for development and system verification

**Production Mode** (`DEMO_MODE = false`):
- Runs `runProductionMode()` function with its own main loop
- Green LED heartbeat indication only
- Minimal console output (reduces power consumption)
- Sensor readings every 30 seconds, heartbeat every 60 seconds
- Clear TODO markers for actual sensor/actuator integration
- Ready for deployment with real hardware

**Benefits of Separation**:
- Easy to understand each mode independently
- No conditional logic scattered throughout code
- Simple to maintain and debug each mode
- Clear separation of concerns

## Process

* Use a plans/project.md file to plan out your tasks
* Create subplans when working on features, so we can iterate on the design before implementation.
* Maintain that file as a progress file
* Current development todo list is maintained in PLAN.md under "Development Todo List" section
* Organized as NOW (critical), NEXT (important), LATER (future), and COMPLETED
* Each time we start on a new feature from the NOW list in project.md, create a new feature plan, named "${feature}.md".

## Design Philosophy Principles
* KISS (Keep It Simple, Stupid)
* Clearly seperate concerns (especially "hardware hiding" -- the HAL should be separate from the domain logic)
* Use full words not abbreviations (e.g address vs addr)
* Ensure you keep README.md updated.
