# SX1262 Driver Cleanup Plan

## Overview

Clean up `src/lora/sx1262.cpp` and `src/lora/sx1262.h` to remove debugging artifacts, eliminate unnecessary workarounds, and improve code structure.

## Changes

### 1. Replace Raw ISR with GpioInterruptManager

**Problem**: SX1262 uses a raw static ISR (`dio1Isr`) with a global `isr_instance_` pointer, bypassing the `GpioInterruptManager` that SX1276 already uses. The comment says it was to "eliminate std::function overhead" — but SX1276 proves this works fine.

**Changes**:
- Remove `static SX1262* isr_instance_` and `static void dio1Isr()` (lines 9-18, header lines 181-183)
- Remove `isr_call_count_` diagnostic counter (header line 179)
- In `enableInterruptMode()`: register via `GpioInterruptManager::getInstance().registerHandler()` with a lambda, matching the SX1276 pattern
- In `disableInterruptMode()`: use `GpioInterruptManager::getInstance().unregisterHandler()`
- Remove `isr_call_count_` references from log messages in `isTxDone()` and `checkForMissedRxInterrupt()`

### 2. Extract Retry Helper

**Problem**: Two hand-coded retry loops in `begin()` (reset retry at lines 57-75, sync word retry at lines 202-216) with duplicated retry/log/check patterns.

**Changes**:
- Add a private `retryWithBackoff()` template method to SX1262:
  ```cpp
  bool retryWithBackoff(int max_attempts, uint32_t delay_us,
                        const char* description,
                        std::function<bool()> action);
  ```
- Refactor the reset retry loop and sync word retry loop to use it
- Keep it as a private method on SX1262 (not a standalone utility) — KISS

### 3. Reduce Calibration Timeout

**Problem**: `waitBusy(100)` on line 109 uses 100ms timeout. The comment explains TCXO delay (10ms) + calibration (~25ms) = ~35ms. 100ms is overly generous.

**Changes**:
- Reduce from 100ms to 50ms — still 40% margin over the expected ~35ms
- Update the comment to reflect the tighter margin

### 4. Remove Undocumented SPI Corruption Workarounds

**Problem**: `send()` re-applies sync word, PA config, OCP, packet type, modulation params, and DIO1 IRQ mapping before every TX, based on claims of "SPI corruption." These are not documented in the datasheet and were likely added while debugging hardware issues.

**What stays** (documented errata/behavior):
- **TxClamp errata** (section 15.2): Must apply before TX — read-modify-write on 0x08D8
- **RX gain reset**: SetRx resets register 0x08AC to power-saving mode — must re-apply after every `startReceive()`

**What gets removed from `send()`**:
- Sync word re-application (line 371)
- PA config re-application (lines 376-377)
- OCP re-write (line 378)
- Packet type + modulation re-apply (lines 366-368) — these don't change between calls
- DIO1 IRQ mapping re-apply (lines 400-404)

**What gets removed from `startReceive()`**:
- DIO1 IRQ mapping re-apply (lines 594-597)

**What gets removed from `handleInterrupt()`**:
- DIO1 IRQ mapping re-apply after clearing flags (lines 716-719)

### 5. Refactor Large Methods

**5a. Extract `performFullRecovery()`**

The sequence `reset(); begin(); startReceive();` appears 4+ times in `send()` and `isTxDone()`.

- Extract to `void performFullRecovery()`
- Replace all inline occurrences

**5b. Simplify `send()`**

After removing workarounds and extracting recovery, `send()` becomes:
1. Set STDBY_XOSC
2. Clear IRQ flags
3. Apply TxClamp errata fix
4. Set TX power
5. Set packet params with payload length
6. Write payload to buffer
7. Set DIO1 IRQ mapping
8. Start TX
9. Verify chip entered TX mode

**5c. Simplify `isTxDone()`**

The interrupt and polling branches share duplicated chip-mode checking logic.

- Extract `checkTxDoneIrq()` that handles the common IRQ checking + chip mode validation + FS recovery
- Both branches call the shared helper, reducing duplication

### 6. Remove Diagnostic/Debug Artifacts from `enableInterruptMode()`

- Remove verbose status readback and logging after `setDioIrqParams()` (lines 652-662) — this was for debugging
- Keep the DIO1 stale-high check (lines 680-686) — this is operationally useful

## Files Modified

- `src/lora/sx1262.h` — Remove static ISR members, add new private method declarations
- `src/lora/sx1262.cpp` — All the changes above

## Build Verification

After changes, build all variants with `/bramble-build ALL` since this is shared LoRa code.

## Order of Implementation

1. Extract `performFullRecovery()` (safe, mechanical refactor)
2. Remove SPI corruption workarounds (simplification)
3. Extract retry helper + use it (refactor)
4. Replace ISR with GpioInterruptManager (behavior change)
5. Reduce calibration timeout (config change)
6. Simplify `isTxDone()` (refactor)
7. Clean up `enableInterruptMode()` diagnostics
8. Build all variants to verify
