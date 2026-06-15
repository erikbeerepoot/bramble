"""Unit tests for the master-valve mirror engine (pure logic, no DB/IO)."""
import pytest

from valve_groups import (compute_master_windows, diff_master_slots,
                          MasterSlotOverflow, USABLE_MASTER_SLOTS)

ALL_DAYS = 0b1111111  # 127


def _sched(hour, minute, duration, days=ALL_DAYS, valve=0, index=0):
    return {'index': index, 'hour': hour, 'minute': minute,
            'duration': duration, 'days': days, 'valve': valve}


class TestComputeMasterWindows:
    def test_empty(self):
        assert compute_master_windows([], master_valve=3) == []

    def test_single_window_mirrored(self):
        # 06:00 for 15 min, every day -> one master window, same shape.
        windows = compute_master_windows([_sched(6, 0, 900)], master_valve=3)
        assert windows == [{'hour': 6, 'minute': 0, 'duration': 900,
                            'days': ALL_DAYS, 'valve': 3}]

    def test_overlapping_zones_merge_to_union(self):
        # zone A 06:00-06:15, zone B 06:10-06:25 (overlap) -> one 06:00-06:25.
        windows = compute_master_windows(
            [_sched(6, 0, 900), _sched(6, 10, 900)], master_valve=0)
        assert len(windows) == 1
        assert windows[0]['hour'] == 6 and windows[0]['minute'] == 0
        assert windows[0]['duration'] == 25 * 60

    def test_abutting_zones_merge(self):
        # Back-to-back 06:00-06:15 and 06:15-06:30 -> one continuous 06:00-06:30.
        windows = compute_master_windows(
            [_sched(6, 0, 900), _sched(6, 15, 900)], master_valve=0)
        assert len(windows) == 1
        assert windows[0]['duration'] == 30 * 60

    def test_staggered_zones_stay_separate(self):
        # Gap between zones -> master closed in the gap -> two windows.
        windows = compute_master_windows(
            [_sched(6, 0, 900), _sched(6, 20, 900)], master_valve=0)
        assert len(windows) == 2
        starts = sorted((w['hour'], w['minute']) for w in windows)
        assert starts == [(6, 0), (6, 20)]

    def test_same_window_across_days_regroups_to_one_slot(self):
        # Same time on Mon/Wed/Fri (separate schedules) -> one window, combined days.
        mon, wed, fri = 0b0000010, 0b0001000, 0b0100000
        windows = compute_master_windows([
            _sched(6, 0, 900, days=mon, index=0),
            _sched(6, 0, 900, days=wed, index=1),
            _sched(6, 0, 900, days=fri, index=2),
        ], master_valve=0)
        assert len(windows) == 1
        assert windows[0]['days'] == (mon | wed | fri)

    def test_distinct_times_distinct_slots(self):
        windows = compute_master_windows(
            [_sched(6, 0, 900), _sched(18, 0, 1800)], master_valve=0)
        assert len(windows) == 2

    def test_midnight_crossing_splits_into_next_day(self):
        # Sunday 23:30 for 1h -> Sun 23:30-24:00 and Mon 00:00-00:30.
        sun = 0b0000001
        windows = compute_master_windows(
            [_sched(23, 30, 3600, days=sun)], master_valve=0)
        assert len(windows) == 2
        by_start = {(w['hour'], w['minute']): w for w in windows}
        assert by_start[(23, 30)]['duration'] == 30 * 60
        assert by_start[(23, 30)]['days'] == sun
        assert by_start[(0, 0)]['duration'] == 30 * 60
        assert by_start[(0, 0)]['days'] == 0b0000010  # Monday

    def test_overflow_raises(self):
        # More distinct windows than usable slots -> overflow. Space each 5-min
        # window 10 min apart (non-overlapping, valid hours) so each is its own
        # master slot.
        scheds = []
        for i in range(USABLE_MASTER_SLOTS + 1):
            total_min = i * 10
            scheds.append(_sched(total_min // 60, total_min % 60, 300, index=i))
        with pytest.raises(MasterSlotOverflow):
            compute_master_windows(scheds, master_valve=0)

    def test_long_merged_window_split_to_fit_uint16_duration(self):
        # A continuous span longer than 65535s must be split into chunks that fit.
        # Two back-to-back 18000s (5h) windows = 10h continuous = 36000s < 65535,
        # so build a longer chain: 06:00 for 65000s + abutting next.
        windows = compute_master_windows([
            _sched(0, 0, 65000, index=0),
            _sched(18, 3, 20000, index=1),  # starts at 65000-ish boundary region
        ], master_valve=0)
        for w in windows:
            assert w['duration'] <= 65535


class TestDiffMasterSlots:
    def test_all_new(self):
        desired = [{'hour': 6, 'minute': 0, 'duration': 900, 'days': ALL_DAYS, 'valve': 0}]
        diff = diff_master_slots(desired, [], group_id=1)
        assert len(diff['to_set']) == 1
        assert diff['to_set'][0]['master_index'] == 0
        assert diff['to_remove'] == []
        assert len(diff['slots']) == 1

    def test_unchanged_no_set(self):
        desired = [{'hour': 6, 'minute': 0, 'duration': 900, 'days': ALL_DAYS, 'valve': 0}]
        stored = [{'group_id': 1, 'master_index': 0, 'hour': 6, 'minute': 0,
                   'duration': 900, 'days': ALL_DAYS}]
        diff = diff_master_slots(desired, stored, group_id=1)
        assert diff['to_set'] == []
        assert diff['to_remove'] == []

    def test_duration_change_resends_same_index(self):
        # Window shrank (a contributing zone removed) -> SET same slot in place.
        desired = [{'hour': 6, 'minute': 0, 'duration': 600, 'days': ALL_DAYS, 'valve': 0}]
        stored = [{'group_id': 1, 'master_index': 0, 'hour': 6, 'minute': 0,
                   'duration': 900, 'days': ALL_DAYS}]
        diff = diff_master_slots(desired, stored, group_id=1)
        assert len(diff['to_set']) == 1
        assert diff['to_set'][0]['master_index'] == 0
        assert diff['to_set'][0]['duration'] == 600
        assert diff['to_remove'] == []

    def test_removed_window(self):
        desired = []
        stored = [{'group_id': 1, 'master_index': 2, 'hour': 6, 'minute': 0,
                   'duration': 900, 'days': ALL_DAYS}]
        diff = diff_master_slots(desired, stored, group_id=1)
        assert diff['to_remove'] == [2]
        assert diff['slots'] == []

    def test_other_groups_indices_avoided(self):
        # Index 0 used by another group on the same device -> new window gets 1.
        desired = [{'hour': 7, 'minute': 0, 'duration': 900, 'days': ALL_DAYS, 'valve': 0}]
        stored = [{'group_id': 99, 'master_index': 0, 'hour': 6, 'minute': 0,
                   'duration': 900, 'days': ALL_DAYS}]
        diff = diff_master_slots(desired, stored, group_id=1)
        assert diff['to_set'][0]['master_index'] == 1
        assert diff['to_remove'] == []  # other group's slot untouched
