# Bramble PMU (STM32)

Power Management Unit for the Bramble flower farm monitoring system.

## Hardware
- **MCU**: STM32L010F4 (20-pin TSSOP, ultra-low-power)
- **Features**:
  - Bicolour LED status indication (common anode)
  - DC/DC converter control via MOSFET
  - UART communication
  - Power management and battery monitoring

## Build System

### Using STM32CubeIDE
1. Import this directory as an existing project
2. Build and flash using IDE

### Using Make (if configured)
```bash
make
make flash
```

## Getting Started

Current status: Basic LED blink demo

## Pin Configuration

| Function       | Pin | GPIO   | Notes                    |
|----------------|-----|--------|--------------------------|
| LED Red        | 10  | PA4    | Common anode bicolour LED|
| LED Green      | 11  | PA5    | Common anode bicolour LED|
| UART TX        | 8   | PA2    | USART2_TX                |
| UART RX        | 9   | PA3    | USART2_RX                |
| DC/DC Enable   | 7   | PA1    | MOSFET gate control      |

