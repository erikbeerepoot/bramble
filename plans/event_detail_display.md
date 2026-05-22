# Event detail display

## Goal

Make `Recent Events` in the dashboard show useful detail per event:

1. **Sub-minute timestamps** — 11 events in the screenshot all read `19:03`. We have
   second-precision data; we're just rendering `HH:mm`.
2. **Watering duration** — for a VALVE_OPEN / VALVE_TIMER_SET, surface how long the
   watering ran (or is running). Today there's no way to tell `15min` schedule from
   `5min Run` from the event log alone.
3. **Which valve** — events say "Valve Open" with no valve ID. We have the ID, we're
   not displaying it.

## What we have on the wire today

`src/util/event_record.h`:

```cpp
struct EventRecord {
    uint16_t uptime_offset;  // seconds since time reference
    uint8_t  event_type;     // EventType enum
    uint8_t  severity;       // 0/1/2
    uint16_t detail;         // event-specific
};  // 6 bytes packed
```

Hub serial bridge (`api/serial_interface.py:494-502`) packs `severity` + `detail`
into `data_hex` like `"000003"` and stores it in the `node_events` row. The
dashboard receives `NodeEvent { device_id, timestamp, event_code, data_hex }`
(`dashboard/src/types/index.ts:233`) but **never reads `data_hex`**.

For irrigation events the `detail` field currently carries:

| EventType         | detail meaning           | source                              |
|-------------------|--------------------------|-------------------------------------|
| VALVE_OPEN        | valve_id                 | `irrigation_mode.cpp:448, 358`      |
| VALVE_CLOSE       | valve_id                 | `irrigation_mode.cpp:453`           |
| VALVE_TIMER_SET   | valve_id                 | `irrigation_mode.cpp:237`           |
| VALVE_TIMER_CLOSE | valve_id                 | `irrigation_mode.cpp:345`           |
| SCHEDULE_APPLIED  | schedule index           | `irrigation_mode.cpp:552`           |
| SCHEDULE_REMOVED  | schedule index           | `irrigation_mode.cpp:579`           |
| SCHEDULE_FAILED   | schedule index           | `irrigation_mode.cpp:558, 583`      |

**Duration is not on the wire.** It has to be added.

Timestamps already have second precision end-to-end — only the render is truncated
(`dashboard/src/components/RecentEvents.tsx:214`, `:487-488`).

## Approach

Three independent changes, do them in order — each is shippable on its own.

### 1. Render `HH:mm:ss` instead of `HH:mm`

Trivial. Two `format(date, 'HH:mm')` call sites in `RecentEvents.tsx`:
- `:214` — single event row (`CopyableTimestamp`)
- `:487-488` — collapsed wake/sleep range label

Risk: the row gets a few pixels wider. Mono font already; should fit.

### 2. Render detail in the event label

Decode `data_hex` (already on `NodeEvent`) and append context:

- `VALVE_OPEN | VALVE_CLOSE | VALVE_TIMER_SET | VALVE_TIMER_CLOSE` → `"Valve N"` suffix (e.g. `"Valve Open · Valve 1"`)
- `SCHEDULE_APPLIED | SCHEDULE_REMOVED | SCHEDULE_FAILED` → `"#<index>"` suffix

Helper in `dashboard/src/types/index.ts` (next to `getEventName`):

```ts
export function getEventDetail(code: number, dataHex: string): string | null {
  if (!dataHex || dataHex.length < 6) return null;
  const detail = parseInt(dataHex.slice(2, 6), 16);
  switch (code) {
    case EventType.VALVE_OPEN:
    case EventType.VALVE_CLOSE:
    case EventType.VALVE_TIMER_SET:
    case EventType.VALVE_TIMER_CLOSE:
      return `Valve ${detail + 1}`;            // 1-indexed for display
    case EventType.SCHEDULE_APPLIED:
    case EventType.SCHEDULE_REMOVED:
    case EventType.SCHEDULE_FAILED:
      return `#${detail}`;
    default:
      return null;
  }
}
```

Render site is the event row span in `RecentEvents.tsx:564` and `:526` (collapsed
expansion). Append the detail with a subtle separator if non-null.

Decision needed: 0- or 1-indexed? Schedules list in screenshot already shows
`Valve 1 / Valve 2` (1-indexed) so match that.

### 3. Add duration to watering events

Two sub-options. Pick one.

**Option A — Pair on the dashboard.** Walk the event list; for each
`VALVE_OPEN(valve=X)` find the next `VALVE_TIMER_CLOSE(valve=X)` or
`VALVE_CLOSE(valve=X)` and compute `close.timestamp - open.timestamp`. Render
as `"Valve Open · Valve 1 · 60s"`.

- Pro: no firmware/protocol change. Works for events already in DB.
- Con: ambiguous if pairs span page boundaries (loadMore). Ambiguous if events arrive out of order or get dropped.
- Edge cases: open with no matching close → `"ongoing"`; close with no open → leave bare.

**Option B — Encode duration on the wire.** Repurpose the 16-bit `detail` to pack
both valve ID and duration. 4 bits valve (16 valves max — we use 4), 12 bits
duration in 4-second units = ~68 minutes max. For schedules that go longer,
saturate. Or use 16 bits for duration on `VALVE_TIMER_SET` only (drop valve ID
from that event — duration is the interesting bit, valve ID is implicit from the
preceding `VALVE_OPEN`).

- Pro: exact, no pairing logic.
- Con: protocol change. Need to update node firmware (`irrigation_mode.cpp:237, 232-239`), serial bridge (`api/serial_interface.py:494-502`), regenerate types.

**Recommendation: A first.** Most watering events come in pairs within one TX batch
(open → sleep enter → ... → wake → valve timer close, all in one wake cycle's
event log). Dashboard pairing covers 90% of real use. Revisit B if pairing
quality is poor in practice.

For Option A, the pairing function lives in `RecentEvents.tsx` next to
`collapseWakeCycles`. Pseudo:

```ts
function annotateDurations(events: NodeEvent[]): Map<number, number> {
  // events assumed sorted desc by timestamp (as the API returns them)
  const durationByOpenTs = new Map<number, number>();
  const openByValve = new Map<number, NodeEvent>();
  // walk oldest→newest, so reverse iterate
  for (let i = events.length - 1; i >= 0; i--) {
    const e = events[i];
    const valveId = parseInt(e.data_hex.slice(2, 6), 16);
    if (e.event_code === EventType.VALVE_OPEN) {
      openByValve.set(valveId, e);
    } else if (
      e.event_code === EventType.VALVE_CLOSE ||
      e.event_code === EventType.VALVE_TIMER_CLOSE
    ) {
      const open = openByValve.get(valveId);
      if (open) {
        durationByOpenTs.set(open.timestamp, e.timestamp - open.timestamp);
        openByValve.delete(valveId);
      }
    }
  }
  return durationByOpenTs;
}
```

Render: if `durationByOpenTs.has(event.timestamp)` and `event.event_code === VALVE_OPEN`,
append `· ${formatDuration(duration)}` (e.g. `60s`, `5m`, `15m 30s`).

## Open questions for the user

1. **1-indexed valves in display?** (Schedules already show `Valve 1` / `Valve 2`.)
2. **Where to put detail in the row** — same line (`"Valve Open · Valve 1 · 60s"`)
   or a second line under the event name? Single-line is more scannable; second
   line gives more breathing room when many events stack.
3. **Pairing only (Option A) or wire-format duration (Option B)?** Auto-mode-friendly
   if A; B requires firmware reflash on all irrigation nodes.

## File touch list

- `dashboard/src/types/index.ts` — add `getEventDetail`, `formatDuration` helpers
- `dashboard/src/components/RecentEvents.tsx` — sub-minute timestamps + detail + duration

If Option B is chosen later:
- `src/modes/irrigation_mode.cpp` — pack duration into detail
- `api/serial_interface.py` — unchanged (still ships `severity:detail`)
- `dashboard/src/types/index.ts` — decode new layout
