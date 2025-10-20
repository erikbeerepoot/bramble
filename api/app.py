"""Bramble REST API - Flask application."""
from flask import Flask, request, jsonify
import logging
from typing import Optional

from config import Config
from serial_interface import SerialInterface
from models import Node, Schedule, QueuedUpdate


# Setup logging
logging.basicConfig(
    level=logging.DEBUG if Config.DEBUG else logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Initialize Flask app
app = Flask(__name__)
app.config.from_object(Config)

# Initialize serial interface
serial_interface: Optional[SerialInterface] = None


def get_serial() -> SerialInterface:
    """Get serial interface instance, initializing if needed."""
    global serial_interface
    if serial_interface is None:
        serial_interface = SerialInterface()
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


@app.route('/api/nodes', methods=['GET'])
def list_nodes():
    """List all registered nodes.

    Returns:
        JSON array of node objects
    """
    try:
        serial = get_serial()
        responses = serial.send_command('LIST_NODES')

        # Parse response
        # Format: NODE_LIST <count>
        #         NODE <addr> <type> <online> <last_seen_sec>
        if not responses or not responses[0].startswith('NODE_LIST'):
            return jsonify({'error': 'Invalid response from hub'}), 500

        header = responses[0].split()
        count = int(header[1])

        nodes = []
        for line in responses[1:]:
            if line.startswith('NODE '):
                parts = line.split()
                if len(parts) >= 5:
                    node = Node(
                        address=int(parts[1]),
                        node_type=parts[2],
                        online=parts[3] == '1',
                        last_seen_seconds=int(parts[4])
                    )
                    nodes.append(node.to_dict())

        return jsonify({
            'count': count,
            'nodes': nodes
        })

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error listing nodes: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:addr>', methods=['GET'])
def get_node(addr: int):
    """Get details for a specific node.

    Args:
        addr: Node address

    Returns:
        JSON node object
    """
    try:
        serial = get_serial()
        responses = serial.send_command('LIST_NODES')

        # Parse and find the node
        for line in responses[1:]:
            if line.startswith('NODE '):
                parts = line.split()
                if len(parts) >= 5 and int(parts[1]) == addr:
                    node = Node(
                        address=int(parts[1]),
                        node_type=parts[2],
                        online=parts[3] == '1',
                        last_seen_seconds=int(parts[4])
                    )
                    return jsonify(node.to_dict())

        return jsonify({'error': f'Node {addr} not found'}), 404

    except Exception as e:
        logger.error(f"Error getting node {addr}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:addr>/queue', methods=['GET'])
def get_queue(addr: int):
    """Get pending updates queue for a node.

    Args:
        addr: Node address

    Returns:
        JSON array of queued updates
    """
    try:
        serial = get_serial()
        responses = serial.send_command(f'GET_QUEUE {addr}')

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
            'node_address': addr,
            'count': count,
            'updates': updates
        })

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error getting queue for node {addr}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:addr>/schedules', methods=['POST'])
def add_schedule(addr: int):
    """Add or update a schedule entry for a node.

    Args:
        addr: Node address

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
        JSON response with queue position
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

        # Send to hub
        serial = get_serial()
        command = schedule.to_hub_command(addr)
        responses = serial.send_command(command)

        # Parse response: QUEUED SET_SCHEDULE <addr> <position>
        if responses and responses[0].startswith('QUEUED'):
            parts = responses[0].split()
            return jsonify({
                'status': 'queued',
                'node_address': addr,
                'schedule': data,
                'position': int(parts[3]) if len(parts) > 3 else None
            }), 201

        return jsonify({'error': 'Unexpected response from hub'}), 500

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error adding schedule for node {addr}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:addr>/schedules/<int:index>', methods=['DELETE'])
def remove_schedule(addr: int, index: int):
    """Remove a schedule entry from a node.

    Args:
        addr: Node address
        index: Schedule index (0-7)

    Returns:
        JSON response with queue position
    """
    try:
        if not 0 <= index <= 7:
            return jsonify({'error': 'Schedule index must be 0-7'}), 400

        # Send to hub
        serial = get_serial()
        responses = serial.send_command(f'REMOVE_SCHEDULE {addr} {index}')

        # Parse response: QUEUED REMOVE_SCHEDULE <addr> <position>
        if responses and responses[0].startswith('QUEUED'):
            parts = responses[0].split()
            return jsonify({
                'status': 'queued',
                'node_address': addr,
                'schedule_index': index,
                'position': int(parts[3]) if len(parts) > 3 else None
            })

        return jsonify({'error': 'Unexpected response from hub'}), 500

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error removing schedule {index} for node {addr}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:addr>/wake-interval', methods=['POST'])
def set_wake_interval(addr: int):
    """Set periodic wake interval for a node.

    Args:
        addr: Node address

    Request body:
        {
            "interval_seconds": 60
        }

    Returns:
        JSON response with queue position
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

        # Send to hub
        serial = get_serial()
        responses = serial.send_command(f'SET_WAKE_INTERVAL {addr} {interval}')

        # Parse response: QUEUED SET_WAKE_INTERVAL <addr> <position>
        if responses and responses[0].startswith('QUEUED'):
            parts = responses[0].split()
            return jsonify({
                'status': 'queued',
                'node_address': addr,
                'interval_seconds': interval,
                'position': int(parts[3]) if len(parts) > 3 else None
            })

        return jsonify({'error': 'Unexpected response from hub'}), 500

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error setting wake interval for node {addr}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:addr>/datetime', methods=['POST'])
def set_datetime(addr: int):
    """Set date/time for a node's RTC.

    Args:
        addr: Node address

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
        JSON response with queue position
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

        # Send to hub: SET_DATETIME <addr> <year> <month> <day> <weekday> <hour> <minute> <second>
        serial = get_serial()
        cmd = f'SET_DATETIME {addr} {year} {month} {day} {weekday} {hour} {minute} {second}'
        responses = serial.send_command(cmd)

        # Parse response: QUEUED SET_DATETIME <addr> <position>
        if responses and responses[0].startswith('QUEUED'):
            parts = responses[0].split()
            return jsonify({
                'status': 'queued',
                'node_address': addr,
                'datetime': {
                    'year': year,
                    'month': month,
                    'day': day,
                    'weekday': weekday,
                    'hour': hour,
                    'minute': minute,
                    'second': second
                },
                'position': int(parts[3]) if len(parts) > 3 else None
            })

        return jsonify({'error': 'Unexpected response from hub'}), 500

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Error setting datetime for node {addr}: {e}")
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
