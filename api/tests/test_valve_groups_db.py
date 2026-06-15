"""Tests for the valve-group database layer."""
import duckdb
import pytest

MASTER = 0x1111111111111111
ZONE_A = 0x2222222222222222
ZONE_B = 0x3333333333333333


def _members(*pairs):
    return [{'zone_device_id': dev, 'zone_valve': valve} for dev, valve in pairs]


class TestValveGroupCrud:
    def test_create_and_get(self, temp_db):
        group = temp_db.create_valve_group(
            'Front beds', MASTER, 3, _members((ZONE_A, 0), (ZONE_B, 1)))
        assert group['name'] == 'Front beds'
        assert group['master_device_id'] == str(MASTER)  # stringified
        assert group['master_valve'] == 3
        assert len(group['members']) == 2

        fetched = temp_db.get_valve_group(group['id'])
        assert fetched == group

    def test_get_all(self, temp_db):
        temp_db.create_valve_group('G1', MASTER, 0, _members((ZONE_A, 0)))
        temp_db.create_valve_group('G2', ZONE_B, 0, _members((ZONE_A, 1)))
        assert len(temp_db.get_all_valve_groups()) == 2

    def test_get_group_for_zone_valve(self, temp_db):
        group = temp_db.create_valve_group('G', MASTER, 3, _members((ZONE_A, 0)))
        found = temp_db.get_group_for_zone_valve(ZONE_A, 0)
        assert found['id'] == group['id']
        assert temp_db.get_group_for_zone_valve(ZONE_A, 1) is None

    def test_update_members(self, temp_db):
        group = temp_db.create_valve_group('G', MASTER, 3, _members((ZONE_A, 0)))
        temp_db.update_valve_group(group['id'], name='Renamed',
                                   members=_members((ZONE_B, 2)))
        updated = temp_db.get_valve_group(group['id'])
        assert updated['name'] == 'Renamed'
        assert updated['members'] == [{'zone_device_id': str(ZONE_B), 'zone_valve': 2}]
        # Old member no longer mapped.
        assert temp_db.get_group_for_zone_valve(ZONE_A, 0) is None

    def test_delete(self, temp_db):
        group = temp_db.create_valve_group('G', MASTER, 3, _members((ZONE_A, 0)))
        assert temp_db.delete_valve_group(group['id']) is True
        assert temp_db.get_valve_group(group['id']) is None
        assert temp_db.delete_valve_group(group['id']) is False


class TestValveGroupUniqueness:
    def test_zone_valve_in_one_group_only(self, temp_db):
        temp_db.create_valve_group('G1', MASTER, 3, _members((ZONE_A, 0)))
        with pytest.raises(duckdb.Error):
            temp_db.create_valve_group('G2', ZONE_B, 0, _members((ZONE_A, 0)))

    def test_master_valve_backs_one_group_only(self, temp_db):
        temp_db.create_valve_group('G1', MASTER, 3, _members((ZONE_A, 0)))
        with pytest.raises(duckdb.Error):
            temp_db.create_valve_group('G2', MASTER, 3, _members((ZONE_B, 0)))


class TestMasterSlots:
    def test_replace_and_list(self, temp_db):
        group = temp_db.create_valve_group('G', MASTER, 3, _members((ZONE_A, 0)))
        temp_db.replace_master_slots(group['id'], MASTER, [
            {'master_index': 0, 'hour': 6, 'minute': 0, 'duration': 900, 'days': 127},
            {'master_index': 1, 'hour': 18, 'minute': 0, 'duration': 600, 'days': 127},
        ])
        slots = temp_db.list_master_slots(MASTER)
        assert len(slots) == 2
        assert slots[0]['master_index'] == 0
        assert slots[0]['group_id'] == group['id']

        # Replace with a smaller set.
        temp_db.replace_master_slots(group['id'], MASTER, [
            {'master_index': 0, 'hour': 6, 'minute': 0, 'duration': 900, 'days': 127},
        ])
        assert len(temp_db.list_master_slots(MASTER)) == 1


class TestDeleteNodeCleanup:
    def test_delete_master_node_removes_group(self, temp_db):
        group = temp_db.create_valve_group('G', MASTER, 3, _members((ZONE_A, 0)))
        temp_db.replace_master_slots(group['id'], MASTER, [
            {'master_index': 0, 'hour': 6, 'minute': 0, 'duration': 900, 'days': 127}])
        # Seed a nodes row so delete_node finds the node to delete.
        temp_db.set_node_valve_count(MASTER, 4)

        assert temp_db.delete_node(MASTER) is True
        assert temp_db.get_valve_group(group['id']) is None
        assert temp_db.list_master_slots(MASTER) == []

    def test_delete_member_node_removes_membership(self, temp_db):
        group = temp_db.create_valve_group('G', MASTER, 3,
                                           _members((ZONE_A, 0), (ZONE_B, 1)))
        temp_db.set_node_valve_count(ZONE_A, 2)
        assert temp_db.delete_node(ZONE_A) is True
        remaining = temp_db.get_valve_group(group['id'])
        assert remaining is not None
        assert remaining['members'] == [{'zone_device_id': str(ZONE_B), 'zone_valve': 1}]
