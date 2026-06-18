# AC Valve Controller (mains, always-awake)

Status: implemented on branch `feature/ac-valve-controller` (builds clean: IRRIGATION_AC v5/v4 +
IRRIGATION v5 regression). Full design: `~/.claude/plans/virtual-sniffing-hanrahan.md`.

## What it does
Supports **AC valves** (24 V AC) switched by **SSRs** — one GPIO per valve, high = open, held while
open (continuous power). Hardware is **mains-powered, pure-AC**, two valves on **GPIO36/38**. Because
an open SSR needs the GPIO driven, the node **never deep-sleeps** (deep sleep drops the rail → valve
closes). It stays awake and reacts to events live — which also makes it the ideal **master valve**
host: a one-off "run 5 min" mirrored to the master is picked up within a poll interval, not after a
30-min sleep TTL.

Firmware-only: the node registers as a normal IRRIGATION node (`valve_count=2`). Schedules still live
in PMU FRAM via `SET_SCHEDULE`, so the API, dashboard, and valve-group mirroring are unchanged — the
node just *reads and fires* schedules itself.

## Implemented
- **`ACValveDriver`** (`src/hal/valve_controller.h`): SSR GPIO on/off, `requiresContinuousPower()=true`.
- **`ValveController`** (`.cpp`): `#ifdef HARDWARE_IRRIGATION_AC` init builds AC drivers + GPIO out,
  skips H-bridge/indexer; `operateValve` skips indexer select/pulse for continuous-power drivers;
  `usesContinuousPower()` accessor.
- **Board pins** (`bramble_v5_pins.h`, `bramble_v4_pins.h`): under `HARDWARE_IRRIGATION_AC`,
  `NUM_VALVES=2`, `VALVE_PINS={36,38}`; DC defs kept in `#else`.
- **Build** (`CMakeLists.txt`): `IRRIGATION_AC` variant → `bramble_irrigation_ac*`, defines
  `HARDWARE_IRRIGATION=1` + `HARDWARE_IRRIGATION_AC=1`; in `ALL`. `main.cpp`: AC info log.
- **`IrrigationMode`** (all under `#ifdef HARDWARE_IRRIGATION_AC`, DC build byte-identical):
  - Never sleeps: `signalReadyForSleep()` keeps PMU awake and returns; `VALVE_ACTIVE` keeps awake and
    advances the SM instead of arming the PMU close-alarm.
  - `startAlwaysAwakeTasks()` registers periodic `task_manager_` tasks: PMU keepalive (5s),
    hub update poll (15s, gated on registered+time-synced), schedule eval (20s), auto-close (2s).
  - `evaluateLocalSchedules()` fires due schedules from a local cache (fed by `SET_SCHEDULE`/
    `REMOVE_SCHEDULE` updates) against `getUnixTimestamp()`, fire-once per epoch-minute, opens valve
    for its duration; the auto-close task closes at the deadline.

## Verification done
Builds clean: `IRRIGATION_AC` (V5, V4) and `IRRIGATION` (V5) regression, SX1262. No warnings/errors.

## Needs bench validation (user flashes)
The always-awake state-machine integration and SSR timing can only be confirmed on hardware:
valve 0/1 on GPIO36/38 open and **hold** while running; node never sleeps; auto-close at deadline;
manual run/stop respond promptly; a stored schedule fires at its time; as a group master, a zone's
one-off run opens this node's valve quickly. `/bramble-flash IRRIGATION_AC` + `bramble-logs`.

## Known limitations / follow-ups
- **Boot schedule enumeration:** the local schedule cache is fed by the update stream only. After a
  reboot, existing PMU-FRAM schedules won't fire until re-pushed by the hub. Follow-up: enumerate via
  `reliable_pmu_->getSchedule(index, ...)` on boot (bounded), or a hub re-sync on registration.
- Local cache holds 32 schedules (indices ≥32 are skipped with a warning).
- Pure-AC only; v3 unsupported (no GPIO36/38); mixed DC+AC not supported.
