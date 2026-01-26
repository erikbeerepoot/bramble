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
        responses = serial.send_command('LIST_NODES')

        # Parse response
        # Format: NODE_LIST <count>
        #         NODE <addr> <device_id> <type> <online> <last_seen_sec> [<firmware_version>]
        if not responses or not responses[0].startswith('NODE_LIST'):
            return jsonify({'error': 'Invalid response from hub'}), 500

        header = responses[0].split()
        count = int(header[1])

        # Get all metadata and status to include with nodes
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
                    # firmware_version is optional for backwards compatibility
                    firmware_version_raw = int(parts[6]) if len(parts) >= 7 else None
                    node = Node(
                        address=address,
                        device_id=device_id if device_id != 0 else None,
                        node_type=parts[3],
                        online=parts[4] == '1',
                        last_seen_seconds=int(parts[5]),
                        firmware_version=format_firmware_version(firmware_version_raw) if firmware_version_raw else None
                    )
                    node_dict = node.to_dict()
                    # Include metadata if available
                    if address in all_metadata:
                        node_dict['metadata'] = all_metadata[address]
                    # Include status if available
                    if address in all_status:
                        node_dict['status'] = all_status[address]
                    # Include hub queue count if requested
                    if include_queue:
                        node_dict['hub_queue_count'] = _get_hub_queue_count(serial, address)
                    nodes.append(node_dict)

        return jsonify({
            'count': count,
            'nodes': nodes
        })

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error listing nodes: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>', methods=['GET'])
def get_node(address: int):
    """Get details for a specific node.

    Args:
        address: Node address

    Returns:
        JSON node object
    """
    try:
        serial = get_serial()
        responses = serial.send_command('LIST_NODES')

        # Parse and find the node
        # Format: NODE <addr> <device_id> <type> <online> <last_seen_sec> [<firmware_version>]
        for line in responses[1:]:
            if line.startswith('NODE '):
                parts = line.split()
                if len(parts) >= 6 and int(parts[1]) == address:
                    device_id = int(parts[2])
                    firmware_version_raw = int(parts[6]) if len(parts) >= 7 else None
                    node = Node(
                        address=int(parts[1]),
                        device_id=device_id if device_id != 0 else None,
                        node_type=parts[3],
                        online=parts[4] == '1',
                        last_seen_seconds=int(parts[5]),
                        firmware_version=format_firmware_version(firmware_version_raw) if firmware_version_raw else None
                    )
                    return jsonify(node.to_dict())

        return jsonify({'error': f'Node {address} not found'}), 404

    except Exception as e:
        logger.error(f"Error getting node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/status', methods=['GET'])
def get_node_status(address: int):
    """Get status for a specific node.

    Args:
        address: Node address

    Returns:
        JSON status object or 404 if not found
    """
    try:
        db = get_database()
        status = db.get_node_status(address)

        if status:
            return jsonify(status)
        else:
            # Return empty status structure for nodes without status
            return jsonify({
                'address': address,
                'device_id': None,
                'battery_level': None,
                'error_flags': None,
                'signal_strength': None,
                'uptime_seconds': None,
                'pending_records': None,
                'updated_at': None
            })

    except Exception as e:
        logger.error(f"Error getting status for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/metadata', methods=['GET'])
def get_node_metadata(address: int):
    """Get metadata for a specific node.

    Args:
        address: Node address

    Returns:
        JSON metadata object or 404 if not found
    """
    try:
        db = get_database()
        metadata = db.get_node_metadata(address)

        if metadata:
            return jsonify(metadata)
        else:
            # Return empty metadata structure for nodes without metadata
            return jsonify({
                'address': address,
                'name': None,
                'location': None,
                'notes': None,
                'updated_at': None
            })

    except Exception as e:
        logger.error(f"Error getting metadata for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/metadata', methods=['PUT'])
def update_node_metadata(address: int):
    """Update metadata for a node.

    Args:
        address: Node address

    Request body:
        {
            "name": "Greenhouse Sensor",
            "location": "North greenhouse, row 3",
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
            address=address,
            name=data.get('name'),
            location=data.get('location'),
            notes=data.get('notes')
        )

        return jsonify(metadata)

    except Exception as e:
        logger.error(f"Error updating metadata for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/queue', methods=['GET'])
def get_queue(address: int):
    """Get pending updates queue for a node.

    Args:
        address: Node address

    Returns:
        JSON array of queued updates
    """
    try:
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
            'node_address': address,
            'count': count,
            'updates': updates
        })

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error getting queue for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/schedules', methods=['POST'])
def add_schedule(address: int):
    """Add or update a schedule entry for a node.

    Args:
        address: Node address

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

        # Queue command for delivery
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
            'node_address': address,
            'schedule': data,
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error adding schedule for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/schedules/<int:index>', methods=['DELETE'])
def remove_schedule(address: int, index: int):
    """Remove a schedule entry from a node.

    Args:
        address: Node address
        index: Schedule index (0-7)

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        if not 0 <= index <= 7:
            return jsonify({'error': 'Schedule index must be 0-7'}), 400

        # Queue command for delivery
        from command_queue import queue_remove_schedule

        result = queue_remove_schedule(node_address=address, index=index)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'node_address': address,
            'schedule_index': index,
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error removing schedule {index} for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/wake-interval', methods=['POST'])
def set_wake_interval(address: int):
    """Set periodic wake interval for a node.

    Args:
        address: Node address

    Request body:
        {
            "interval_seconds": 60
        }

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        try:
            interval = int(data['interval_seconds'])
        except (KeyError, ValueError) as e:
            return jsonify({'error': 'Invalid interval_seconds value'}), 400

        if not 10 <= interval <= 3600:
            return jsonify({'error': 'interval_seconds must be 10-3600'}), 400

        # Queue command for delivery
        from command_queue import queue_set_wake_interval

        result = queue_set_wake_interval(node_address=address, interval_seconds=interval)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'node_address': address,
            'interval_seconds': interval,
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error setting wake interval for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/datetime', methods=['POST'])
def set_datetime(address: int):
    """Set date/time for a node's RTC.

    Args:
        address: Node address

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

        # Queue command for delivery
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
            'node_address': address,
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
        logger.error(f"Error setting datetime for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


# ===== Sensor Data Endpoints =====

@app.route('/api/sensor-data', methods=['GET'])
def get_sensor_data():
    """Query sensor data with optional filters.

    Query parameters:
        node_address: Filter by node address (optional)
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
        node_address = request.args.get('node_address', type=int)
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
            node_address=node_address,
            start_time=start_time,
            end_time=end_time,
            limit=limit,
            offset=offset
        )

        # Get total count for pagination
        total = db.get_reading_count(
            node_address=node_address,
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
        node_address: Filter by node address (optional)
        start_time: Start timestamp (optional)
        end_time: End timestamp (optional)
        format: Export format (default 'csv')

    Returns:
        CSV file download
    """
    try:
        db = get_database()

        # Parse query parameters
        node_address = request.args.get('node_address', type=int)
        start_time = request.args.get('start_time', type=int)
        end_time = request.args.get('end_time', type=int)
        export_format = request.args.get('format', default='csv')

        if export_format != 'csv':
            return jsonify({'error': 'Only CSV format is currently supported'}), 400

        # Generate CSV
        csv_content = db.export_csv(
            node_address=node_address,
            start_time=start_time,
            end_time=end_time
        )

        # Return as downloadable file
        filename = f"sensor_data_{int(time.time())}.csv"
        return Response(
            csv_content,
            mimetype='text/csv',
            headers={'Content-Disposition': f'attachment; filename={filename}'}
        )

    except Exception as e:
        logger.error(f"Error exporting sensor data: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/sensor-data', methods=['GET'])
def get_node_sensor_data(address: int):
    """Get sensor data for a specific node.

    Args:
        address: Node address

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
                node_address=address,
                start_time=start_time,
                end_time=end_time,
                max_points=min(downsample, 2000)  # Cap at 2000 points
            )
            query_time = time.time() - query_start

            json_start = time.time()
            response_data = {
                'node_address': address,
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
            node_address=address,
            start_time=start_time,
            end_time=end_time,
            limit=limit
        )

        return jsonify({
            'node_address': address,
            'count': len(readings),
            'readings': [r.to_chart_dict() for r in readings]
        })

    except Exception as e:
        logger.error(f"Error getting sensor data for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/sensor-data/latest', methods=['GET'])
def get_node_latest(address: int):
    """Get the most recent sensor reading for a node.

    Args:
        address: Node address

    Returns:
        JSON sensor reading or 404 if not found
    """
    try:
        db = get_database()
        reading = db.get_latest_reading(address)

        if reading:
            return jsonify(reading.to_dict())
        else:
            return jsonify({'error': f'No readings found for node {address}'}), 404

    except Exception as e:
        logger.error(f"Error getting latest reading for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:address>/statistics', methods=['GET'])
def get_node_statistics(address: int):
    """Get statistics for a specific node.

    Args:
        address: Node address

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
        stats = db.get_node_statistics(address, start_time=start_time, end_time=end_time)
        query_time = time.time() - query_start

        if stats:
            stats['_timing'] = {
                'query_ms': round(query_time * 1000, 1),
                'total_ms': round((time.time() - request_start) * 1000, 1),
            }
            logger.info(f"statistics: query={query_time*1000:.0f}ms")
            return jsonify(stats)
        else:
            return jsonify({'error': f'No data found for node {address}'}), 404

    except Exception as e:
        logger.error(f"Error getting statistics for node {address}: {e}")
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


@app.route('/api/nodes/<int:address>/zone', methods=['PUT'])
def set_node_zone(address: int):
    """Set a node's zone.

    Args:
        address: Node address

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
        metadata = db.set_node_zone(address, zone_id)

        if metadata:
            return jsonify(metadata)
        else:
            # Node metadata doesn't exist yet, create it
            metadata = db.update_node_metadata(address, zone_id=zone_id if zone_id else -1)
            return jsonify(metadata)

    except Exception as e:
        logger.error(f"Error setting zone for node {address}: {e}")
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
