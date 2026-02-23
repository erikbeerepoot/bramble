# PMU Wake Notification Bug Investigation

## Problem Statement
The RP2040 receives the CTS (ClearToSend) ACK from the PMU, but the subsequent
WakeNotification never arrives. The RP2040 times out after 1000ms and proceeds
without PMU state restoration.

## Expected Flow
```
RP2040                          STM32 (PMU)
  |                                |
  |-- ClearToSend (0x19) -------->|
  |                                | sets clearToSendReceived_ = true
  |<-------- ACK (0x80) ----------|
  |                                |
  |  (RP2040 starts 1000ms timer) | (main loop dispatches CTS_RECEIVED)
  |                                | → state machine → WAKE_ACTIVE
  |                                | → determineWakeType()
  |                                | → sendWakeNotification()
  |                                |
  |<--- WakeReason (0x84) --------|  <-- THIS NEVER ARRIVES
  |                                |
```

## Root Causes Identified

### Cause 1 (Primary): CTS_TIMEOUT sends WakeNotification before RP2040 is ready

**Location**: `pmu-stm32/Core/Src/pmu_state_machine.cpp:55-59` and `pmu-stm32/Core/Src/main.cpp:793-800`

The CTS_TIMEOUT (2 seconds) in AWAITING_CTS is shorter than the RP2040 boot time.
When the timeout fires, the state machine transitions to WAKE_ACTIVE and sends the
WakeNotification to a RP2040 that hasn't initialized its UART yet. The bytes are
lost on the wire.

**Sequence:**
1. RTC wakes STM32 → transitions to AWAITING_CTS → enables DC/DC
2. RP2040 starts booting (takes >2 seconds including UART init + preamble + CTS)
3. **CTS_TIMEOUT fires after 2s** → AWAITING_CTS → WAKE_ACTIVE
4. STM32 sends WakeNotification (RP2040 UART not initialized yet, bytes lost)
5. RP2040 finishes booting, sends CTS with 300ms preamble
6. STM32 ACKs the CTS (**CTS ack received**)
7. RP2040 starts 1000ms timer waiting for WakeNotification
8. STM32 is already in WAKE_ACTIVE, CTS_RECEIVED is a no-op (reducer returns same state)
9. **No WakeNotification is sent** → RP2040 times out

**Why CTS_RECEIVED is a no-op in WAKE_ACTIVE** (`pmu_state_machine.cpp:68-77`):
```cpp
case PmuState::WAKE_ACTIVE:
    switch (event) {
        case PmuEvent::READY_FOR_SLEEP:
        case PmuEvent::WAKE_TIMEOUT:
            return PmuState::SLEEPING;
        // ...
        default:
            return state;  // CTS_RECEIVED falls here - no transition!
    }
```

Since there's no state transition, `onStateChange` is never called, and
`sendWakeNotification()` is never invoked for the late-arriving CTS.

**Timing analysis:**
- DC/DC enable → RP2040 power-on: ~few ms
- RP2040 boot (Pico SDK init, flash, etc.): ~500-1500ms
- PMU client UART init + 150ms stabilization delay: ~200ms
- Wake preamble (3 x 50ms + 150ms): 300ms
- CTS command TX at 9600 baud: ~6ms
- **Total: ~1200-2200ms** (overlaps or exceeds the 2000ms CTS_TIMEOUT)

### Cause 2 (Secondary): SLEEPING handler race condition with STOP mode re-entry

**Location**: `pmu-stm32/Core/Src/main.cpp:220-226`

When the RP2040 is USB-powered (stays alive after DC/DC disable), it can send CTS
while the STM32 is in SLEEPING state. The CTS is received and ACKed during the
`HAL_Delay(100)` in the SLEEPING handler, but the main loop goes back to STOP mode
without checking the CTS flag.

```cpp
case PmuState::SLEEPING:
    dcdc.disable();
    led.setColor(LED::GREEN);
    HAL_Delay(100);         // <-- CTS can arrive here (UART interrupt)
    led.off();              //     ACK is sent from interrupt context
    enterStopMode();        // <-- Goes to STOP with CTS flag unchecked!
    wakeupFromStopMode();
    break;
```

**Sequence:**
1. STM32 is in SLEEPING state, enters STOP mode
2. RP2040 (USB-powered) sends wake preamble → wakes STM32
3. `wakeupFromStopMode()` runs, main loop restarts
4. State is still SLEEPING, `protocol.isCtsReceived()` → false (CTS not complete yet)
5. SLEEPING handler: `HAL_Delay(100)`
6. **During the delay**: remaining CTS bytes arrive, parsed by UART interrupt,
   `clearToSendReceived_ = true`, ACK sent from interrupt context
7. After delay: `enterStopMode()` → **goes back to STOP without processing CTS flag**
8. RP2040 received ACK, waits for WakeNotification that never comes

The CTS flag is only checked at the **top** of the main loop (`main.cpp:196-199`),
not before `enterStopMode()`.

### Cause 3 (Minor): Shared MessageBuilder between interrupt and main loop contexts

**Location**: `pmu-stm32/Core/Src/pmu_protocol.cpp` - single `builder_` instance

The STM32's `Protocol` class has a single `MessageBuilder builder_` used for both:
- `sendAck()` called from UART RX interrupt context (inside `processReceivedByte`)
- `sendWakeNotification()` called from main loop context (via `onStateChange`)

If a CTS retry arrives while the main loop is building a WakeNotification message,
the `builder_` buffer would be corrupted mid-transmission since `HAL_UART_Transmit`
reads from the same buffer progressively.

This is unlikely in normal operation (RP2040 won't retry after receiving ACK) but
could occur if the ACK was corrupted on the wire, causing the RP2040 to retry
CTS while the STM32 is building/sending the WakeNotification.

## Resolution

After analysis, the cleanest fix addresses both root causes by removing the
`CTS_TIMEOUT` entirely and eliminating the SLEEPING race window.

### Change 1: Remove `CTS_TIMEOUT` — rely on `WAKE_TIMEOUT` as safety net

**Files**: `pmu_state_machine.h`, `pmu_state_machine.cpp`

Removed `CTS_TIMEOUT_MS`, `PmuEvent::CTS_TIMEOUT`, and the CTS timeout check in
`tick()`. The 2-minute `PERIODIC_WAKE_TIMEOUT_MS` is the safety net for the
pathological case where the RP2040 crashes and never sends CTS.

Without `CTS_TIMEOUT`, the PMU stays in `AWAITING_CTS` until the RP2040 finishes
booting (1.2–2.2s) and sends CTS. The state machine then transitions correctly
to `WAKE_ACTIVE` and sends the `WakeNotification`.

### Change 2: Remove `HAL_Delay(100)` from `SLEEPING` handler

**File**: `main.cpp`

The 100ms delay was the entire race window where CTS could arrive during
`SLEEPING` and be ACKed, but the STM32 would still re-enter STOP mode on the
next iteration without processing the CTS flag.

Removing the delay eliminates this window entirely. If CTS arrives after the
STM32 has entered STOP mode, the LPUART start-bit detection wakes the MCU
immediately, and the main loop processes the CTS flag on the next iteration.
