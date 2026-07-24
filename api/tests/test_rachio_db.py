"""Tests for the rachio_zone_mappings DB layer."""

RACHIO_DEVICE = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
BRAMBLE_DEVICE = 0x123456789ABCDEF0


class TestRachioMappingDb:
    def test_upsert_and_get(self, temp_db):
        stored = temp_db.upsert_rachio_mapping(
            rachio_device_id=RACHIO_DEVICE, rachio_zone_number=1,
            bramble_device_id=BRAMBLE_DEVICE, bramble_valve=0,
            duration_seconds=900)
        assert stored['rachio_zone_number'] == 1
        assert stored['bramble_device_id'] == BRAMBLE_DEVICE
        assert stored['bramble_valve'] == 0
        assert stored['duration_seconds'] == 900
        assert stored['enabled'] is True

        got = temp_db.get_rachio_mapping(RACHIO_DEVICE, 1)
        assert got == stored

    def test_get_missing_returns_none(self, temp_db):
        assert temp_db.get_rachio_mapping(RACHIO_DEVICE, 99) is None

    def test_upsert_updates_existing(self, temp_db):
        temp_db.upsert_rachio_mapping(RACHIO_DEVICE, 1, BRAMBLE_DEVICE, 0, 900)
        updated = temp_db.upsert_rachio_mapping(
            RACHIO_DEVICE, 1, BRAMBLE_DEVICE, 2, 300, enabled=False)
        assert updated['bramble_valve'] == 2
        assert updated['duration_seconds'] == 300
        assert updated['enabled'] is False

        # Still a single row for the (device, zone) key.
        assert len(temp_db.get_all_rachio_mappings()) == 1

    def test_list_ordered(self, temp_db):
        temp_db.upsert_rachio_mapping(RACHIO_DEVICE, 3, BRAMBLE_DEVICE, 0, 600)
        temp_db.upsert_rachio_mapping(RACHIO_DEVICE, 1, BRAMBLE_DEVICE, 1, 600)
        temp_db.upsert_rachio_mapping(RACHIO_DEVICE, 2, BRAMBLE_DEVICE, 2, 600)
        zones = [m['rachio_zone_number'] for m in temp_db.get_all_rachio_mappings()]
        assert zones == [1, 2, 3]

    def test_delete(self, temp_db):
        temp_db.upsert_rachio_mapping(RACHIO_DEVICE, 1, BRAMBLE_DEVICE, 0, 900)
        assert temp_db.delete_rachio_mapping(RACHIO_DEVICE, 1) is True
        assert temp_db.get_rachio_mapping(RACHIO_DEVICE, 1) is None
        # Deleting again is a no-op that reports nothing removed.
        assert temp_db.delete_rachio_mapping(RACHIO_DEVICE, 1) is False
