"""Persistent command queue using huey with SQLite backend."""
from huey import SqliteHuey
from config import Config
import logging

logger = logging.getLogger(__name__)

# Initialize huey with SQLite backend
huey = SqliteHuey(
    filename=Config.QUEUE_DB_PATH,
    immediate=Config.QUEUE_IMMEDIATE,
)


@huey.task(retries=3, retry_delay=30)
def send_hub_command(command: str, command_id: str) -> dict:
    """Send a command to the hub via serial.

    Args:
        command: The serial command string (e.g., "SET_SCHEDULE 1 0 14 30 900 127 0")
        command_id: Unique ID for tracking

    Returns:
        dict with status and response
    """
    # Import here to avoid circular imports
    from app import get_serial

    logger.info(f"[{command_id}] Sending command: {command}")

    try:
        serial = get_serial()
        responses = serial.send_command(command)

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


def queue_set_config(node_address: int, param_id: int, value: int):
    """Queue a SET_CONFIG command.

    Args:
        node_address: Node address
        param_id: Configuration parameter ID (ConfigParamId from firmware)
        value: Parameter value

    Returns:
        huey TaskResultWrapper for tracking status
    """
    command = f"SET_CONFIG {node_address} {param_id} {value}"
    command_id = f"config-{node_address}-{param_id}"
    return send_hub_command(command, command_id)
