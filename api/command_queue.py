"""Persistent command queue using huey with SQLite backend.

Commands are sent to the hub via the API's internal endpoint, avoiding
the worker opening its own serial connection (which causes dual-reader
byte stealing).
"""
from huey import SqliteHuey
from config import Config
import logging
import requests

logger = logging.getLogger(__name__)

# Initialize huey with SQLite backend
huey = SqliteHuey(
    filename=Config.QUEUE_DB_PATH,
    immediate=Config.QUEUE_IMMEDIATE,
)

# The API service is reachable at http://api:5000 from within the Docker network
API_BASE_URL = "http://api:5000"

# Default TTLs for the node_commands audit table — how long we wait for a
# confirming event from the node before marking the command 'expired'.
# Tuned to typical wake intervals: valve commands tolerate up to 30 min of
# node sleep; curtain nodes are usually mains-powered and react fast;
# wake_interval has no confirming event today so it always expires by design.
COMMAND_TTL_DEFAULTS = {
    'valve_open': 1800,
    'valve_close': 1800,
    'curtain': 600,
    'wake_interval': 300,
    'schedule_set': 1800,
    'schedule_remove': 1800,
}


def _send_via_api(command: str, command_id: str) -> dict:
    """Send a hub command through the API's serial connection.

    Args:
        command: The serial command string
        command_id: Unique ID for tracking

    Returns:
        dict with status and response

    Raises:
        TimeoutError: If hub does not respond
        RuntimeError: On other failures
    """
    url = f"{API_BASE_URL}/api/internal/hub-command"
    try:
        resp = requests.post(url, json={
            "command": command,
            "command_id": command_id,
        }, timeout=10)

        if resp.status_code == 504:
            raise TimeoutError(f"Hub did not respond to: {command}")
        resp.raise_for_status()
        return resp.json()

    except requests.ConnectionError as e:
        raise RuntimeError(f"Cannot reach API service: {e}")
    except requests.Timeout:
        raise TimeoutError(f"API request timed out for: {command}")


@huey.task(retries=3, retry_delay=30)
def send_hub_command(command: str, command_id: str) -> dict:
    """Send a command to the hub via the API's serial interface.

    Args:
        command: The serial command string (e.g., "SET_SCHEDULE 1 0 14 30 900 127 0")
        command_id: Unique ID for tracking

    Returns:
        dict with status and response
    """
    logger.info(f"[{command_id}] Sending command: {command}")

    try:
        result = _send_via_api(command, command_id)
        responses = result.get('responses', [])

        if responses and responses[0].startswith('QUEUED'):
            logger.info(f"[{command_id}] Command queued on hub: {responses[0]}")
            return {'status': 'success', 'response': responses}
        else:
            logger.warning(f"[{command_id}] Unexpected response: {responses}")
            return {'status': 'warning', 'response': responses}

    except TimeoutError:
        logger.error(f"[{command_id}] Hub timeout - will retry")
        raise  # Raising causes huey to retry

    except Exception as e:
        logger.error(f"[{command_id}] Failed: {e}")
        raise


def _set_schedule_command(node_address: int, index: int, hour: int, minute: int,
                          duration: int, days: int, valve: int) -> tuple:
    """Build the (command, command_id) pair for a SET_SCHEDULE. Single source of
    the on-wire format, shared by the single-op and ordered-batch paths."""
    return (
        f"SET_SCHEDULE {node_address} {index} {hour}:{minute} {duration} {days} {valve}",
        f"schedule-{node_address}-{index}",
    )


def _remove_schedule_command(node_address: int, index: int) -> tuple:
    """Build the (command, command_id) pair for a REMOVE_SCHEDULE."""
    return (
        f"REMOVE_SCHEDULE {node_address} {index}",
        f"remove-schedule-{node_address}-{index}",
    )


@huey.task(retries=3, retry_delay=30)
def send_hub_commands_ordered(commands: list) -> dict:
    """Send a sequence of (command, command_id) pairs to the hub in order, one at
    a time, from a single task.

    Used for schedule replaces: a new slot can transiently overlap the old slot
    it is meant to replace, and the PMU rejects overlaps. Sending every REMOVE
    before the SETs — sequentially, in ONE task — guarantees the removes reach
    the PMU first. N independent tasks can't guarantee this: huey runs them
    across multiple worker threads, so a SET may hit the PMU before its REMOVE.

    Whole-batch retry on timeout is safe: on the PMU, re-removing an absent slot
    and re-setting an existing slot are both idempotent.
    """
    results = []
    for command, command_id in commands:
        logger.info(f"[{command_id}] Sending (ordered): {command}")
        try:
            result = _send_via_api(command, command_id)
        except TimeoutError:
            logger.error(f"[{command_id}] Hub timeout - will retry batch")
            raise
        responses = result.get('responses', [])
        if not (responses and responses[0].startswith('QUEUED')):
            logger.warning(f"[{command_id}] Unexpected response: {responses}")
        results.append({'command_id': command_id, 'response': responses})
    return {'status': 'success', 'results': results}


def queue_schedule_diff(node_address: int, removes: list, sets: list):
    """Queue an ordered schedule replace as a single task: all REMOVEs, then all
    SETs. Prevents transient-overlap rejections during a replace.

    Args:
        node_address: target node LoRa address
        removes: slot indices to remove
        sets: dicts with index, hour, minute, duration, days, valve

    Returns:
        huey TaskResultWrapper for the batch
    """
    commands = [_remove_schedule_command(node_address, i) for i in removes]
    commands += [
        _set_schedule_command(node_address, s['index'], s['hour'], s['minute'],
                              s['duration'], s['days'], s['valve'])
        for s in sets
    ]
    return send_hub_commands_ordered(commands)


def queue_set_schedule(node_address: int, index: int, hour: int, minute: int,
                       duration: int, days: int, valve: int):
    """Queue a SET_SCHEDULE command.

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command, command_id = _set_schedule_command(
        node_address, index, hour, minute, duration, days, valve)
    return send_hub_command(command, command_id)


def queue_remove_schedule(node_address: int, index: int):
    """Queue a REMOVE_SCHEDULE command.

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command, command_id = _remove_schedule_command(node_address, index)
    return send_hub_command(command, command_id)


def queue_set_wake_interval(node_address: int, interval_seconds: int):
    """Queue a SET_WAKE_INTERVAL command.

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command = f"SET_WAKE_INTERVAL {node_address} {interval_seconds}"
    command_id = f"wake-{node_address}"
    return send_hub_command(command, command_id)


def queue_set_datetime(node_address: int, year: int, month: int, day: int,
                       weekday: int, hour: int, minute: int, second: int):
    """Queue a SET_DATETIME command.

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command = f"SET_DATETIME {node_address} {year} {month} {day} {weekday} {hour} {minute} {second}"
    command_id = f"datetime-{node_address}"
    return send_hub_command(command, command_id)


def queue_reboot_node(node_address: int):
    """Queue a REBOOT_NODE command.

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command = f"REBOOT_NODE {node_address}"
    command_id = f"reboot-{node_address}"
    return send_hub_command(command, command_id)


def queue_send_actuator(node_address: int, actuator_type: int, command: int,
                        param: int = 0, duration_seconds: int = 0):
    """Queue a SEND_ACTUATOR command.

    Args:
        node_address: Target node LoRa address
        actuator_type: Actuator type (e.g. 4 = curtain)
        command: Command code (0=off, 1=on, 4=stop)
        param: Optional parameter byte
        duration_seconds: Optional duration for timed commands (e.g. valve auto-close)

    Returns:
        huey TaskResultWrapper for tracking status
    """
    if duration_seconds > 0:
        cmd = f"SEND_ACTUATOR {node_address} {actuator_type} {command} {param} {duration_seconds}"
    else:
        cmd = f"SEND_ACTUATOR {node_address} {actuator_type} {command} {param}"
    command_id = f"actuator-{node_address}-{actuator_type}-{command}"
    return send_hub_command(cmd, command_id)
