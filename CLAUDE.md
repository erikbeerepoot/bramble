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

The build outputs are in the `build/` directory. The main executable will be `bramble.uf2` which can be copied to the Pico when it's in bootloader mode.

## Hardware Configuration

- **SPI Configuration**: 
  - Port: SPI0 at 1MHz
  - MISO: GPIO 16, CS: GPIO 17, SCK: GPIO 18, MOSI: GPIO 19
- **Communication**: USB stdio enabled, UART stdio disabled
- **Libraries used**: hardware_spi, hardware_timer, hardware_clocks

## Code Architecture

The project structure is minimal with a single source file:
- `bramble.cpp`: Main application demonstrating SPI setup, timer callbacks, and system clock reporting
- `CMakeLists.txt`: Build configuration with Pico SDK integration
- `pico_sdk_import.cmake`: SDK import script
- Code should be modular, with a HAL and the application code built on top of it.

## Process

* Use a PLAN.md file to plan out your tasks
* Maintain that file as a progress file.

## Design Philosophy Principles
* KISS (Keep It Simple, Stupid)
* SOLID Principles

