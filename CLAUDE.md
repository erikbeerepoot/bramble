# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Raspberry Pi Pico project using the Pico SDK. This project will use a LORA wireless link and interface with various sensors and actuators used on a flower farm, with the first use cases being a temperature and humidity monitor and an irrigation valve switcher.

## Build System

- **Build tool**: CMake with Ninja generator
- **Target platform**: Raspberry Pi Pico (RP2040 microcontroller) on LoRA feather rp2040 board (for now).
- **Language standards**: C11, C++17 with exceptions and RTTI enabled
- **Output**: UF2 file for flashing to Pico

### CMake Path

CMake is bundled with the Pico SDK:
```bash
CMAKE=~/.pico-sdk/cmake/v3.31.5/bin/cmake
```

### Common Commands

#### Production Build
```bash
# Configure build with hardware variant (run from project root)
~/.pico-sdk/cmake/v3.31.5/bin/cmake -B build -DHARDWARE_VARIANT=SENSOR

# Build the project (parallel)
~/.pico-sdk/cmake/v3.31.5/bin/cmake --build build -j8

# Clean build
rm -rf build && ~/.pico-sdk/cmake/v3.31.5/bin/cmake -B build -DHARDWARE_VARIANT=SENSOR && ~/.pico-sdk/cmake/v3.31.5/bin/cmake --build build -j8
```

#### Test Build
```bash
# Configure test build
~/.pico-sdk/cmake/v3.31.5/bin/cmake -B build -DBUILD_TESTS=ON

# Build test version
~/.pico-sdk/cmake/v3.31.5/bin/cmake --build build -j8

# Clean test build
rm -rf build && ~/.pico-sdk/cmake/v3.31.5/bin/cmake -B build -DBUILD_TESTS=ON && ~/.pico-sdk/cmake/v3.31.5/bin/cmake --build build -j8
```

#### Flash via OpenOCD
```bash
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg \
  -f target/rp2040.cfg \
  -c "adapter speed 5000" \
  -c "program build/bramble_sensor.elf verify reset exit"
```

The build outputs are in the `build/` directory. The main executable will be `bramble.uf2` which can be copied to the Pico when it's in bootloader mode.

## Hardware Configuration

Two board revisions exist. Pin maps are in `src/board/bramble_v3_pins.h` and `src/board/bramble_v4_pins.h`.

### Bramble v3 (Adafruit Feather RP2040 RFM9x)
- **LoRa (SX1276)**: SPI1 — MISO=8, SCK=14, MOSI=15, CS=16, RST=17, DIO0=21
- **External flash**: Shares SPI1 with LoRa — CS=6, RST=7
- **PMU UART**: UART0 — TX=0, RX=1
- **Stdio UART**: UART1 TX-only — GPIO 24 (non-hub nodes); hub uses USB CDC

### Bramble v4 (custom RP2350B)
- **LoRa (SX1262)**: SPI1 (bit-bang, MOSI/SCK swapped) — MISO=12, MOSI=14, SCK=15, CS=10, RST=16, DIO1=19, BUSY=17
- **External flash**: Dedicated SPI0 — MISO=0, CS=1, SCK=2, MOSI=3, RST=5
- **PMU UART**: UART1 — TX=8, RX=9
- **Stdio UART**: UART0 — TX=28, RX=29 (non-hub nodes); hub uses USB CDC

## Code Architecture

The project follows a modular design with HAL abstraction:
- `main.cpp`: Main application entry point with hardware initialization and mode selection
- `src/hal/`: Hardware abstraction layer (NeoPixel, sensors, actuators)
- `src/lora/`: LoRa communication (SX1276 driver, message protocol, reliability)
- `src/storage/`: Persistent storage layer (flash buffer, sensor data records)
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

### Hardware Variants
The build system selects the appropriate mode based on hardware variant:

- **IRRIGATION**: IrrigationMode - valve control, soil moisture sensing
- **SENSOR**: SensorMode - temperature/humidity monitoring
- **CONTROLLER**: ControllerMode (hub) or HubMode (node fallback)
- **HUB**: HubMode - pure hub mode
- **GENERIC**: GenericMode - basic node functionality
- **ALL**: Build all variants in a single configure (targets built in parallel with `-j8`)

Each variant produces a uniquely-named executable (e.g., `bramble_sensor`, `bramble_irrigation`).

## Process

* Use a plans/project.md file to plan out your tasks
* Create subplans when working on features, so we can iterate on the design before implementation.
* Maintain that file as a progress file
* Current development todo list is maintained in PLAN.md under "Development Todo List" section
* Organized as NOW (critical), NEXT (important), LATER (future), and COMPLETED
* Each time we start on a new feature from the NOW list in project.md, create a new feature plan, named "${feature}.md".

## Git Workflow

* **Never commit directly to main/master** - always create a feature branch first
* Branch naming: use descriptive names like `feature/add-logging`, `fix/valve-timing`, `refactor/cleanup-hal`
* After completing work on a branch, create a pull request and merge it (no approval required)
* **Always rebase, never merge** — use `git rebase` to integrate branches, keeping a linear history
* Keep commits atomic and focused on a single logical change
* The `VERSION` file is updated automatically by a commit hook — don't commit it separately

## Design Philosophy Principles
* KISS (Keep It Simple, Stupid)
* Clearly seperate concerns (especially "hardware hiding" -- the HAL should be separate from the domain logic)
* Use full words not abbreviations (e.g address vs addr)
* Ensure you keep README.md updated.
* **Do not use `auto`** — always use explicit types so that the type is clear at the point of declaration (e.g. `uint8_t count = getCount();` not `auto count = getCount();`).

## STM32 PMU Build System

The `pmu-stm32/` directory contains the STM32L010 PMU (Power Management Unit) firmware.

**IMPORTANT**: Do not try to run STM32 build/flash commands directly. Instead, give the user instructions to run them manually.

### STM32 Build Instructions (for user)
```bash
# Build from VS Code: Cmd+Shift+B or use CMake Tools extension
# Or from command line in pmu-stm32 directory:
cube-cmake --build build/Debug
```

### STM32 Flash Instructions (for user)
Flash using STM32CubeIDE, ST-Link, or your preferred programmer.

## Code Change Rules

* **Do not change log levels** (e.g. `logger.info` → `logger.debug`) without confirming with the user first.

## Claude Code Behavior

### RP2040 Build/Flash
Use the `/bramble-build` and `/bramble-flash` skills to build and flash RP2040 firmware:
- `/bramble-build [VARIANT]` - Build firmware (default: SENSOR)
- `/bramble-flash [VARIANT]` - Flash via OpenOCD with CMSIS-DAP debug probe

Example: `/bramble-build SENSOR` then `/bramble-flash SENSOR`

### After Code Changes
After making code changes, build all affected variants using the `/bramble-build` skill:
- Changes to shared code (src/util/, src/lora/, src/hal/) affect ALL variants - use `/bramble-build ALL`
- Changes to mode-specific code (src/modes/sensor_mode.*) only affect that variant - use `/bramble-build SENSOR`
- `HARDWARE_VARIANT=ALL` configures all targets in one CMake run, so `-j8` parallelizes across variants automatically

### STM32 PMU
**Do not attempt to run STM32 build/flash commands directly.** These require specific toolchains. Instead:
1. Make the code changes
2. Provide the user with instructions to build and flash
3. Wait for user to report results
