"""Bramble REST API - Flask application."""
from flask import Flask, request, jsonify, Response
from flask_cors import CORS
from flask_compress import Compress
import logging
import time
from typing import Optional

from config import Config
from serial_interface import SerialInterface
from database import SensorDatabase
from models import Node, NodeMetadata, Schedule, QueuedUpdate, Zone, format_firmware_version


# Setup logging
logging.basicConfig(
    level=logging.DEBUG if Config.DEBUG else logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Initialize Flask app
app = Flask(__name__)
app.config.from_object(Config)

# Enable CORS for all /api/* routes
CORS(app, resources={r"/api/*": {"origins": "*"}})

# Enable gzip compression for responses
Compress(app)

# Initialize database
sensor_db: Optional[SensorDatabase] = None

# Initialize serial interface
serial_interface: Optional[SerialInterface] = None


def get_database() -> SensorDatabase:
    """Get database instance, initializing if needed."""
    global sensor_db
    if sensor_db is None:
        sensor_db = SensorDatabase(Config.SENSOR_DB_PATH)
        sensor_db.init_db()
        logger.info(f"Sensor database initialized at {Config.SENSOR_DB_PATH}")
    return sensor_db


def get_serial() -> SerialInterface:
    """Get serial interface instance, initializing if needed."""
    global serial_interface
    if serial_interface is None:
        db = get_database()
        serial_interface = SerialInterface(database=db)
        serial_interface.connect()
    return serial_interface


@app.route('/api/health', methods=['GET'])
def health():
    """Health check endpoint."""
    try:
        serial = get_serial()
        connected = serial.serial and serial.serial.is_open
        return jsonify({
            'status': 'healthy' if connected else 'degraded',
            'serial_connected': connected,
            'serial_port': Config.SERIAL_PORT
        })
    except Exception as e:
        logger.error(f"Health check failed: {e}")
        return jsonify({
            'status': 'unhealthy',
            'error': str(e)
        }), 503


@app.route('/api/system/time', methods=['GET'])
def system_time():
    """Get current system time (for debugging)."""
    from datetime import datetime
    now = datetime.now()
    weekday = (now.weekday() + 1) % 7  # Convert to hub format (0=Sunday)

    return jsonify({
        'datetime': now.isoformat(),
        'formatted': now.strftime('%Y-%m-%d %H:%M:%S'),
        'weekday': weekday,
        'timestamp': int(now.timestamp())
    })


def _get_hub_queue_count(serial: SerialInterface, address: int) -> Optional[int]:
    """Get the hub queue count for a node.

    Args:
        serial: Serial interface to hub
        address: Node address

    Returns:
        Queue count or None if unavailable
    """
    try:
        responses = serial.send_command(f'GET_QUEUE {address}', timeout=1.0)
        if responses and responses[0].startswith('QUEUE'):
            parts = responses[0].split()
            if len(parts) >= 3:
                return int(parts[2])
    except Exception:
        pass
    return None


def _build_nodes_from_database():
    """Build node list from database when hub UART is unavailable.

    Uses nodes, node_status, and node_metadata tables to construct
    the same response shape as the hub-based path. Online status is
    inferred from the last heartbeat timestamp.
    """
    db = get_database()
    all_metadata = db.get_all_node_metadata()
    all_status = db.get_all_node_status()

    now = int(time.time())
    nodes = []

    for device_id, status in all_status.items():
        updated_at = status.get('updated_at', 0)
        last_seen_seconds = now - updated_at if updated_at else 0
        online = last_seen_seconds < 300  # Match hub's 5-minute maintenance interval

        node = Node(
            device_id=device_id,
            address=status.get('address', 0),
            node_type='SENSOR',  # Default; node_type not stored in node_status
            online=online,
            last_seen_seconds=last_seen_seconds,
            firmware_version=None  # Only available from hub memory
        )
        node_dict = node.to_dict()
        if device_id in all_metadata:
            node_dict['metadata'] = all_metadata[device_id]
        node_dict['status'] = status
        nodes.append(node_dict)

    return jsonify({
        'count': len(nodes),
        'nodes': nodes,
        'source': 'database'
    })


@app.route('/api/nodes', methods=['GET'])
def list_nodes():
    """List all registered nodes.

    Query parameters:
        include_queue: If 'true', include hub_queue_count for each node (slower)

    Returns:
        JSON array of node objects with metadata if available
    """
    try:
        serial = get_serial()

        # Retry LIST_NODES up to 3 times — the hub may not respond on the first
        # attempt if it is busy with LoRa operations or if UART bytes are lost.
        responses = None
        for attempt in range(Config.MAX_RETRIES):
            try:
                responses = serial.send_command('LIST_NODES', timeout=2.0)
                if responses and responses[0].startswith('NODE_LIST'):
                    break
                logger.warning(f"LIST_NODES attempt {attempt + 1}: invalid response {responses}")
                responses = None
            except TimeoutError:
                logger.warning(f"LIST_NODES attempt {attempt + 1}/{Config.MAX_RETRIES} timed out")

        if not responses or not responses[0].startswith('NODE_LIST'):
            logger.warning("LIST_NODES failed after retries, falling back to database")
            return _build_nodes_from_database()

        header = responses[0].split()
        count = int(header[1])

        # Get all metadata and status to include with nodes (keyed by device_id)
        db = get_database()
        all_metadata = db.get_all_node_metadata()
        all_status = db.get_all_node_status()

        # Check if we should include queue counts
        include_queue = request.args.get('include_queue', '').lower() == 'true'

        nodes = []
        for line in responses[1:]:
            if line.startswith('NODE '):
                parts = line.split()
                if len(parts) >= 6:
                    device_id = int(parts[2])
                    address = int(parts[1])
                    # Skip nodes without device_id (shouldn't happen normally)
                    if device_id == 0:
                        continue
                    # firmware_version is optional for backwards compatibility
                    firmware_version_raw = int(parts[6]) if len(parts) >= 7 else None
                    node = Node(
                        device_id=device_id,
                        address=address,
                        node_type=parts[3],
                        online=parts[4] == '1',
                        last_seen_seconds=int(parts[5]),
                        firmware_version=format_firmware_version(firmware_version_raw) if firmware_version_raw else None
                    )
                    node_dict = node.to_dict()
                    # Include metadata if available (keyed by device_id)
                    if device_id in all_metadata:
                        node_dict['metadata'] = all_metadata[device_id]
                    # Include status if available (keyed by device_id)
                    if device_id in all_status:
                        node_dict['status'] = all_status[device_id]
                    # Include hub queue count if requested (uses address for hub routing)
                    if include_queue:
                        node_dict['hub_queue_count'] = _get_hub_queue_count(serial, address)
                    nodes.append(node_dict)

        return jsonify({
            'count': count,
            'nodes': nodes,
            'source': 'hub'
        })

    except Exception as e:
        logger.error(f"Error listing nodes: {e}")
        return _build_nodes_from_database()


@app.route('/api/nodes/<int:device_id>', methods=['GET'])
def get_node(device_id: int):
    """Get details for a specific node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Returns:
        JSON node object
    """
    try:
        serial = get_serial()
        responses = serial.send_command('LIST_NODES')

        # Parse and find the node by device_id
        # Format: NODE <addr> <device_id> <type> <online> <last_seen_sec> [<firmware_version>]
        if responses and responses[0].startswith('NODE_LIST'):
            for line in responses[1:]:
                if line.startswith('NODE '):
                    parts = line.split()
                    if len(parts) >= 6 and int(parts[2]) == device_id:
                        address = int(parts[1])
                        firmware_version_raw = int(parts[6]) if len(parts) >= 7 else None
                        node = Node(
                            device_id=device_id,
                            address=address,
                            node_type=parts[3],
                            online=parts[4] == '1',
                            last_seen_seconds=int(parts[5]),
                            firmware_version=format_firmware_version(firmware_version_raw) if firmware_version_raw else None
                        )
                        return jsonify(node.to_dict())

            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        # Invalid response — fall through to database fallback
        logger.warning(f"Invalid LIST_NODES response for get_node, falling back to database")

    except (TimeoutError, RuntimeError):
        logger.warning(f"Hub unavailable for get_node({device_id}), falling back to database")

    except Exception as e:
        logger.error(f"Error getting node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500

    # Database fallback
    try:
        db = get_database()
        all_status = db.get_all_node_status()
        if device_id not in all_status:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        status = all_status[device_id]
        now = int(time.time())
        updated_at = status.get('updated_at', 0)
        last_seen_seconds = now - updated_at if updated_at else 0

        node = Node(
            device_id=device_id,
            address=status.get('address', 0),
            node_type='SENSOR',
            online=last_seen_seconds < 300,
            last_seen_seconds=last_seen_seconds,
            firmware_version=None
        )
        return jsonify(node.to_dict())
    except Exception as e:
        logger.error(f"Database fallback failed for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>', methods=['DELETE'])
def delete_node(device_id: int):
    """Delete a node and all its associated data.

    Removes the node from:
    - Hub firmware registry (via DELETE_NODE command, using address)
    - nodes table
    - node_metadata table
    - node_status table
    - node_status_history table
    - sensor_readings table

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Returns:
        JSON success message or 404 if not found
    """
    try:
        db = get_database()
        serial = get_serial()

        # Look up the node's address for hub command
        # First try the database
        node_info = db.get_node_by_device_id(device_id)
        address = node_info['address'] if node_info else None

        # If not in database, try to find address from hub's LIST_NODES
        if address is None:
            try:
                responses = serial.send_command('LIST_NODES')
                for line in responses[1:]:
                    if line.startswith('NODE '):
                        parts = line.split()
                        if len(parts) >= 3 and int(parts[2]) == device_id:
                            address = int(parts[1])
                            logger.info(f"Found node {device_id} address {address} from hub")
                            break
            except Exception as e:
                logger.warning(f"Could not query hub for node address: {e}")

        # Try to tell the hub to delete the node from its registry
        hub_deleted = False
        hub_error = None
        if address:
            try:
                responses = serial.send_command(f'DELETE_NODE {address}', timeout=2.0)
                if responses:
                    if responses[0].startswith('DELETED_NODE'):
                        hub_deleted = True
                        logger.info(f"Hub deleted node {address} from registry")
                    elif responses[0].startswith('ERROR'):
                        hub_error = responses[0]
                        logger.warning(f"Hub error deleting node {address}: {hub_error}")
            except Exception as e:
                hub_error = str(e)
                logger.warning(f"Could not send DELETE_NODE to hub: {e}")

        # Delete from database regardless of hub result
        db_deleted = db.delete_node(device_id)

        if db_deleted or hub_deleted:
            logger.info(f"Deleted node device_id={device_id} (hub={hub_deleted}, db={db_deleted})")
            return jsonify({
                'message': f'Node {device_id} deleted',
                'hub_deleted': hub_deleted,
                'db_deleted': db_deleted
            })
        else:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

    except Exception as e:
        logger.error(f"Error deleting node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/status', methods=['GET'])
def get_node_status(device_id: int):
    """Get status for a specific node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Returns:
        JSON status object or 404 if not found
    """
    try:
        db = get_database()
        status = db.get_node_status(device_id)

        if status:
            return jsonify(status)
        else:
            # Return empty status structure for nodes without status
            return jsonify({
                'device_id': str(device_id),  # String to preserve JS precision
                'address': None,
                'battery_level': None,
                'error_flags': None,
                'signal_strength': None,
                'uptime_seconds': None,
                'pending_records': None,
                'updated_at': None
            })

    except Exception as e:
        logger.error(f"Error getting status for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/error-history', methods=['GET'])
def get_node_error_history(device_id: int):
    """Get error flag history for a specific node.

    Returns history of error_flags changes over time, useful for
    tracking when errors like flash failures occurred.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Query parameters:
        start: Start timestamp (required)
        end: End timestamp (required)

    Returns:
        JSON array of error history entries
    """
    try:
        start_time = request.args.get('start', type=int)
        end_time = request.args.get('end', type=int)

        if start_time is None or end_time is None:
            return jsonify({'error': 'start and end query parameters are required'}), 400

        if start_time > end_time:
            return jsonify({'error': 'start must be <= end'}), 400

        db = get_database()
        history = db.get_node_error_history(device_id, start_time, end_time)

        return jsonify({
            'device_id': str(device_id),  # String to preserve JS precision
            'start': start_time,
            'end': end_time,
            'count': len(history),
            'history': history
        })

    except Exception as e:
        logger.error(f"Error getting error history for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/metadata', methods=['GET'])
def get_node_metadata(device_id: int):
    """Get metadata for a specific node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Returns:
        JSON metadata object or 404 if not found
    """
    try:
        db = get_database()
        metadata = db.get_node_metadata(device_id)

        if metadata:
            return jsonify(metadata)
        else:
            # Return empty metadata structure for nodes without metadata
            return jsonify({
                'device_id': str(device_id),  # String to preserve JS precision
                'name': None,
                'notes': None,
                'zone_id': None,
                'updated_at': None
            })

    except Exception as e:
        logger.error(f"Error getting metadata for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/metadata', methods=['PUT'])
def update_node_metadata(device_id: int):
    """Update metadata for a node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Request body:
        {
            "name": "Greenhouse Sensor",
            "notes": "Replaced battery 2024-01"
        }

    Returns:
        JSON updated metadata object
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        db = get_database()
        metadata = db.update_node_metadata(
            device_id=device_id,
            name=data.get('name'),
            notes=data.get('notes')
        )

        return jsonify(metadata)

    except Exception as e:
        logger.error(f"Error updating metadata for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/queue', methods=['GET'])
def get_queue(device_id: int):
    """Get pending updates queue for a node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Returns:
        JSON array of queued updates
    """
    try:
        db = get_database()
        node_info = db.get_node_by_device_id(device_id)
        if not node_info or not node_info.get('address'):
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        address = node_info['address']
        serial = get_serial()
        responses = serial.send_command(f'GET_QUEUE {address}')

        # Parse response
        # Format: QUEUE <addr> <count>
        #         UPDATE <seq> <type> <age_sec>
        if not responses or not responses[0].startswith('QUEUE'):
            return jsonify({'error': 'Invalid response from hub'}), 500

        header = responses[0].split()
        if len(header) < 3:
            return jsonify({'error': 'Invalid queue response format'}), 500

        count = int(header[2])

        updates = []
        for line in responses[1:]:
            if line.startswith('UPDATE '):
                parts = line.split()
                if len(parts) >= 4:
                    update = QueuedUpdate(
                        sequence=int(parts[1]),
                        update_type=parts[2],
                        age_seconds=int(parts[3])
                    )
                    updates.append(update.to_dict())

        return jsonify({
            'device_id': str(device_id),  # String to preserve JS precision
            'address': address,
            'count': count,
            'updates': updates
        })

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error getting queue for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/schedules', methods=['POST'])
def add_schedule(device_id: int):
    """Add or update a schedule entry for a node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Request body:
        {
            "index": 0,
            "hour": 14,
            "minute": 30,
            "duration": 900,
            "days": 127,
            "valve": 0
        }

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        db = get_database()
        node_info = db.get_node_by_device_id(device_id)
        if not node_info or not node_info.get('address'):
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        address = node_info['address']

        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        # Create and validate schedule
        try:
            schedule = Schedule(
                index=data['index'],
                hour=data['hour'],
                minute=data['minute'],
                duration=data['duration'],
                days=data['days'],
                valve=data['valve']
            )
        except KeyError as e:
            return jsonify({'error': f'Missing required field: {e}'}), 400

        # Validate
        errors = schedule.validate()
        if errors:
            return jsonify({'error': 'Validation failed', 'details': errors}), 400

        # Queue command for delivery (uses address for hub routing)
        from command_queue import queue_set_schedule

        result = queue_set_schedule(
            node_address=address,
            index=data['index'],
            hour=data['hour'],
            minute=data['minute'],
            duration=data['duration'],
            days=data['days'],
            valve=data['valve']
        )

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'device_id': str(device_id),  # String to preserve JS precision
            'address': address,
            'schedule': data,
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error adding schedule for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/schedules/<int:index>', methods=['DELETE'])
def remove_schedule(device_id: int, index: int):
    """Remove a schedule entry from a node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)
        index: Schedule index (0-7)

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        if not 0 <= index <= 7:
            return jsonify({'error': 'Schedule index must be 0-7'}), 400

        db = get_database()
        node_info = db.get_node_by_device_id(device_id)
        if not node_info or not node_info.get('address'):
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        address = node_info['address']

        # Queue command for delivery (uses address for hub routing)
        from command_queue import queue_remove_schedule

        result = queue_remove_schedule(node_address=address, index=index)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'device_id': str(device_id),  # String to preserve JS precision
            'address': address,
            'schedule_index': index,
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error removing schedule {index} for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/wake-interval', methods=['POST'])
def set_wake_interval(device_id: int):
    """Set periodic wake interval for a node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Request body:
        {
            "interval_seconds": 60
        }

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        db = get_database()
        node_info = db.get_node_by_device_id(device_id)
        if not node_info or not node_info.get('address'):
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        address = node_info['address']

        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        try:
            interval = int(data['interval_seconds'])
        except (KeyError, ValueError) as e:
            return jsonify({'error': 'Invalid interval_seconds value'}), 400

        if not 10 <= interval <= 3600:
            return jsonify({'error': 'interval_seconds must be 10-3600'}), 400

        # Queue command for delivery (uses address for hub routing)
        from command_queue import queue_set_wake_interval

        result = queue_set_wake_interval(node_address=address, interval_seconds=interval)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'device_id': str(device_id),  # String to preserve JS precision
            'address': address,
            'interval_seconds': interval,
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error setting wake interval for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/datetime', methods=['POST'])
def set_datetime(device_id: int):
    """Set date/time for a node's RTC.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Request body:
        {
            "year": 25,      # 2025 = 25
            "month": 10,     # 1-12
            "day": 19,       # 1-31
            "weekday": 7,    # 1-7 (1=Monday, 7=Sunday)
            "hour": 9,       # 0-23
            "minute": 4,     # 0-59
            "second": 0      # 0-59
        }

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        db = get_database()
        node_info = db.get_node_by_device_id(device_id)
        if not node_info or not node_info.get('address'):
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        address = node_info['address']

        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        # Extract and validate datetime fields
        try:
            year = int(data['year'])
            month = int(data['month'])
            day = int(data['day'])
            weekday = int(data['weekday'])
            hour = int(data['hour'])
            minute = int(data['minute'])
            second = int(data['second'])
        except (KeyError, ValueError) as e:
            return jsonify({'error': f'Invalid datetime field: {e}'}), 400

        # Validate ranges
        if not (0 <= year <= 99):
            return jsonify({'error': 'year must be 0-99 (e.g., 25 for 2025)'}), 400
        if not (1 <= month <= 12):
            return jsonify({'error': 'month must be 1-12'}), 400
        if not (1 <= day <= 31):
            return jsonify({'error': 'day must be 1-31'}), 400
        if not (1 <= weekday <= 7):
            return jsonify({'error': 'weekday must be 1-7 (1=Monday, 7=Sunday)'}), 400
        if not (0 <= hour <= 23):
            return jsonify({'error': 'hour must be 0-23'}), 400
        if not (0 <= minute <= 59):
            return jsonify({'error': 'minute must be 0-59'}), 400
        if not (0 <= second <= 59):
            return jsonify({'error': 'second must be 0-59'}), 400

        # Queue command for delivery (uses address for hub routing)
        from command_queue import queue_set_datetime

        result = queue_set_datetime(
            node_address=address,
            year=year,
            month=month,
            day=day,
            weekday=weekday,
            hour=hour,
            minute=minute,
            second=second
        )

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'device_id': str(device_id),  # String to preserve JS precision
            'address': address,
            'datetime': {
                'year': year,
                'month': month,
                'day': day,
                'weekday': weekday,
                'hour': hour,
                'minute': minute,
                'second': second
            },
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error setting datetime for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/reboot', methods=['POST'])
def reboot_node(address: int):
    """Request a remote reboot for a node.

    The reboot command is queued and will be delivered to the node
    on its next heartbeat. The node will perform a full system reset.

    Args:
        address: Node address

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        from command_queue import queue_reboot_node

        result = queue_reboot_node(node_address=address)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'node_address': address,
            'message': 'Reboot command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error queueing reboot for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


# ===== Sensor Data Endpoints =====

@app.route('/api/sensor-data', methods=['GET'])
def get_sensor_data():
    """Query sensor data with optional filters.

    Query parameters:
        device_id: Filter by device ID (optional)
        start_time: Start timestamp (optional)
        end_time: End timestamp (optional)
        limit: Maximum records to return (default 1000)
        offset: Number of records to skip (default 0)

    Returns:
        JSON array of sensor readings
    """
    try:
        db = get_database()

        # Parse query parameters
        device_id = request.args.get('device_id', type=int)
        start_time = request.args.get('start_time', type=int)
        end_time = request.args.get('end_time', type=int)
        limit = request.args.get('limit', default=1000, type=int)
        offset = request.args.get('offset', default=0, type=int)

        # Validate limits
        if limit < 1 or limit > 10000:
            return jsonify({'error': 'limit must be 1-10000'}), 400
        if offset < 0:
            return jsonify({'error': 'offset must be >= 0'}), 400

        # Query database
        readings = db.query_readings(
            device_id=device_id,
            start_time=start_time,
            end_time=end_time,
            limit=limit,
            offset=offset
        )

        # Get total count for pagination
        total = db.get_reading_count(
            device_id=device_id,
            start_time=start_time,
            end_time=end_time
        )

        return jsonify({
            'count': len(readings),
            'total': total,
            'limit': limit,
            'offset': offset,
            'readings': [r.to_dict() for r in readings]
        })

    except Exception as e:
        logger.error(f"Error querying sensor data: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/sensor-data/export', methods=['GET'])
def export_sensor_data():
    """Export sensor data as CSV.

    Query parameters:
        device_id: Filter by device ID (optional)
        start_time: Start timestamp (optional)
        end_time: End timestamp (optional)
        format: Export format (default 'csv')

    Returns:
        CSV file download
    """
    try:
        db = get_database()

        # Parse query parameters
        device_id = request.args.get('device_id', type=int)
        start_time = request.args.get('start_time', type=int)
        end_time = request.args.get('end_time', type=int)
        export_format = request.args.get('format', default='csv')

        if export_format != 'csv':
            return jsonify({'error': 'Only CSV format is currently supported'}), 400

        # Stream CSV rows to avoid loading all data into memory
        filename = f"sensor_data_{int(time.time())}.csv"
        return Response(
            db.export_csv_iter(
                device_id=device_id,
                start_time=start_time,
                end_time=end_time
            ),
            mimetype='text/csv',
            headers={'Content-Disposition': f'attachment; filename={filename}'}
        )

    except Exception as e:
        logger.error(f"Error exporting sensor data: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/sensor-data', methods=['GET'])
def get_node_sensor_data(device_id: int):
    """Get sensor data for a specific node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Query parameters:
        start_time: Start timestamp (optional, required for downsampling)
        end_time: End timestamp (optional, required for downsampling)
        limit: Maximum records to return (default 100, ignored when downsampling)
        downsample: Max data points for chart display (optional, enables bucket averaging)

    Returns:
        JSON array of sensor readings for this node
    """
    try:
        request_start = time.time()
        db = get_database()

        start_time = request.args.get('start_time', type=int)
        end_time = request.args.get('end_time', type=int)
        limit = request.args.get('limit', default=100, type=int)
        downsample = request.args.get('downsample', type=int)

        # Use downsampled query for chart display
        if downsample and start_time and end_time:
            query_start = time.time()
            readings = db.query_readings_downsampled(
                device_id=device_id,
                start_time=start_time,
                end_time=end_time,
                max_points=min(downsample, 2000)  # Cap at 2000 points
            )
            query_time = time.time() - query_start

            json_start = time.time()
            response_data = {
                'device_id': str(device_id),  # String to preserve JS precision
                'count': len(readings),
                'downsampled': True,
                'readings': readings,  # Already in chart format
                '_timing': {
                    'query_ms': round(query_time * 1000, 1),
                    'json_ms': round((time.time() - json_start) * 1000, 1),
                    'total_ms': round((time.time() - request_start) * 1000, 1),
                }
            }
            logger.info(f"sensor-data: query={query_time*1000:.0f}ms, readings={len(readings)}")
            return jsonify(response_data)

        # Standard query for full data
        readings = db.query_readings(
            device_id=device_id,
            start_time=start_time,
            end_time=end_time,
            limit=limit
        )

        return jsonify({
            'device_id': str(device_id),  # String to preserve JS precision
            'count': len(readings),
            'readings': [r.to_chart_dict() for r in readings]
        })

    except Exception as e:
        logger.error(f"Error getting sensor data for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/sensor-data/latest', methods=['GET'])
def get_node_latest(device_id: int):
    """Get the most recent sensor reading for a node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Returns:
        JSON sensor reading or 404 if not found
    """
    try:
        db = get_database()
        reading = db.get_latest_reading(device_id)

        if reading:
            return jsonify(reading.to_dict())
        else:
            return jsonify({'error': f'No readings found for node {device_id}'}), 404

    except Exception as e:
        logger.error(f"Error getting latest reading for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/statistics', methods=['GET'])
def get_node_statistics(device_id: int):
    """Get statistics for a specific node.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Query parameters:
        start_time: Start timestamp for stats calculation (optional)
        end_time: End timestamp for stats calculation (optional)

    Returns:
        JSON object with node statistics
    """
    try:
        request_start = time.time()
        db = get_database()

        start_time = request.args.get('start_time', type=int)
        end_time = request.args.get('end_time', type=int)

        query_start = time.time()
        stats = db.get_node_statistics(device_id, start_time=start_time, end_time=end_time)
        query_time = time.time() - query_start

        if stats:
            stats['_timing'] = {
                'query_ms': round(query_time * 1000, 1),
                'total_ms': round((time.time() - request_start) * 1000, 1),
            }
            logger.info(f"statistics: query={query_time*1000:.0f}ms")
            return jsonify(stats)
        else:
            return jsonify({'error': f'No data found for node {device_id}'}), 404

    except Exception as e:
        logger.error(f"Error getting statistics for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


# ===== Task Status Endpoint =====

@app.route('/api/tasks/<task_id>', methods=['GET'])
def get_task_status(task_id: str):
    """Check status of a queued command.

    Args:
        task_id: The task ID returned when command was queued

    Returns:
        JSON with task status (pending, completed, or failed)
    """
    try:
        from command_queue import huey

        result = huey.result(task_id)

        if result is None:
            return jsonify({'status': 'pending', 'task_id': task_id})
        elif isinstance(result, Exception):
            return jsonify({
                'status': 'failed',
                'task_id': task_id,
                'error': str(result)
            })
        else:
            return jsonify({
                'status': 'completed',
                'task_id': task_id,
                'result': result
            })

    except Exception as e:
        logger.error(f"Error getting task status for {task_id}: {e}")
        return jsonify({'error': str(e)}), 500


# ===== Zone Endpoints =====

@app.route('/api/zones', methods=['GET'])
def list_zones():
    """List all zones.

    Returns:
        JSON array of zone objects
    """
    try:
        db = get_database()
        zones = db.get_all_zones()
        return jsonify({
            'count': len(zones),
            'zones': zones
        })
    except Exception as e:
        logger.error(f"Error listing zones: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/zones', methods=['POST'])
def create_zone():
    """Create a new zone.

    Request body:
        {
            "name": "Greenhouse",
            "color": "#4CAF50",
            "description": "North greenhouse section"
        }

    Returns:
        JSON zone object (201 Created)
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        name = data.get('name')
        color = data.get('color')

        if not name:
            return jsonify({'error': 'name is required'}), 400
        if not color:
            return jsonify({'error': 'color is required'}), 400

        # Validate color format (hex color)
        if not color.startswith('#') or len(color) != 7:
            return jsonify({'error': 'color must be a hex color (e.g., #4CAF50)'}), 400

        db = get_database()
        zone = db.create_zone(
            name=name,
            color=color,
            description=data.get('description')
        )

        return jsonify(zone), 201

    except Exception as e:
        logger.error(f"Error creating zone: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/zones/<int:zone_id>', methods=['GET'])
def get_zone(zone_id: int):
    """Get a zone by ID.

    Args:
        zone_id: Zone ID

    Returns:
        JSON zone object or 404 if not found
    """
    try:
        db = get_database()
        zone = db.get_zone(zone_id)

        if zone:
            return jsonify(zone)
        else:
            return jsonify({'error': f'Zone {zone_id} not found'}), 404

    except Exception as e:
        logger.error(f"Error getting zone {zone_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/zones/<int:zone_id>', methods=['PUT'])
def update_zone(zone_id: int):
    """Update a zone.

    Args:
        zone_id: Zone ID

    Request body:
        {
            "name": "Updated Name",
            "color": "#FF5722",
            "description": "Updated description"
        }

    Returns:
        JSON updated zone object or 404 if not found
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        # Validate color format if provided
        color = data.get('color')
        if color and (not color.startswith('#') or len(color) != 7):
            return jsonify({'error': 'color must be a hex color (e.g., #4CAF50)'}), 400

        db = get_database()
        zone = db.update_zone(
            zone_id=zone_id,
            name=data.get('name'),
            color=color,
            description=data.get('description')
        )

        if zone:
            return jsonify(zone)
        else:
            return jsonify({'error': f'Zone {zone_id} not found'}), 404

    except Exception as e:
        logger.error(f"Error updating zone {zone_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/zones/<int:zone_id>', methods=['DELETE'])
def delete_zone(zone_id: int):
    """Delete a zone. Nodes in this zone become unzoned.

    Args:
        zone_id: Zone ID

    Returns:
        JSON success message or 404 if not found
    """
    try:
        db = get_database()
        deleted = db.delete_zone(zone_id)

        if deleted:
            return jsonify({'message': f'Zone {zone_id} deleted'})
        else:
            return jsonify({'error': f'Zone {zone_id} not found'}), 404

    except Exception as e:
        logger.error(f"Error deleting zone {zone_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/zone', methods=['PUT'])
def set_node_zone(device_id: int):
    """Set a node's zone.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Request body:
        {
            "zone_id": 1
        }
        or
        {
            "zone_id": null
        }
        to unzone

    Returns:
        JSON updated node metadata
    """
    try:
        data = request.get_json()
        if data is None:
            return jsonify({'error': 'Request body must be JSON'}), 400

        zone_id = data.get('zone_id')

        # Validate zone exists if not null
        if zone_id is not None:
            db = get_database()
            zone = db.get_zone(zone_id)
            if not zone:
                return jsonify({'error': f'Zone {zone_id} not found'}), 404

        db = get_database()
        metadata = db.set_node_zone(device_id, zone_id)

        if metadata:
            return jsonify(metadata)
        else:
            # Node metadata doesn't exist yet, create it
            metadata = db.update_node_metadata(device_id, zone_id=zone_id if zone_id else -1)
            return jsonify(metadata)

    except Exception as e:
        logger.error(f"Error setting zone for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.errorhandler(404)
def not_found(error):
    """Handle 404 errors."""
    return jsonify({'error': 'Endpoint not found'}), 404


@app.errorhandler(500)
def internal_error(error):
    """Handle 500 errors."""
    logger.error(f"Internal server error: {error}")
    return jsonify({'error': 'Internal server error'}), 500


def main():
    """Run the Flask application."""
    try:
        logger.info(f"Starting Bramble API on {Config.HOST}:{Config.PORT}")
        logger.info(f"Serial port: {Config.SERIAL_PORT} @ {Config.SERIAL_BAUD} baud")

        # Initialize serial interface on startup (don't wait for first API call)
        # Note: In debug mode, Flask restarts the app, so this runs twice
        # Use werkzeug.serving.is_running_from_reloader to detect reloader
        import os
        if not Config.DEBUG or os.environ.get('WERKZEUG_RUN_MAIN') == 'true':
            # Only connect in production or in the reloader subprocess
            logger.info("Connecting to hub via serial...")
            serial = get_serial()
            logger.info("Serial connection established - ready to respond to GET_DATETIME queries")

        app.run(
            host=Config.HOST,
            port=Config.PORT,
            debug=Config.DEBUG
        )
    except KeyboardInterrupt:
        logger.info("Shutting down...")
    finally:
        if serial_interface:
            serial_interface.disconnect()


if __name__ == '__main__':
    main()
