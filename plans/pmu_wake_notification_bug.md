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

## Recommended Fixes

### Fix 1: Resend WakeNotification when CTS arrives in WAKE_ACTIVE

In `onStateChange` or directly in the main loop, detect CTS_RECEIVED while already
in WAKE_ACTIVE and resend the WakeNotification:

```cpp
// main.cpp main loop
if (protocol.isCtsReceived()) {
    bool wasAlreadyActive = (pmuState.state() == PmuState::WAKE_ACTIVE);
    pmuState.dispatch(PmuEvent::CTS_RECEIVED);
    protocol.clearCtsReceived();

    // If we were already in WAKE_ACTIVE, the state machine won't transition,
    // but we still need to send the wake notification for the late CTS
    if (wasAlreadyActive) {
        sendWakeNotification();
    }
}
```

### Fix 2: Check CTS flag before entering STOP mode

Add a guard in the SLEEPING handler to abort STOP entry if CTS was received:

```cpp
case PmuState::SLEEPING:
    dcdc.disable();
    led.setColor(LED::GREEN);
    HAL_Delay(100);
    led.off();

    // Check if CTS arrived during the delay - don't sleep if so
    if (!protocol.isCtsReceived()) {
        enterStopMode();
        wakeupFromStopMode();
    }
    break;
```

### Fix 3: Increase CTS_TIMEOUT or remove it

The 2-second CTS_TIMEOUT is too aggressive for the RP2040 boot time. Options:
- Increase to 5+ seconds to accommodate worst-case boot time
- Or remove CTS_TIMEOUT entirely and only rely on the overall WAKE_TIMEOUT
  (currently 120s for periodic wakes)

The CTS_TIMEOUT was designed as a "fallback for old firmware that doesn't send CTS"
(per the comment in `pmu_state_machine.h:123`). If all firmware now sends CTS,
increasing this timeout is safe.

### Fix Priority
- **Fix 1 is the most important** - it handles the common case where CTS arrives
  after the timeout has already transitioned to WAKE_ACTIVE
- **Fix 2 prevents the SLEEPING race** - important for USB-powered development
- **Fix 3 prevents the root timing issue** - but Fix 1 is needed regardless as
  a safety net
