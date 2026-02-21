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


def queue_set_schedule(node_address: int, index: int, hour: int, minute: int,
                       duration: int, days: int, valve: int):
    """Queue a SET_SCHEDULE command.

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command = f"SET_SCHEDULE {node_address} {index} {hour} {minute} {duration} {days} {valve}"
    command_id = f"schedule-{node_address}-{index}"
    return send_hub_command(command, command_id)


def queue_remove_schedule(node_address: int, index: int):
    """Queue a REMOVE_SCHEDULE command.

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command = f"REMOVE_SCHEDULE {node_address} {index}"
    command_id = f"remove-schedule-{node_address}-{index}"
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
