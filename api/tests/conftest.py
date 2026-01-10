"""Pytest fixtures for Bramble API tests."""
import os
import sys
import tempfile
import pytest

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from database import SensorDatabase, SensorReading


@pytest.fixture
def temp_db():
    """Create a temporary database for testing."""
    with tempfile.NamedTemporaryFile(suffix='.db', delete=False) as f:
        db_path = f.name

    os.environ['SENSOR_DB_PATH'] = db_path
    db = SensorDatabase(db_path)
    db.init_db()

    yield db

    # Cleanup
    try:
        os.unlink(db_path)
    except OSError:
        pass


@pytest.fixture
def seeded_db(temp_db):
    """Database pre-populated with test data."""
    import time
    import random

    base_time = int(time.time()) - 3600  # Start 1 hour ago
    node_address = 0x02

    for i in range(50):
        reading = SensorReading(
            node_address=node_address,
            timestamp=base_time + (i * 60),
            temperature_centidegrees=2200 + random.randint(-100, 100),
            humidity_centipercent=6500 + random.randint(-200, 200),
            received_at=base_time + (i * 60) + 1
        )
        temp_db.insert_reading(reading)

    return temp_db


@pytest.fixture
def app_client(temp_db):
    """Flask test client with mocked serial."""
    # Import here to avoid circular imports
    import app as flask_app

    # Prevent serial connection attempt
    flask_app.serial_interface = None
    flask_app.sensor_db = temp_db

    flask_app.app.config['TESTING'] = True

    with flask_app.app.test_client() as client:
        yield client
