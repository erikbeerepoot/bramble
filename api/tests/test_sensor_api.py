"""Tests for sensor data API endpoints."""
import time
from database import SensorReading


class TestSensorDataEndpoints:
    """Test /api/sensor-data endpoints."""

    def test_get_sensor_data_empty(self, app_client, temp_db):
        """Empty database returns empty list."""
        response = app_client.get('/api/sensor-data')
        assert response.status_code == 200

        data = response.get_json()
        assert data['count'] == 0
        assert data['readings'] == []

    def test_get_sensor_data_with_readings(self, app_client, seeded_db):
        """Returns readings from database."""
        response = app_client.get('/api/sensor-data')
        assert response.status_code == 200

        data = response.get_json()
        assert data['count'] == 50
        assert len(data['readings']) == 50

    def test_get_sensor_data_with_limit(self, app_client, seeded_db):
        """Respects limit parameter."""
        response = app_client.get('/api/sensor-data?limit=10')
        assert response.status_code == 200

        data = response.get_json()
        assert data['count'] == 10
        assert data['total'] == 50

    def test_get_sensor_data_filter_by_node(self, app_client, seeded_db):
        """Filters by device ID."""
        # Device 0x123456789ABCDEF0 has data (from seeded_db fixture)
        response = app_client.get('/api/sensor-data?device_id=1311768467463790320')
        assert response.status_code == 200
        assert response.get_json()['count'] == 50

        # Different device ID has no data
        response = app_client.get('/api/sensor-data?device_id=99')
        assert response.status_code == 200
        assert response.get_json()['count'] == 0


class TestNodeSensorEndpoints:
    """Test /api/nodes/<addr>/sensor-data endpoints."""

    def test_get_node_latest_reading(self, app_client, seeded_db):
        """Returns most recent reading for node."""
        # Use device_id from seeded_db fixture: 0x123456789ABCDEF0
        response = app_client.get('/api/nodes/1311768467463790320/sensor-data/latest')
        assert response.status_code == 200

        data = response.get_json()
        assert data['device_id'] == '1311768467463790320'
        assert 'temperature_celsius' in data
        assert 'humidity_percent' in data

    def test_get_node_latest_not_found(self, app_client, temp_db):
        """Returns 404 for node with no readings."""
        response = app_client.get('/api/nodes/99/sensor-data/latest')
        assert response.status_code == 404

    def test_get_node_statistics(self, app_client, seeded_db):
        """Returns statistics for node."""
        # Use device_id from seeded_db fixture: 0x123456789ABCDEF0
        response = app_client.get('/api/nodes/1311768467463790320/statistics')
        assert response.status_code == 200

        data = response.get_json()
        assert 'total_readings' in data
        assert data['total_readings'] == 50
        assert 'temperature' in data
        assert 'min_celsius' in data['temperature']
        assert 'max_celsius' in data['temperature']


class TestDatabaseLayer:
    """Direct database tests."""

    def test_insert_and_retrieve(self, temp_db):
        """Basic insert and query."""
        device_id = 0xDEADBEEF12345678
        reading = SensorReading(
            device_id=device_id,
            timestamp=int(time.time()),
            temperature_centidegrees=2534,
            humidity_centipercent=6521,
            received_at=int(time.time()),
            address=0x05
        )
        temp_db.insert_reading(reading)

        readings = temp_db.query_readings(device_id=device_id)
        assert len(readings) == 1
        assert readings[0].temperature_celsius == 25.34
        assert readings[0].humidity_percent == 65.21

    def test_device_id_stored_and_retrieved(self, temp_db):
        """Device ID is properly stored and retrieved."""
        device_id = 0xDEADBEEF12345678
        reading = SensorReading(
            device_id=device_id,
            timestamp=int(time.time()),
            temperature_centidegrees=2200,
            humidity_centipercent=6000,
            received_at=int(time.time()),
            address=0x05
        )
        temp_db.insert_reading(reading)

        readings = temp_db.query_readings(device_id=device_id)
        assert len(readings) == 1
        assert readings[0].device_id == device_id

    def test_device_id_in_to_dict(self, temp_db):
        """Device ID appears in to_dict output."""
        device_id = 0xCAFEBABE
        reading = SensorReading(
            device_id=device_id,
            timestamp=int(time.time()),
            temperature_centidegrees=2200,
            humidity_centipercent=6000,
            received_at=int(time.time()),
            address=0x05
        )
        temp_db.insert_reading(reading)

        readings = temp_db.query_readings(device_id=device_id)
        result = readings[0].to_dict()
        assert 'device_id' in result
        assert result['device_id'] == device_id

    def test_duplicate_rejected(self, temp_db):
        """Duplicate node+timestamp is rejected."""
        timestamp = int(time.time())
        device_id = 0xDEADBEEF12345678
        reading = SensorReading(
            device_id=device_id,
            timestamp=timestamp,
            temperature_centidegrees=2500,
            humidity_centipercent=6500,
            received_at=timestamp,
            address=0x05
        )
        temp_db.insert_reading(reading)

        # Same node+timestamp should fail
        duplicate = SensorReading(
            device_id=device_id,
            timestamp=timestamp,
            temperature_centidegrees=2600,
            humidity_centipercent=7000,
            received_at=timestamp,
            address=0x05
        )
        # Should not raise, but also shouldn't insert
        try:
            temp_db.insert_reading(duplicate)
        except Exception:
            pass  # Expected

        readings = temp_db.query_readings(device_id=device_id)
        assert len(readings) == 1
