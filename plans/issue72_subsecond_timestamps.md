# Issue #72: Add Subsecond Precision to RTC-Synced Log Timestamps

## Problem Summary

Currently, log timestamps lose millisecond precision after RTC sync:
- **Before RTC sync**: `[+Xms]` - relative time with millisecond precision
- **After RTC sync**: `[2026-01-31 02:53:13]` - absolute time with only second precision

This makes it difficult to profile wake cycles (~400ms) and debug timing-sensitive issues.

## Proposed Solution

Change the absolute timestamp format from:
```
[2026-01-31 02:53:13]
```
to:
```
[2026-01-31 02:53:13.045]
```

## Technical Approach

### Synchronized Millisecond Tracking

The RTC and system timer are independent clocks. To get accurate milliseconds within each RTC second, we track when the RTC was synced:

1. When `rtc_set_datetime()` is called, record the system timer value
2. On each log, calculate elapsed time since sync
3. The ms within current second = `(elapsed_ms) % 1000`

This works because both the RTC and elapsed calculation advance at 1000ms per second.

### Implementation

**1. Add to `Logger` class (`src/hal/logger.h`):**

```cpp
private:
    static uint64_t rtc_sync_us_;  // System time (us) when RTC was last synced

public:
    /**
     * @brief Call immediately after rtc_set_datetime() to sync subsecond precision
     */
    static void onRtcSynced() {
        rtc_sync_us_ = to_us_since_boot(get_absolute_time());
    }
```

**2. Update timestamp formatting:**

```cpp
if (rtc_running() && rtc_get_datetime(&dt)) {
    uint32_t ms_in_second = 0;
    if (rtc_sync_us_ > 0) {
        uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - rtc_sync_us_;
        ms_in_second = (elapsed_us / 1000) % 1000;
    }
    printf("[%04d-%02d-%02d %02d:%02d:%02d.%03lu] ",
           dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec, ms_in_second);
}
```

**3. Initialize static member (`src/hal/logger.cpp`):**

```cpp
uint64_t Logger::rtc_sync_us_ = 0;
```

**4. Update RTC set call sites to call `Logger::onRtcSynced()`:**

| File | Line | Context |
|------|------|---------|
| `src/modes/application_mode.cpp` | ~128 | After heartbeat response sets RTC |
| `src/modes/hub_mode.cpp` | ~512 | After receiving datetime from Raspberry Pi |
| `src/modes/sensor_mode.cpp` | ~1061 | After receiving datetime from PMU |

## Tasks

- [ ] Add `rtc_sync_us_` static member and `onRtcSynced()` method to Logger
- [ ] Update timestamp formatting to use synchronized milliseconds
- [ ] Initialize static member in logger.cpp
- [ ] Add `Logger::onRtcSynced()` call after `rtc_set_datetime()` in application_mode.cpp
- [ ] Add `Logger::onRtcSynced()` call after `rtc_set_datetime()` in hub_mode.cpp
- [ ] Add `Logger::onRtcSynced()` call after `rtc_set_datetime()` in sensor_mode.cpp
- [ ] Build all variants to verify compilation
- [ ] Test on hardware

## Files Changed

| File | Change |
|------|--------|
| `src/hal/logger.h` | Add `rtc_sync_us_`, `onRtcSynced()`, update timestamp format |
| `src/hal/logger.cpp` | Initialize `rtc_sync_us_` static member |
| `src/modes/application_mode.cpp` | Call `Logger::onRtcSynced()` after RTC set |
| `src/modes/hub_mode.cpp` | Call `Logger::onRtcSynced()` after RTC set |
| `src/modes/sensor_mode.cpp` | Call `Logger::onRtcSynced()` after RTC set |

## Example Output

Before:
```
[+1234ms] INFO [SensorMode]: Starting measurement
[+1456ms] INFO [SensorMode]: Temperature: 22.5C
[2026-01-31 02:53:13] INFO [SensorMode]: RTC synced, sending data
[2026-01-31 02:53:13] INFO [SensorMode]: Transmission complete
```

After:
```
[+1234ms] INFO [SensorMode]: Starting measurement
[+1456ms] INFO [SensorMode]: Temperature: 22.5C
[2026-01-31 02:53:13.045] INFO [SensorMode]: RTC synced, sending data
[2026-01-31 02:53:13.312] INFO [SensorMode]: Transmission complete
```
