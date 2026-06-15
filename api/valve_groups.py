"""Master-valve group orchestration — pure compilation logic.

A valve group links N zone valves to one master valve (usually on a different
node). The master is a *series* belt-and-suspenders shutoff: water reaches a zone
only if both the zone valve and the master are open, so the master must be open
when — and only when — one of its zone valves is actively watering. Because nodes
sleep between a valve open and its auto-close, the master cannot be coordinated in
real time; instead the API mirrors each zone's watering window onto the master
node as its own schedule, so both nodes run autonomously from their RTCs.

This module holds the *pure* compilation (no DB/queue I/O) so it is unit-testable.
The only merging is of zone windows that genuinely overlap or abut in time —
required because the master is a single valve and the PMU rejects overlapping
schedules on the same valve. Non-overlapping (staggered) zones each get their own
master window, so the master stays CLOSED in the gaps (exact per-zone mirror).
"""
from models import MAX_SCHEDULE_SLOTS

SECONDS_PER_DAY = 86400
MAX_SCHEDULE_DURATION = 65535  # PMU schedule duration field is uint16_t
# Reserve the top slot for one-shot run-once, matching the zone-node convention.
USABLE_MASTER_SLOTS = MAX_SCHEDULE_SLOTS - 1


class MasterSlotOverflow(Exception):
    """Raised when a group needs more master schedule slots than are available."""

    def __init__(self, needed: int, available: int):
        self.needed = needed
        self.available = available
        super().__init__(
            f"master valve needs {needed} schedule slots but only {available} "
            f"are available")


def _day_intervals(schedules: list[dict]) -> dict:
    """Return {day: [(start_sec, end_sec), ...]} from zone schedules (UTC).

    Each schedule contributes an interval on each day in its bitmask (bit 0 =
    Sunday). A window that crosses midnight is split into the tail on the
    following day. A single zone duration is <= MAX_SCHEDULE_DURATION (< 1 day),
    so at most one midnight crossing per schedule.
    """
    by_day: dict = {d: [] for d in range(7)}
    for sched in schedules:
        duration = sched['duration']
        if duration <= 0:
            continue
        start = sched['hour'] * 3600 + sched['minute'] * 60
        end = start + duration
        for day in range(7):
            if not (sched['days'] >> day) & 1:
                continue
            if end <= SECONDS_PER_DAY:
                by_day[day].append((start, end))
            else:
                by_day[day].append((start, SECONDS_PER_DAY))
                by_day[(day + 1) % 7].append((0, end - SECONDS_PER_DAY))
    return by_day


def _merge(intervals: list) -> list:
    """Merge overlapping/abutting [start, end) intervals; return sorted list."""
    if not intervals:
        return []
    ordered = sorted(intervals)
    merged = [list(ordered[0])]
    for start, end in ordered[1:]:
        if start <= merged[-1][1]:  # overlaps or abuts the current run
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(s, e) for s, e in merged]


def _split_long(start: int, end: int) -> list:
    """Split [start, end) into consecutive chunks no longer than the PMU max
    duration so each fits a uint16_t. Chunks abut, so the master stays
    continuously open across the boundaries."""
    chunks = []
    cursor = start
    while end - cursor > MAX_SCHEDULE_DURATION:
        chunks.append((cursor, cursor + MAX_SCHEDULE_DURATION))
        cursor += MAX_SCHEDULE_DURATION
    chunks.append((cursor, end))
    return chunks


def compute_master_windows(member_schedules: list[dict], master_valve: int,
                           usable_slots: int = USABLE_MASTER_SLOTS) -> list[dict]:
    """Compile zone schedules into the master valve's desired schedule windows.

    Returns a deterministic list of {hour, minute, duration, days, valve} dicts.
    Raises MasterSlotOverflow if more than usable_slots distinct windows result.
    """
    by_day = _day_intervals(member_schedules)
    # (start_sec, duration_sec) shape -> days bitmask it appears on
    shape_days: dict = {}
    for day, intervals in by_day.items():
        for start, end in _merge(intervals):
            for chunk_start, chunk_end in _split_long(start, end):
                shape = (chunk_start, chunk_end - chunk_start)
                shape_days[shape] = shape_days.get(shape, 0) | (1 << day)

    if len(shape_days) > usable_slots:
        raise MasterSlotOverflow(len(shape_days), usable_slots)

    windows = []
    for (start, duration), days in sorted(shape_days.items()):
        windows.append({
            'hour': start // 3600,
            'minute': (start % 3600) // 60,
            'duration': duration,
            'days': days,
            'valve': master_valve,
        })
    return windows


def _window_key(window: dict) -> tuple:
    return (window['hour'], window['minute'], window['days'])


def diff_master_slots(desired: list[dict], stored_slots: list[dict],
                      group_id: int,
                      usable_slots: int = USABLE_MASTER_SLOTS) -> dict:
    """Diff desired master windows against the slots the API currently owns.

    Args:
        desired: windows from compute_master_windows (for this group).
        stored_slots: list_master_slots(master_device_id) — ALL slots on the
            master device (the index space is shared across any groups it
            masters), each carrying its group_id.
        group_id: the group being recomputed.

    Returns dict with:
        'to_set':    [{master_index, hour, minute, duration, days, valve}] -> SET_SCHEDULE
        'to_remove': [master_index, ...] -> REMOVE_SCHEDULE
        'slots':     full new slot rows for this group (for replace_master_slots)

    Matched windows (same hour/minute/days) keep their slot index and are only
    re-sent when the duration changed (window shrank/grew). Removed indices are
    freed for reuse. Raises MasterSlotOverflow if the device lacks free indices
    (only possible when one device masters several groups).
    """
    this_group = [s for s in stored_slots if s['group_id'] == group_id]
    used_by_others = {s['master_index'] for s in stored_slots
                      if s['group_id'] != group_id}
    stored_by_key = {_window_key(s): s for s in this_group}

    to_set: list = []
    new_slots: list = []
    matched_keys: set = set()
    reused_indices: set = set()
    unmatched: list = []

    for window in desired:
        key = _window_key(window)
        slot = stored_by_key.get(key)
        if slot is not None:
            index = slot['master_index']
            matched_keys.add(key)
            reused_indices.add(index)
            new_slots.append({**window, 'master_index': index})
            if slot['duration'] != window['duration']:
                to_set.append({**window, 'master_index': index})
        else:
            unmatched.append(window)

    to_remove = [s['master_index'] for s in this_group
                 if _window_key(s) not in matched_keys]

    occupied = used_by_others | reused_indices
    free_indices = [i for i in range(usable_slots) if i not in occupied]
    if len(free_indices) < len(unmatched):
        raise MasterSlotOverflow(
            len(used_by_others) + len(reused_indices) + len(unmatched),
            usable_slots)

    for window, index in zip(unmatched, free_indices):
        new_slots.append({**window, 'master_index': index})
        to_set.append({**window, 'master_index': index})

    # Don't REMOVE an index we immediately reused (a SET overwrites it in place).
    assigned = {s['master_index'] for s in new_slots}
    to_remove = [i for i in to_remove if i not in assigned]

    new_slots.sort(key=lambda s: s['master_index'])
    return {'to_set': to_set, 'to_remove': to_remove, 'slots': new_slots}
