# Bramble

LoRa sensor and actuator network for farm monitoring, built on RP2040.

Hub-and-spoke architecture: a central hub manages the network while nodes collect sensor data (temperature, humidity) and control actuators (irrigation valves). LoRa provides long-range wireless with reliable delivery guarantees.

## Hardware

- Adafruit Feather RP2040 LoRa (SX1276, 915 MHz)
- MT25QL01GBBB 128MB external flash for sensor data storage
- STM32L010 PMU for power management and sleep

## Building

Requires the Pico SDK. CMake is bundled at `~/.pico-sdk/cmake/v3.31.5/bin/cmake`.

```bash
# Build a specific variant
cmake -B build -DHARDWARE_VARIANT=SENSOR
cmake --build build -j8

# Variants: SENSOR, IRRIGATION, CONTROLLER, HUB, GENERIC, ALL
```

Each variant produces a named executable (e.g. `bramble_sensor.uf2`).

```bash
# Run tests
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j8
```

### Flashing

Hold BOOTSEL and connect via USB, then copy the `.uf2` file. Or use OpenOCD with a debug probe:

```bash
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
  -c "adapter speed 5000" \
  -c "program build/bramble_sensor.elf verify reset exit"
```

## Architecture

```
[Sensor Nodes] --LoRa--> [Hub/Pico] --Serial--> [Raspberry Pi] --REST--> [Dashboard]
                               |                       |
                          128MB Flash              DuckDB
                        (local buffer)          (long-term storage)
```

### Firmware (`src/`)

- `modes/` - Application modes per hardware variant (sensor, irrigation, hub, etc.)
- `lora/` - SX1276 driver, message protocol, reliable delivery, routing
- `hal/` - Hardware abstraction (flash, NeoPixel, valves, SPI)
- `storage/` - 128MB circular flash buffer for sensor records
- `config/` - Persistent node/hub configuration

### API (`api/`)

Flask REST API on the Raspberry Pi. Receives sensor data from the hub over serial, stores in DuckDB.

```bash
cd api && uv sync && uv run app.py
```

### Dashboard (`dashboard/`)

React SPA for viewing sensor data, managing nodes, and organizing zones.

```bash
cd dashboard && npm install && npm run dev
```

## Setup

After cloning:

```bash
./setup.sh  # configures git hooks
```

See `CLAUDE.md` for development guidelines and `PLAN.md` for the roadmap.
