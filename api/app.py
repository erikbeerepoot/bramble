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
from models import (Node, NodeMetadata, Schedule, QueuedUpdate, Zone,
                    MAX_SCHEDULE_SLOTS, format_firmware_version)
from valve_groups import (compute_master_windows, diff_master_slots,
                          MasterSlotOverflow)
from auth import require_token


# Setup logging
logging.basicConfig(
    level=logging.DEBUG if Config.DEBUG else logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Initialize Flask app
app = Flask(__name__)
app.config.from_object(Config)

# Enable CORS for dashboard origin with credentials (needed for Cloudflare Access cookie)
CORS(app, resources={r"/api/*": {"origins": "https://dashboard.bramble.ag"}}, supports_credentials=True)

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


def _link_status() -> dict:
    """Best-effort serial link status; never raises.

    Reads the existing interface without forcing a connect, so it is safe to
    call from health checks and the nodes endpoint.
    """
    down = {'connected': False, 'last_rx_age_seconds': None, 'healthy': False}
    try:
        if serial_interface is None:
            return down
        return serial_interface.link_status()
    except Exception:
        return down


@app.route('/api/health', methods=['GET'])
@app.route('/healthz', methods=['GET'])
def health():
    """Fail-loud health check.

    Returns 503 (not 200) when the serial link to the hub is down or stale, so
    a dead link surfaces instead of being masked behind stale data.
    """
    link = _link_status()
    if link['healthy']:
        status = 'healthy'
    elif link['connected']:
        status = 'degraded'
    else:
        status = 'down'
    payload = {
        'status': status,
        'serial_connected': link['connected'],
        'serial_link_up': link['healthy'],
        'last_sync_age_seconds': link['last_rx_age_seconds'],
        'serial_port': Config.SERIAL_PORT,
    }
    return jsonify(payload), (200 if link['healthy'] else 503)


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

    # Look up node_type from nodes table (node_status doesn't store it)
    all_nodes_info = {}
    try:
        with db._get_connection() as conn:
            rows = conn.execute("SELECT device_id, node_type FROM nodes").fetchall()
            for row in rows:
                all_nodes_info[row[0]] = row[1]
    except Exception:
        pass

    for device_id, status in all_status.items():
        updated_at = status.get('updated_at', 0)
        last_seen_seconds = now - updated_at if updated_at else 0
        online = last_seen_seconds < 300  # Match hub's 5-minute maintenance interval

        node = Node(
            device_id=device_id,
            address=status.get('address', 0),
            node_type=all_nodes_info.get(device_id, 'SENSOR'),
            online=online,
            last_seen_seconds=last_seen_seconds,
            firmware_version=None  # Only available from hub memory
        )
        node_dict = node.to_dict()
        if device_id in all_metadata:
            node_dict['metadata'] = all_metadata[device_id]
        node_dict['status'] = status
        nodes.append(node_dict)

    link = _link_status()
    return jsonify({
        'count': len(nodes),
        'nodes': nodes,
        'source': 'database',
        'link_up': link['healthy'],
        'last_sync_age_seconds': link['last_rx_age_seconds'],
    })


def _resolve_node_address(device_id: int) -> Optional[int]:
    """Resolve a node's network address from its device_id.

    Queries the hub first (live data), falls back to the database.
    Returns the address or None if the node cannot be found.
    """
    try:
        serial = get_serial()
        responses = serial.send_command('LIST_NODES', timeout=2.0)
        if responses and responses[0].startswith('NODE_LIST'):
            for line in responses[1:]:
                if line.startswith('NODE '):
                    parts = line.split()
                    if len(parts) >= 6 and int(parts[2]) == device_id:
                        return int(parts[1])
    except Exception as e:
        logger.warning(f"Hub lookup failed for device_id {device_id}: {e}")

    try:
        db = get_database()
        node_info = db.get_node_by_device_id(device_id)
        if node_info and node_info.get('address'):
            return node_info['address']
    except Exception as e:
        logger.warning(f"Database lookup failed for device_id {device_id}: {e}")

    return None


# Legacy fallback when a node's valve count is unknown (old firmware that does not
# report it). Matches the dashboard's fallback so validation and UI agree.
LEGACY_VALVE_COUNT = 2


def _node_valve_count(device_id: int) -> int:
    """Resolve how many valves a node has (for valve-id range validation).

    Queries the hub LIST_NODES first (live), falls back to the database, and
    finally to LEGACY_VALVE_COUNT when the count is unknown.
    """
    try:
        serial = get_serial()
        responses = serial.send_command('LIST_NODES', timeout=2.0)
        if responses and responses[0].startswith('NODE_LIST'):
            for line in responses[1:]:
                if line.startswith('NODE '):
                    parts = line.split()
                    if len(parts) >= 8 and int(parts[2]) == device_id:
                        count = int(parts[7])
                        if count > 0:
                            return count
    except Exception as e:
        logger.warning(f"Hub valve-count lookup failed for device_id {device_id}: {e}")

    try:
        node_info = get_database().get_node_by_device_id(device_id)
        if node_info and node_info.get('valve_count'):
            return node_info['valve_count']
    except Exception as e:
        logger.warning(f"DB valve-count lookup failed for device_id {device_id}: {e}")

    return LEGACY_VALVE_COUNT


# --- Valve group (master valve) helpers ---

def _gather_member_schedules(db, members, override=None):
    """Collect zone schedules for all group members, filtered to each member's
    valve.

    `override` simulates a not-yet-persisted change for overflow pre-checks:
    {'device_id': int, 'index': int, 'schedule': dict|None} where schedule None
    means a deletion. A schedule is uniquely (device_id, index).
    """
    member_valves = {}
    for member in members:
        member_valves.setdefault(int(member['zone_device_id']), set()).add(
            member['zone_valve'])
    result = []
    for device_id, valves in member_valves.items():
        rows = {s['index']: s for s in db.get_schedules(device_id)}
        if override and override['device_id'] == device_id:
            if override['schedule'] is None:
                rows.pop(override['index'], None)
            else:
                rows[override['index']] = override['schedule']
        result.extend(s for s in rows.values() if s['valve'] in valves)
    return result


def _apply_master_diff(db, group, force=False):
    """Recompute a group's master node schedules and queue the minimal diff.

    Mirrors each zone window onto the master node. With force=True every owned
    slot is re-sent regardless of the diff (used by /resync to recover from a
    TTL-expired command). Returns a summary dict. Raises MasterSlotOverflow.
    """
    from command_queue import (queue_set_schedule, queue_remove_schedule,
                               COMMAND_TTL_DEFAULTS)
    master_device_id = int(group['master_device_id'])
    master_valve = group['master_valve']
    master_address = _resolve_node_address(master_device_id)

    member_schedules = _gather_member_schedules(db, group['members'])
    desired = compute_master_windows(member_schedules, master_valve)
    stored = db.list_master_slots(master_device_id)
    diff = diff_master_slots(desired, stored, group['id'])
    if force:
        diff['to_set'] = list(diff['slots'])

    if master_address is None:
        # Persist intent so a later /resync (once the master is reachable) syncs.
        db.replace_master_slots(group['id'], master_device_id, diff['slots'])
        logger.warning(f"Master device {master_device_id} address unknown; "
                       f"stored slots but skipped queueing")
        return {'set': 0, 'removed': 0, 'master_unreachable': True}

    for slot in diff['to_set']:
        cmd_id = db.insert_command(
            device_id=master_device_id, command_type='schedule_set',
            params={'index': slot['master_index'], 'hour': slot['hour'],
                    'minute': slot['minute'], 'duration': slot['duration'],
                    'days': slot['days'], 'valve': master_valve,
                    'mirrored_from_group': group['id']},
            ttl_seconds=COMMAND_TTL_DEFAULTS['schedule_set'])
        result = queue_set_schedule(
            node_address=master_address, index=slot['master_index'],
            hour=slot['hour'], minute=slot['minute'], duration=slot['duration'],
            days=slot['days'], valve=master_valve)
        if cmd_id is not None:
            db.set_command_huey_task(cmd_id, result.id)

    for index in diff['to_remove']:
        cmd_id = db.insert_command(
            device_id=master_device_id, command_type='schedule_remove',
            params={'index': index, 'mirrored_from_group': group['id']},
            ttl_seconds=COMMAND_TTL_DEFAULTS['schedule_remove'])
        result = queue_remove_schedule(node_address=master_address, index=index)
        if cmd_id is not None:
            db.set_command_huey_task(cmd_id, result.id)

    db.replace_master_slots(group['id'], master_device_id, diff['slots'])
    return {'set': len(diff['to_set']), 'removed': len(diff['to_remove']),
            'master_unreachable': False}


def _teardown_master_slots(db, group):
    """Queue REMOVE_SCHEDULE for every master slot the API owns for this group
    and clear them. Used before re-homing or deleting a group."""
    from command_queue import queue_remove_schedule, COMMAND_TTL_DEFAULTS
    master_device_id = int(group['master_device_id'])
    master_address = _resolve_node_address(master_device_id)
    owned = [s for s in db.list_master_slots(master_device_id)
             if s['group_id'] == group['id']]
    if master_address is not None:
        for slot in owned:
            cmd_id = db.insert_command(
                device_id=master_device_id, command_type='schedule_remove',
                params={'index': slot['master_index'],
                        'mirrored_from_group': group['id']},
                ttl_seconds=COMMAND_TTL_DEFAULTS['schedule_remove'])
            result = queue_remove_schedule(node_address=master_address,
                                           index=slot['master_index'])
            if cmd_id is not None:
                db.set_command_huey_task(cmd_id, result.id)
    db.replace_master_slots(group['id'], master_device_id, [])


def _queue_master_actuator(db, group, command, duration_seconds=0):
    """Mirror a manual valve run/stop onto the group's master valve."""
    from command_queue import queue_send_actuator, COMMAND_TTL_DEFAULTS
    master_device_id = int(group['master_device_id'])
    master_valve = group['master_valve']
    master_address = _resolve_node_address(master_device_id)
    if master_address is None:
        logger.warning(f"Master device {master_device_id} address unknown; "
                       f"skipping mirror actuator")
        return
    cmd_type = 'valve_open' if command == 1 else 'valve_close'
    params = {'valve': master_valve, 'mirrored_from_group': group['id']}
    if duration_seconds:
        params['duration_seconds'] = duration_seconds
    cmd_id = db.insert_command(device_id=master_device_id, command_type=cmd_type,
                               params=params,
                               ttl_seconds=COMMAND_TTL_DEFAULTS[cmd_type])
    result = queue_send_actuator(node_address=master_address, actuator_type=1,
                                 command=command, param=master_valve,
                                 duration_seconds=duration_seconds)
    if cmd_id is not None:
        db.set_command_huey_task(cmd_id, result.id)


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
                    # firmware_version and valve_count are optional (backwards compatibility)
                    firmware_version_raw = int(parts[6]) if len(parts) >= 7 else None
                    valve_count = int(parts[7]) if len(parts) >= 8 else None
                    node = Node(
                        device_id=device_id,
                        address=address,
                        node_type=parts[3],
                        online=parts[4] == '1',
                        last_seen_seconds=int(parts[5]),
                        firmware_version=format_firmware_version(firmware_version_raw) if firmware_version_raw else None,
                        valve_count=valve_count
                    )
                    # Persist a known valve_count so the DB-fallback path can report it
                    if valve_count is not None:
                        try:
                            get_database().set_node_valve_count(device_id, valve_count)
                        except Exception as e:
                            logger.warning(f"Could not persist valve_count for {device_id}: {e}")
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

        link = _link_status()
        return jsonify({
            'count': count,
            'nodes': nodes,
            'source': 'hub',
            'link_up': link['healthy'],
            'last_sync_age_seconds': link['last_rx_age_seconds'],
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
        # Format: NODE <addr> <device_id> <type> <online> <last_seen_sec> [<firmware_version>] [<valve_count>]
        if responses and responses[0].startswith('NODE_LIST'):
            for line in responses[1:]:
                if line.startswith('NODE '):
                    parts = line.split()
                    if len(parts) >= 6 and int(parts[2]) == device_id:
                        address = int(parts[1])
                        firmware_version_raw = int(parts[6]) if len(parts) >= 7 else None
                        valve_count = int(parts[7]) if len(parts) >= 8 else None
                        node = Node(
                            device_id=device_id,
                            address=address,
                            node_type=parts[3],
                            online=parts[4] == '1',
                            last_seen_seconds=int(parts[5]),
                            firmware_version=format_firmware_version(firmware_version_raw) if firmware_version_raw else None,
                            valve_count=valve_count
                        )
                        if valve_count is not None:
                            try:
                                get_database().set_node_valve_count(device_id, valve_count)
                            except Exception as e:
                                logger.warning(f"Could not persist valve_count for {device_id}: {e}")
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

        # Look up node_type and valve_count from nodes table
        node_info = db.get_node_by_device_id(device_id)
        node_type = node_info.get('node_type', 'SENSOR') if node_info else 'SENSOR'
        valve_count = node_info.get('valve_count') if node_info else None

        node = Node(
            device_id=device_id,
            address=status.get('address', 0),
            node_type=node_type,
            online=last_seen_seconds < 300,
            last_seen_seconds=last_seen_seconds,
            firmware_version=None,
            valve_count=valve_count
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
        address = _resolve_node_address(device_id)
        if address is None:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404
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
        address = _resolve_node_address(device_id)
        if address is None:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        db = get_database()

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

        # If this zone valve belongs to a master-valve group, confirm the
        # resulting master schedule set still fits BEFORE persisting anything
        # (atomic: a slot overflow rejects the request, nothing is stored).
        group = db.get_group_for_zone_valve(device_id, data['valve'])
        if group:
            prospective = _gather_member_schedules(
                db, group['members'],
                override={'device_id': device_id, 'index': data['index'],
                          'schedule': {'index': data['index'], 'hour': data['hour'],
                                       'minute': data['minute'],
                                       'duration': data['duration'],
                                       'days': data['days'], 'valve': data['valve']}})
            try:
                compute_master_windows(prospective, group['master_valve'])
            except MasterSlotOverflow as e:
                return jsonify({'error': f'Master valve schedule full: {e}'}), 409

        # Persist schedule locally so GET /schedules returns intended state
        db.store_schedule(
            device_id=device_id,
            index=data['index'],
            hour=data['hour'],
            minute=data['minute'],
            duration=data['duration'],
            days=data['days'],
            valve=data['valve']
        )

        # Record in the node_commands audit log so the dashboard's Recent
        # Activity shows a pending row immediately (mirrors valve_open flow).
        from command_queue import queue_set_schedule, COMMAND_TTL_DEFAULTS

        command_id = db.insert_command(
            device_id=device_id,
            command_type='schedule_set',
            params={
                'index': data['index'],
                'hour': data['hour'],
                'minute': data['minute'],
                'duration': data['duration'],
                'days': data['days'],
                'valve': data['valve'],
            },
            ttl_seconds=COMMAND_TTL_DEFAULTS['schedule_set'],
        )

        result = queue_set_schedule(
            node_address=address,
            index=data['index'],
            hour=data['hour'],
            minute=data['minute'],
            duration=data['duration'],
            days=data['days'],
            valve=data['valve']
        )

        if command_id is not None:
            db.set_command_huey_task(command_id, result.id)

        # Mirror the resulting window set onto the group's master valve.
        if group:
            _apply_master_diff(db, group)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'command_id': command_id,
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
        if not 0 <= index < MAX_SCHEDULE_SLOTS:
            return jsonify({'error': f'Schedule index must be 0-{MAX_SCHEDULE_SLOTS - 1}'}), 400

        address = _resolve_node_address(device_id)
        if address is None:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        db = get_database()

        # Note which valve this schedule controlled so we can recompute the
        # master mirror after deletion (removal can only shrink the master set).
        existing = next((s for s in db.get_schedules(device_id)
                         if s['index'] == index), None)

        # Remove from local schedule storage
        db.delete_schedule(device_id=device_id, index=index)

        # Record in the node_commands audit log so the dashboard's Recent
        # Activity shows a pending row immediately (mirrors valve_open flow).
        from command_queue import queue_remove_schedule, COMMAND_TTL_DEFAULTS

        command_id = db.insert_command(
            device_id=device_id,
            command_type='schedule_remove',
            params={'index': index},
            ttl_seconds=COMMAND_TTL_DEFAULTS['schedule_remove'],
        )

        result = queue_remove_schedule(node_address=address, index=index)

        if command_id is not None:
            db.set_command_huey_task(command_id, result.id)

        # Recompute the master mirror if this valve was in a group.
        if existing:
            group = db.get_group_for_zone_valve(device_id, existing['valve'])
            if group:
                _apply_master_diff(db, group)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'command_id': command_id,
            'device_id': str(device_id),  # String to preserve JS precision
            'address': address,
            'schedule_index': index,
            'message': 'Command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error removing schedule {index} for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/schedules', methods=['GET'])
def get_schedules(device_id: int):
    """Get all irrigation schedules for a node.

    Returns the intended schedule state from local storage. Schedules are
    queued asynchronously for delivery, so this reflects what the user has
    configured, not necessarily what the node has received.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Returns:
        JSON array of schedule entries
    """
    try:
        db = get_database()
        schedules = db.get_schedules(device_id)
        return jsonify({
            'device_id': str(device_id),
            'count': len(schedules),
            'schedules': schedules,
        })
    except Exception as e:
        logger.error(f"Error getting schedules for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/valve', methods=['POST'])
@require_token
def run_valve(device_id: int):
    """Run a valve for a specified duration (run-once).

    Queues a one-shot PMU schedule entry that will open the valve at the
    next wake and auto-close after the specified duration.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Request body:
        {
            "valve": 0,
            "duration_seconds": 900
        }

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        address = _resolve_node_address(device_id)
        if address is None:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        valve = data.get('valve')
        duration_seconds = data.get('duration_seconds')

        if valve is None or duration_seconds is None:
            return jsonify({'error': 'Missing required fields: valve, duration_seconds'}), 400

        valve_count = _node_valve_count(device_id)
        if not isinstance(valve, int) or not 0 <= valve < valve_count:
            return jsonify({'error': f'valve must be 0..{valve_count - 1}'}), 400

        if not 1 <= duration_seconds <= 7200:
            return jsonify({'error': 'duration_seconds must be 1-7200'}), 400

        # Record the command in the audit log BEFORE queueing so the dashboard
        # can surface "pending" state immediately on poll.
        from command_queue import queue_send_actuator, COMMAND_TTL_DEFAULTS
        db = get_database()
        command_id = db.insert_command(
            device_id=device_id,
            command_type='valve_open',
            params={'valve': valve, 'duration_seconds': duration_seconds},
            ttl_seconds=COMMAND_TTL_DEFAULTS['valve_open'],
        )

        # Send actuator ON command with duration — firmware sets RTC Alarm A
        # for auto-close after the specified duration (node sleeps in between)
        actuator_type = 1  # ACTUATOR_VALVE
        command = 1  # CMD_TURN_ON
        result = queue_send_actuator(
            node_address=address,
            actuator_type=actuator_type,
            command=command,
            param=valve,
            duration_seconds=duration_seconds,
        )

        if command_id is not None:
            db.set_command_huey_task(command_id, result.id)

        # Mirror the run onto the group's master valve (same duration so both
        # auto-close together).
        group = db.get_group_for_zone_valve(device_id, valve)
        if group:
            _queue_master_actuator(db, group, command=1,
                                   duration_seconds=duration_seconds)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'command_id': command_id,
            'device_id': str(device_id),
            'address': address,
            'valve': valve,
            'duration_seconds': duration_seconds,
            'message': f'Valve {valve} run-once command queued ({duration_seconds}s)'
        }), 202

    except Exception as e:
        logger.error(f"Error running valve for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/valve/stop', methods=['POST'])
@require_token
def stop_valve(device_id: int):
    """Stop a running valve immediately.

    Args:
        device_id: Device ID (64-bit hardware unique ID)

    Request body:
        { "valve": 0 }

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    try:
        address = _resolve_node_address(device_id)
        if address is None:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        valve = data.get('valve')
        valve_count = _node_valve_count(device_id)
        if not isinstance(valve, int) or not 0 <= valve < valve_count:
            return jsonify({'error': f'valve must be 0..{valve_count - 1}'}), 400

        from command_queue import queue_send_actuator, COMMAND_TTL_DEFAULTS
        db = get_database()
        command_id = db.insert_command(
            device_id=device_id,
            command_type='valve_close',
            params={'valve': valve},
            ttl_seconds=COMMAND_TTL_DEFAULTS['valve_close'],
        )

        actuator_type = 1  # ACTUATOR_VALVE
        command = 0  # CMD_TURN_OFF
        result = queue_send_actuator(
            node_address=address,
            actuator_type=actuator_type,
            command=command,
            param=valve,
        )

        if command_id is not None:
            db.set_command_huey_task(command_id, result.id)

        # Mirror the stop onto the group's master valve.
        group = db.get_group_for_zone_valve(device_id, valve)
        if group:
            _queue_master_actuator(db, group, command=0)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'command_id': command_id,
            'device_id': str(device_id),
            'address': address,
            'valve': valve,
            'message': f'Valve {valve} stop command queued'
        }), 202

    except Exception as e:
        logger.error(f"Error stopping valve for node {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


def _validate_group_fields(db, master_device_id, master_valve, members):
    """Return an error string if master/members are invalid, else None."""
    master_count = _node_valve_count(master_device_id)
    if not isinstance(master_valve, int) or not 0 <= master_valve < master_count:
        return f'master_valve must be 0..{master_count - 1}'
    seen = set()
    for member in members:
        try:
            zone_device_id = int(member['zone_device_id'])
            zone_valve = int(member['zone_valve'])
        except (KeyError, TypeError, ValueError):
            return 'each member needs zone_device_id and zone_valve'
        if (zone_device_id, zone_valve) == (master_device_id, master_valve):
            return 'the master valve cannot also be a zone member'
        if (zone_device_id, zone_valve) in seen:
            return 'duplicate member'
        seen.add((zone_device_id, zone_valve))
        zone_count = _node_valve_count(zone_device_id)
        if not 0 <= zone_valve < zone_count:
            return (f'zone_valve {zone_valve} out of range for device '
                    f'{zone_device_id} (0..{zone_count - 1})')
    return None


def _check_group_uniqueness(db, master_device_id, master_valve, members,
                            exclude_group_id=None):
    """Return a (message, status) tuple if a uniqueness rule is violated, else None.

    A zone valve may belong to at most one group; a master valve backs at most
    one group.
    """
    for group in db.get_all_valve_groups():
        if group['id'] == exclude_group_id:
            continue
        if (int(group['master_device_id']) == master_device_id
                and group['master_valve'] == master_valve):
            return (f"master valve {master_valve} on device {master_device_id} "
                    f"already backs group '{group['name']}'", 409)
    for member in members:
        existing = db.get_group_for_zone_valve(int(member['zone_device_id']),
                                               int(member['zone_valve']))
        if existing and existing['id'] != exclude_group_id:
            return (f"zone valve {member['zone_valve']} on device "
                    f"{member['zone_device_id']} is already in group "
                    f"'{existing['name']}'", 409)
    return None


@app.route('/api/valve-groups', methods=['GET'])
def list_valve_groups():
    """List all valve groups."""
    try:
        db = get_database()
        groups = db.get_all_valve_groups()
        return jsonify({'count': len(groups), 'groups': groups})
    except Exception as e:
        logger.error(f"Error listing valve groups: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/valve-groups', methods=['POST'])
def create_valve_group():
    """Create a valve group and mirror members' existing schedules to the master.

    Request body:
        {
            "name": "Front beds",
            "master_device_id": "123...",
            "master_valve": 3,
            "members": [{"zone_device_id": "456...", "zone_valve": 0}, ...]
        }
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400
        name = data.get('name')
        if not name:
            return jsonify({'error': 'name is required'}), 400
        if data.get('master_device_id') is None or data.get('master_valve') is None:
            return jsonify({'error': 'master_device_id and master_valve are required'}), 400
        master_device_id = int(data['master_device_id'])
        master_valve = int(data['master_valve'])
        members = data.get('members', []) or []

        db = get_database()
        err = _validate_group_fields(db, master_device_id, master_valve, members)
        if err:
            return jsonify({'error': err}), 400
        conflict = _check_group_uniqueness(db, master_device_id, master_valve, members)
        if conflict:
            return jsonify({'error': conflict[0]}), conflict[1]

        group = db.create_valve_group(name, master_device_id, master_valve, members)

        # Backfill: mirror members' existing schedules onto the master valve.
        try:
            _apply_master_diff(db, group)
        except MasterSlotOverflow as e:
            db.delete_valve_group(group['id'])
            return jsonify({'error': f'Master valve schedule full: {e}'}), 409

        return jsonify(group), 201
    except Exception as e:
        logger.error(f"Error creating valve group: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/valve-groups/<int:group_id>', methods=['GET'])
def get_valve_group(group_id: int):
    """Get a valve group by ID."""
    try:
        db = get_database()
        group = db.get_valve_group(group_id)
        if group:
            return jsonify(group)
        return jsonify({'error': f'Valve group {group_id} not found'}), 404
    except Exception as e:
        logger.error(f"Error getting valve group {group_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/valve-groups/<int:group_id>', methods=['PUT'])
def update_valve_group(group_id: int):
    """Update a valve group's name, master valve, or membership."""
    try:
        db = get_database()
        old = db.get_valve_group(group_id)
        if not old:
            return jsonify({'error': f'Valve group {group_id} not found'}), 404

        data = request.get_json()
        if not data:
            return jsonify({'error': 'Request body must be JSON'}), 400

        name = data.get('name')
        master_device_id = (int(data['master_device_id'])
                            if data.get('master_device_id') is not None
                            else int(old['master_device_id']))
        master_valve = (int(data['master_valve'])
                        if data.get('master_valve') is not None
                        else old['master_valve'])
        members = data.get('members')
        effective_members = members if members is not None else old['members']

        err = _validate_group_fields(db, master_device_id, master_valve,
                                     effective_members)
        if err:
            return jsonify({'error': err}), 400
        conflict = _check_group_uniqueness(db, master_device_id, master_valve,
                                           effective_members,
                                           exclude_group_id=group_id)
        if conflict:
            return jsonify({'error': conflict[0]}), conflict[1]

        # If the master valve/node moved, tear down the old master's slots first.
        master_moved = (int(old['master_device_id']) != master_device_id
                        or old['master_valve'] != master_valve)
        if master_moved:
            _teardown_master_slots(db, old)

        updated = db.update_valve_group(
            group_id, name=name, master_device_id=master_device_id,
            master_valve=master_valve, members=members)

        try:
            _apply_master_diff(db, updated)
        except MasterSlotOverflow as e:
            return jsonify({'error': f'Master valve schedule full: {e}'}), 409

        return jsonify(updated)
    except Exception as e:
        logger.error(f"Error updating valve group {group_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/valve-groups/<int:group_id>', methods=['DELETE'])
def delete_valve_group(group_id: int):
    """Delete a valve group; removes its mirrored master schedules."""
    try:
        db = get_database()
        group = db.get_valve_group(group_id)
        if not group:
            return jsonify({'error': f'Valve group {group_id} not found'}), 404
        _teardown_master_slots(db, group)
        db.delete_valve_group(group_id)
        return jsonify({'status': 'deleted', 'id': group_id})
    except Exception as e:
        logger.error(f"Error deleting valve group {group_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/valve-groups/<int:group_id>/resync', methods=['POST'])
def resync_valve_group(group_id: int):
    """Force re-push of the group's master schedules (recover from TTL expiry)."""
    try:
        db = get_database()
        group = db.get_valve_group(group_id)
        if not group:
            return jsonify({'error': f'Valve group {group_id} not found'}), 404
        try:
            summary = _apply_master_diff(db, group, force=True)
        except MasterSlotOverflow as e:
            return jsonify({'error': f'Master valve schedule full: {e}'}), 409
        return jsonify({'status': 'resynced', 'id': group_id, **summary})
    except Exception as e:
        logger.error(f"Error resyncing valve group {group_id}: {e}")
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
        address = _resolve_node_address(device_id)
        if address is None:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

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
        from command_queue import queue_set_wake_interval, COMMAND_TTL_DEFAULTS
        db = get_database()
        command_id = db.insert_command(
            device_id=device_id,
            command_type='wake_interval',
            params={'interval_seconds': interval},
            ttl_seconds=COMMAND_TTL_DEFAULTS['wake_interval'],
        )

        result = queue_set_wake_interval(node_address=address, interval_seconds=interval)

        if command_id is not None:
            db.set_command_huey_task(command_id, result.id)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'command_id': command_id,
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
        address = _resolve_node_address(device_id)
        if address is None:
            return jsonify({'error': f'Node with device_id {device_id} not found'}), 404

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


@app.route('/api/nodes/<int:address>/curtain', methods=['POST'])
def control_curtain(address: int):
    """Control a greenhouse curtain motor.

    Body JSON:
        action: "open" | "close" | "stop"

    Args:
        address: Node LoRa address

    Returns:
        JSON response with task_id for tracking (202 Accepted)
    """
    data = request.get_json()
    if not data or 'action' not in data:
        return jsonify({'error': 'Missing action field'}), 400

    action = data['action']
    action_map = {
        'open': 1,       # CMD_TURN_ON
        'close': 0,      # CMD_TURN_OFF
        'stop': 4,       # CMD_STOP
        'calibrate': 5,  # CMD_CALIBRATE
    }

    if action not in action_map:
        return jsonify({'error': f'Invalid action: {action}. Must be open, close, stop, or calibrate'}), 400

    try:
        from command_queue import queue_send_actuator, COMMAND_TTL_DEFAULTS

        # Reverse-lookup device_id from address so we can write a row to the
        # node_commands audit log. If lookup fails, skip the insert and warn
        # — don't fail the POST itself.
        db = get_database()
        command_id = None
        try:
            with db._get_connection() as conn:
                row = conn.execute(
                    "SELECT device_id FROM nodes WHERE address = ?",
                    (address,),
                ).fetchone()
            if row:
                command_id = db.insert_command(
                    device_id=int(row[0]),
                    command_type='curtain',
                    params={'action': action},
                    ttl_seconds=COMMAND_TTL_DEFAULTS['curtain'],
                )
            else:
                logger.warning(
                    f"Curtain command: no device_id for address {address}, "
                    "skipping audit-log insert"
                )
        except Exception as e:
            logger.warning(f"Curtain command audit-log insert failed: {e}")

        actuator_type = 4  # ACTUATOR_CURTAIN
        command = action_map[action]
        result = queue_send_actuator(
            node_address=address,
            actuator_type=actuator_type,
            command=command,
        )

        if command_id is not None:
            db.set_command_huey_task(command_id, result.id)

        return jsonify({
            'status': 'queued',
            'task_id': result.id,
            'command_id': command_id,
            'node_address': address,
            'action': action,
            'message': f'Curtain {action} command queued for delivery'
        }), 202

    except Exception as e:
        logger.error(f"Error queueing curtain command for node {address}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/events', methods=['GET'])
def get_node_events(device_id: int):
    """Query events for a node.

    Query parameters:
        start_time: Start timestamp (optional)
        end_time: End timestamp (optional)
        limit: Maximum records (default 100, max 1000)

    Args:
        device_id: Device ID

    Returns:
        JSON list of events
    """
    try:
        db = get_database()

        start_time = request.args.get('start_time', type=int)
        end_time = request.args.get('end_time', type=int)
        limit = request.args.get('limit', default=100, type=int)

        if limit < 1 or limit > 1000:
            return jsonify({'error': 'limit must be 1-1000'}), 400

        events = db.query_events(
            device_id=device_id,
            start_time=start_time,
            end_time=end_time,
            limit=limit,
        )

        return jsonify({
            'device_id': str(device_id),
            'count': len(events),
            'events': events,
        })

    except Exception as e:
        logger.error(f"Error querying events for device {device_id}: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/nodes/<int:device_id>/commands', methods=['GET'])
def get_node_commands(device_id: int):
    """Query the ad-hoc command audit log for a node.

    Query parameters:
        status: CSV of statuses to include (pending,confirmed,failed,expired,cancelled)
        since: Earliest created_at to include (unix seconds). Defaults to now-86400.
        limit: Maximum records (default 100, max 200)

    Returns:
        JSON object {device_id, count, commands: [...]}
    """
    try:
        db = get_database()

        # Sweep expired pending rows opportunistically so callers see fresh state
        # without needing a background thread. Cheap indexed UPDATE.
        try:
            db.expire_stale_commands(int(time.time()))
        except Exception as e:
            logger.warning(f"Expire sweep failed: {e}")

        status_csv = request.args.get('status')
        status_list = (
            [s.strip() for s in status_csv.split(',') if s.strip()]
            if status_csv
            else None
        )
        since = request.args.get('since', type=int)
        if since is None:
            since = int(time.time()) - 86400
        limit = request.args.get('limit', default=100, type=int)
        if limit < 1 or limit > 200:
            return jsonify({'error': 'limit must be 1-200'}), 400

        commands = db.query_commands(
            device_id=device_id,
            status=status_list,
            since=since,
            limit=limit,
        )

        return jsonify({
            'device_id': str(device_id),
            'count': len(commands),
            'commands': commands,
        })

    except Exception as e:
        logger.error(f"Error querying commands for device {device_id}: {e}")
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


@app.route('/api/internal/hub-command', methods=['POST'])
def internal_hub_command():
    """Internal endpoint for worker to send hub commands via the API's serial connection.

    This avoids the worker opening its own serial port (which causes dual-reader
    byte stealing). Only intended for use by the huey worker container.

    Request body:
        {
            "command": "SET_SCHEDULE 1 0 14 30 900 127 0",
            "command_id": "schedule-1-0"
        }

    Returns:
        JSON with status and response lines
    """
    try:
        data = request.get_json()
        if not data or 'command' not in data:
            return jsonify({'error': 'command is required'}), 400

        command = data['command']
        command_id = data.get('command_id', 'unknown')

        logger.info(f"[{command_id}] Internal hub command: {command}")

        serial = get_serial()
        responses = serial.send_command(command)

        return jsonify({
            'status': 'success',
            'responses': responses
        })

    except TimeoutError:
        return jsonify({'error': 'Hub did not respond'}), 504
    except Exception as e:
        logger.error(f"Internal hub command failed: {e}")
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
