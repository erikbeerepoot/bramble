# Sensor State Machine Deadlock Analysis

## Summary

Analysis of every state in `SensorStateMachine` to verify that all states have
valid exit transitions for all permutations of input conditions. **Seven issues
found**, two of which are confirmed deadlocks that will halt the device.

---

## State-by-State Analysis

### 1. INITIALIZING

**Entry:** Default state on boot.

**Exit paths:**
- `markInitialized()` → REGISTERING (via PMU wake callback or `!pmu_ok` fallback)
- `markError()` → ERROR

**Verdict: OK** — PMU has a 1000ms wake notification timeout that guarantees the
callback fires. The `!pmu_ok` fallback calls `markInitialized()` directly.

---

### 2. REGISTERING

**Entry:** From INITIALIZING via `markInitialized()`.

**Exit paths:**
- Already registered → `reportRegistrationComplete()` → AWAITING_TIME (immediate)
- Registration sent, response received → callback → `reportRegistrationComplete()` → AWAITING_TIME
- Registration sent, timeout (5000ms) → `reportRegistrationTimeout()` → READY_FOR_SLEEP
- Registration send failed (seq == 0) → `reportRegistrationTimeout()` → READY_FOR_SLEEP

**Verdict: OK** — All paths have exit transitions. Timeout guarantees escape.

---

### 3. AWAITING_TIME

**Entry:** From REGISTERING via `reportRegistrationComplete()`, or from
READY_FOR_SLEEP via `reportWakeFromSleep()`.

**Exit paths (via `requestTimeSync()` posted in onStateChange):**
- PMU available, valid time, time to transmit → `reportHeartbeatSent()` → SYNCING_TIME
- PMU available, valid time, NOT time to transmit → `reportTimeSyncComplete()` → TIME_SYNCED
- PMU available, time NOT valid → `reportHeartbeatSent()` → SYNCING_TIME
- PMU NOT available → `reportHeartbeatSent()` → SYNCING_TIME

**BUG — `rtc_set_datetime()` failure:** If PMU has valid time but
`rtc_set_datetime()` fails (`sensor_mode.cpp:838`), the code logs an error and
falls through the callback with **no state transition**. No timeout is set for
AWAITING_TIME. The state machine halts here permanently.

```cpp
// sensor_mode.cpp:838-840
} else {
    logger.error("Failed to set RTC from PMU time");
    // *** NO STATE TRANSITION — STUCK IN AWAITING_TIME ***
}
```

**Verdict: DEADLOCK** — Missing transition on `rtc_set_datetime()` failure.

**Fix:** Fall back to hub sync (send heartbeat) when `rtc_set_datetime()` fails:
```cpp
} else {
    logger.error("Failed to set RTC from PMU time - falling back to hub sync");
    heartbeat_client_->send();
    sensor_state_.expectResponse();
    sensor_state_.reportHeartbeatSent();
}
```

---

### 4. SYNCING_TIME

**Entry:** From AWAITING_TIME via `reportHeartbeatSent()`.

**Exit paths:**
- Hub response received → heartbeat response callback → `reportTimeSyncComplete()` → TIME_SYNCED
- Heartbeat delivery failed + RTC running → `reportTimeSyncComplete()` → TIME_SYNCED
- Heartbeat delivery failed + no RTC → `reportSyncTimeout()` → READY_FOR_SLEEP
- Timeout (5000ms) + RTC running → `reportTimeSyncComplete()` → TIME_SYNCED
- Timeout (5000ms) + no RTC → `reportSyncTimeout()` → READY_FOR_SLEEP

**Verdict: OK** — The 5000ms timeout guarantees an exit in all conditions.

---

### 5. TIME_SYNCED

**Entry:** From SYNCING_TIME or AWAITING_TIME via `reportTimeSyncComplete()`.

**Exit paths (via `tryInitSensor()` posted in onStateChange):**
- Sensor already initialized → `reportSensorInitSuccess()` → READING_SENSOR
- Sensor init succeeds → `reportSensorInitSuccess()` → READING_SENSOR
- Sensor init fails → `reportSensorInitFailure()` → DEGRADED_NO_SENSOR
- `sensor_` is null → `tryInitSensor()` returns false, **no state transition**

**Edge case:** If `sensor_` is null, `tryInitSensor()` at line 765 returns
`false` without calling any state machine event. The state machine stays in
TIME_SYNCED. `sensor_` is created in `onStart()` so this is very unlikely, but
the code path is not robust.

```cpp
// sensor_mode.cpp:765-767
if (!sensor_) {
    return false;  // *** NO STATE TRANSITION ***
}
```

**Verdict: MINOR RISK** — Unlikely (sensor_ always created in onStart), but
undefended. Should call `reportSensorInitFailure()` when sensor_ is null.

**Fix:**
```cpp
if (!sensor_) {
    sensor_state_.reportSensorInitFailure();
    return false;
}
```

**Design note — lazy init retry for transient failures:** Sensor initialization
failures are not always permanent. I²C bus glitches, power-on settling delays,
and similar transient conditions can cause a single init attempt to fail even
when the hardware is functioning. Rather than sending the state machine
permanently to `DEGRADED_NO_SENSOR` on first failure, consider a lazy retry
approach: on init failure, transition to `READY_FOR_SLEEP` instead. On the next
Periodic wake the full cycle restarts (AWAITING_TIME → TIME_SYNCED → sensor
init), giving the sensor a fresh opportunity to initialize. A retry counter can
cap the number of attempts before escalating to DEGRADED_NO_SENSOR, so a truly
absent sensor is still reported correctly after N failed cycles.

---

### 6. READING_SENSOR

**Entry:** From TIME_SYNCED via `reportSensorInitSuccess()` → `updateState()`.

**Exit paths (from onStateChange task):**
```cpp
self->readAndStoreSensorData(time);
self->sensor_state_.reportReadComplete();  // Always called
```

`reportReadComplete()` → CHECKING_BACKLOG. The call to `reportReadComplete()` is
unconditional — it always runs regardless of whether `readAndStoreSensorData()`
succeeds or fails internally.

**Verdict: OK** — `reportReadComplete()` is always called.

---

### 7. CHECKING_BACKLOG

**Entry:** From READING_SENSOR via `reportReadComplete()`.

**Exit paths (from onStateChange task):**
```cpp
bool needsTx = self->checkNeedsTransmission();
self->sensor_state_.reportCheckComplete(needsTx);
```

- `reportCheckComplete(true)` → TRANSMITTING
- `reportCheckComplete(false)` + `hasExpectedResponses()` → LISTENING
- `reportCheckComplete(false)` + no responses → READY_FOR_SLEEP

**Verdict: OK** — `reportCheckComplete()` is always called; all branches have
transitions.

---

### 8. TRANSMITTING

**Entry:** From CHECKING_BACKLOG via `reportCheckComplete(true)`.

**Exit paths:**
- `transmitBacklog()` callback → `reportTransmitComplete()` → LISTENING or READY_FOR_SLEEP
- `transmitCurrentReading()` callback → `reportTransmitComplete()` → LISTENING or READY_FOR_SLEEP
- Various error paths (null flash, read failure, null reading) → `reportTransmitComplete()`

**Risk:** All exit paths depend on the `BatchTransmitter` callback eventually
firing. The reliable messenger has retry limits and timeouts, so the callback
_should_ always fire. However, there is **no safety timeout** on the
TRANSMITTING state itself. If the messenger gets into an unexpected state (e.g.,
task queue starvation, SPI bus lockup), the callback may never fire and the state
machine halts.

**Verdict: MEDIUM RISK** — No safety timeout. Add a transmit watchdog timeout
(e.g., 30s) that calls `reportTransmitComplete()` as a failsafe.

---

### 9. LISTENING

**Entry:** From TRANSMITTING or CHECKING_BACKLOG.

**Exit paths:**
- 500ms timeout fires → `reportListenComplete()` → READY_FOR_SLEEP

**Verdict: OK** — Timeout guarantees an exit.

---

### 10. READY_FOR_SLEEP

**Entry:** From REGISTERING, SYNCING_TIME, CHECKING_BACKLOG, TRANSMITTING, or
LISTENING.

**Exit paths:**
- PMU periodic wake → `reportWakeFromSleep()` → AWAITING_TIME
- PMU scheduled wake → **logs warning only, no transition**
- PMU external wake → **logs info only, no transition**
- PMU unavailable → `signalReadyForSleep()` returns silently, **no wake will come**

```cpp
// sensor_mode.cpp:137-150 (PMU wake callback)
switch (reason) {
    case PMU::WakeReason::Periodic:
        sensor_state_.reportWakeFromSleep();  // ✓ transitions out
        break;
    case PMU::WakeReason::Scheduled:
        pmu_logger.warn("Unexpected scheduled wake in sensor mode");
        break;  // *** NO TRANSITION ***
    case PMU::WakeReason::External:
        pmu_logger.info("External wake trigger");
        break;  // *** NO TRANSITION ***
}
```

**Verdict: MEDIUM RISK** — Scheduled/External wakes are dead ends. And if PMU is
unavailable, the device is stuck in READY_FOR_SLEEP forever with no recovery.

**Fix:** Treat all wake reasons as valid:
```cpp
switch (reason) {
    case PMU::WakeReason::Periodic:
    case PMU::WakeReason::External:
        sensor_state_.reportWakeFromSleep();
        break;
    case PMU::WakeReason::Scheduled:
        pmu_logger.warn("Unexpected scheduled wake in sensor mode");
        sensor_state_.reportWakeFromSleep();
        break;
}
```

For the no-PMU case, consider a software watchdog fallback in onLoop that
detects prolonged READY_FOR_SLEEP without a wake and triggers recovery.

---

### 11. DEGRADED_NO_SENSOR

**Entry:** From TIME_SYNCED via `reportSensorInitFailure()` → `updateState()`.

**Exit paths (from onStateChange):**
```cpp
case SensorState::DEGRADED_NO_SENSOR:
    // ...
    bool needsTx = self->checkNeedsTransmission();
    self->sensor_state_.reportCheckComplete(needsTx);  // *** REJECTED ***
    break;
```

The state machine's `reportCheckComplete()` has a guard:
```cpp
void SensorStateMachine::reportCheckComplete(bool needsTransmit)
{
    if (state_ != SensorState::CHECKING_BACKLOG) {  // state is DEGRADED_NO_SENSOR!
        logger.warn("reportCheckComplete() called in unexpected state: %s", ...);
        return;  // *** REJECTED — NO TRANSITION ***
    }
```

The call is rejected because the current state is DEGRADED_NO_SENSOR, not
CHECKING_BACKLOG. **The state machine has no exit from DEGRADED_NO_SENSOR.** The
device halts permanently in this state.

**Verdict: CONFIRMED DEADLOCK** — This is the most critical bug. Any sensor
initialization failure permanently halts the wake cycle.

**Fix — transition to READY_FOR_SLEEP and retry on next wake cycle:**

The correct exit from DEGRADED_NO_SENSOR is `READY_FOR_SLEEP`, not
`CHECKING_BACKLOG`. This allows the device to sleep and then repeat the full
wake cycle on the next Periodic PMU event, giving the sensor another
initialization attempt. This implements the lazy-retry strategy described in the
TIME_SYNCED section above — sensor failures are often transient, and sleeping
then retrying is safer than either halting or routing through unrelated backlog
logic.

Add a dedicated `reportSensorRetry()` transition (or reuse an existing one) that
exits DEGRADED_NO_SENSOR directly to READY_FOR_SLEEP:

```cpp
// In SensorStateMachine, add a new event:
void SensorStateMachine::reportSensorRetry()
{
    if (state_ != SensorState::DEGRADED_NO_SENSOR) {
        logger.warn("reportSensorRetry() called in unexpected state: %s", ...);
        return;
    }
    transitionTo(SensorState::READY_FOR_SLEEP);
}
```

Then call it from `onStateChange` for DEGRADED_NO_SENSOR:
```cpp
case SensorState::DEGRADED_NO_SENSOR:
    led_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 255, 165, 0);
    // Sleep and retry init on next wake
    sensor_state_.reportSensorRetry();
    break;
```

A retry counter should guard against infinite loops on permanently absent
hardware — after N failed wake cycles the machine may escalate to ERROR or stop
retrying.

---

### 12. ERROR

**Entry:** From any state via `markError()`.

**Exit paths:** None by design — only recoverable via hardware reset/watchdog.

**Verdict: OK** — Terminal state, intentional.

---

## Issue Summary

| # | Severity | State | Issue | Impact |
|---|----------|-------|-------|--------|
| 1 | **CRITICAL** | DEGRADED_NO_SENSOR | No exit transition; `reportCheckComplete()` rejected | Device halts permanently on sensor init failure; fix: transition to READY_FOR_SLEEP and retry on next wake |
| 2 | **CRITICAL** | AWAITING_TIME | `rtc_set_datetime()` failure — no transition, no timeout | Device halts permanently if RTC set fails |
| 3 | **MEDIUM** | TRANSMITTING | No safety timeout | Device halts if transmit callback never fires |
| 4 | **MEDIUM** | READY_FOR_SLEEP | Scheduled/External wakes don't transition; no PMU = stuck | Device may halt permanently |
| 5 | **LOW** | TIME_SYNCED | Null `sensor_` → no transition | Unlikely but undefended |
| 6 | **LOW** | INITIALIZING | No-PMU path: wasted heartbeat + off-by-one `expected_responses_` | Unnecessary LISTENING state entry |
| 7 | **LOW** | READY_FOR_SLEEP | No software fallback when PMU unavailable | Relies entirely on PMU for wake |

## Recommended Fix Priority

### Immediate (blocks normal operation)

1. **Fix DEGRADED_NO_SENSOR deadlock** — Add a `reportSensorRetry()` transition
   that exits DEGRADED_NO_SENSOR directly to READY_FOR_SLEEP. On the next
   Periodic wake the full cycle restarts and sensor init is retried. Guard with
   a retry counter to escalate to ERROR after N consecutive failures.

2. **Fix AWAITING_TIME deadlock** — Add hub-sync fallback when
   `rtc_set_datetime()` fails in `sensor_mode.cpp:requestTimeSync()`.

### Soon (robustness)

3. **Add TRANSMITTING safety timeout** — Post a 30s watchdog in
   `onStateChange(TRANSMITTING)` that calls `reportTransmitComplete()`.

4. **Handle all PMU wake reasons** — Transition on Scheduled/External wakes in
   the PMU callback.

5. **Defend TIME_SYNCED null sensor path** — Call `reportSensorInitFailure()`
   when `sensor_` is null in `tryInitSensor()`.

### Later (cleanup)

6. **Fix no-PMU bootstrap sequence** — Move the heartbeat send to after
   AWAITING_TIME is reached, or gate `expectResponse()` on successful
   `reportHeartbeatSent()`.

7. **Add READY_FOR_SLEEP software watchdog** — Detect prolonged sleep state
   without PMU and trigger recovery (e.g., reboot via watchdog).
