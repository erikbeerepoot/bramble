# Interval Schedules

## Goal

Support recurring interval watering, e.g. **"run valve N every 2 hours for 15 minutes,
6am–6pm"** — not just the current fixed daily time-of-day windows.

## Current state

A `ScheduleEntry` (`pmu-stm32/Core/Inc/pmu_protocol.h:114`) is a wall-clock calendar
trigger only:

```cpp
uint8_t  hour, minute;   // absolute time of day
uint16_t duration;       // run length, seconds
DayOfWeek daysMask;      // days of week
uint8_t  valveId;
bool     enabled;
```

`minutesUntil()` / `findNextEntry()` match the next `hour:minute`; `isWithinWindow()`
checks a single fixed window. No concept of "every X". Today you'd fake it with 12
separate entries.

## Design

Extend the entry with an optional repeat, keeping legacy entries valid:

- `periodMinutes` (uint16) — interval between firings. **0 = legacy one-shot daily.**
- `windowMinutes` (uint16) — how long the repeating window runs from `hour:minute`.

`hour:minute` becomes the **window start**. Example "every 2h for 15min, 6am–6pm":
`start=06:00, periodMinutes=120, windowMinutes=720, duration=900, period>0`.

Firing logic (when `periodMinutes > 0`): inside `[start, start+windowMinutes]`, fire
when `(minutesSinceStart % periodMinutes) == 0`; run for `duration` seconds. Outside the
window, dormant. `minutesUntil` returns minutes to the next modulo boundary.

Backward compatible: `periodMinutes == 0` keeps exact current behavior.

## Touch points

1. **`ScheduleEntry`** struct + `SCHEDULE_ENTRY_SIZE` 7 → 11 (pmu_protocol.h). Add the
   two uint16 fields.
2. **Schedule logic** — `minutesUntil`, `isWithinWindow`, `overlapsWith` /
   `timeRangesOverlap` (pmu_protocol.cpp): branch on `periodMinutes`, modulo math.
3. **FRAM serialization** — `loadScheduleEntry` / `saveScheduleEntry`
   (persistent_storage.cpp); bump `FORMAT_VERSION` 3 → 4 (layout shift → re-init/wipe,
   which also lets fresh PMUs pick up the new 5-min wake default).
4. **PMU wire parse** — `handleSetSchedule` (pmu_protocol.cpp): payload now
   `index + 11 bytes`; length check `SCHEDULE_ENTRY_SIZE + 1`.
5. **RP2040 side** — `ScheduleEntry` struct + `addScheduleEntry` builder and the parser
   at `src/hal/pmu_protocol.cpp:482` (add the two fields to both ends of the wire).
6. **Hub / API / dashboard** — schedule authoring UI/endpoint to set period + window;
   validation; display "every Nm/Nh" form.

## Risks / notes

- FRAM format bump wipes persisted schedules + wake interval on existing PMUs — they
  re-init to defaults. Acceptable, but coordinate with a re-push of schedules from the hub.
- Overlap detection gets more involved for repeating windows (an interval entry occupies
  many sub-windows). Simplest first cut: treat an interval entry's whole `[start, start+window]`
  as occupied for overlap purposes.
- Keep `duration < periodMinutes*60` validation so a run can't bleed into the next firing.

## Scope estimate

PMU firmware (1–4) is the core, self-contained change. RP2040 (5) is mechanical.
Dashboard/API (6) is the larger surface. Ship PMU + RP2040 first behind period==0
default, then add UI.
