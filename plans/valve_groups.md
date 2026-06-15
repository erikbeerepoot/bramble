# Valve Groups (Master Valve)

Status: implemented on branch `feature/valve-groups-master-valve`. Full design:
`~/.claude/plans/virtual-sniffing-hanrahan.md`.

## What it does
A **valve group** links N zone valves to one **master valve** (usually on a different node). The
master is a *series* belt-and-suspenders shutoff: water reaches a zone only if both the zone valve
and the master are open. So the master must err **CLOSED** and open only while a specific zone is
actively watering (exact per-zone mirror, closed in the gaps between staggered zones).

## How (API-orchestrated, autonomous nodes)
Nodes sleep between a valve open and its auto-close, so the master can't be coordinated in real time.
Instead the API mirrors each zone watering window onto the **master node as its own schedule**; both
nodes run autonomously from hub-synced RTCs. Only genuinely overlapping zone windows are merged (the
PMU rejects overlapping schedules on one valve). Manual run/stop fan out to the master too.

## Implemented
- **Firmware (PMU):** `MAX_SCHEDULE_ENTRIES` 8 → 100 (`pmu-stm32/Core/Inc/pmu_protocol.h`); FRAM
  `FORMAT_VERSION` 2 → 3 (`persistent_storage.h`) so the auto-derived layout reformats cleanly on
  upgrade. **User must build/flash the PMU.**
- **API index range:** schedule index widened from 0-7 to 0-99 (`models.py`, `app.py`); dashboard
  slot loop too (`IrrigationControl.tsx`).
- **DB (`api/database.py`):** tables `valve_groups`, `valve_group_members`,
  `valve_group_master_slots`; CRUD + `get_group_for_zone_valve` + `list/replace_master_slots`;
  `delete_node` cleanup.
- **Mirror engine (`api/valve_groups.py`, pure):** `compute_master_windows` (per-day overlap merge,
  midnight split, long-window split to fit uint16 duration, distinct-shape day regroup, slot-cap →
  `MasterSlotOverflow`) and `diff_master_slots` (recompute + minimal SET/REMOVE diff, in-place
  shrink/grow, index reuse).
- **Endpoints (`api/app.py`):** `GET/POST /api/valve-groups`, `GET/PUT/DELETE
  /api/valve-groups/<id>`, `POST /api/valve-groups/<id>/resync`; `add_schedule`/`remove_schedule`/
  `run_valve`/`stop_valve` are group-aware (atomic 409 on master slot overflow). Master commands get
  their own `node_commands` audit rows (auto-confirmed by existing event reconcilers).
- **Dashboard:** `components/ValveGroups.tsx` (list/create/edit/delete/resync), nav route, and
  read-only group badges in `IrrigationControl.tsx`.

## Tests
`api/tests/test_valve_groups_engine.py`, `_db.py`, `_api.py` — 34 passing (engine algorithm, DB
layer + uniqueness + cleanup, endpoint wiring). Dashboard `npm run build` + `tsc --noEmit` clean.

## Known limitations / follow-ups
- **TTL expiry** (30 min) can leave the master out of sync → that zone waters with the master closed.
  Mitigated by the manual `/resync`. A periodic reconciler is a recommended v1.1.
- v1 enforces one group per master valve and one group per zone valve.
