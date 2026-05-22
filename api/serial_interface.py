"""Serial interface for communicating with LoRa hub."""
import serial
import threading
import time
from datetime import datetime
from typing import Optional, Callable
from queue import Queue, Empty
import logging

from config import Config
from database import SensorDatabase, SensorReading
from event_types import EventType, EventCode


logger = logging.getLogger(__name__)


class SerialInterface:
    """Handles bidirectional serial communication with the LoRa hub."""

    def __init__(self, port: str = Config.SERIAL_PORT, baud: int = Config.SERIAL_BAUD,
                 database: Optional[SensorDatabase] = None):
        """Initialize serial interface.

        Args:
            port: Serial port path (e.g., /dev/ttyACM0)
            baud: Baud rate (default 115200)
            database: Optional SensorDatabase for storing sensor readings
        """
        self.port = port
        self.baud = baud
        self.serial: Optional[serial.Serial] = None
        self.running = False
        self.read_thread: Optional[threading.Thread] = None

        # Response handling
        self.response_queue = Queue()
        self.pending_command: Optional[str] = None
        self.command_lock = threading.Lock()
        self.write_lock = threading.Lock()  # Protects serial.write() from concurrent threads

        # Sensor data handling
        self.database = database
        self._batch_state: Optional[dict] = None  # Tracks in-progress batch

    def connect(self):
        """Open serial connection to hub."""
        try:
            self.serial = serial.Serial(
                self.port,
                self.baud,
                timeout=Config.SERIAL_TIMEOUT
            )
            logger.info(f"Connected to hub on {self.port} at {self.baud} baud")

            # Start read thread
            self.running = True
            self.read_thread = threading.Thread(target=self._read_loop, daemon=True)
            self.read_thread.start()

        except serial.SerialException as e:
            logger.error(f"Failed to connect to serial port: {e}")
            raise

    def disconnect(self):
        """Close serial connection."""
        self.running = False
        if self.read_thread:
            self.read_thread.join(timeout=2.0)
        if self.serial and self.serial.is_open:
            self.serial.close()
            logger.info("Disconnected from hub")

    def _read_loop(self):
        """Background thread to read lines from serial port with auto-reconnect."""
        while self.running:
            try:
                self._read_until_disconnected()
            except Exception as e:
                logger.error(f"Read loop error: {e}")

            if not self.running:
                break

            # Reconnect loop
            logger.warning(f"Serial connection lost, will retry every 5s")
            while self.running:
                try:
                    if self.serial and self.serial.is_open:
                        self.serial.close()
                    self.serial = serial.Serial(self.port, self.baud, timeout=Config.SERIAL_TIMEOUT)
                    logger.info(f"Reconnected to hub on {self.port}")
                    break
                except serial.SerialException:
                    time.sleep(5)

        logger.info("Read loop exiting")

    def _read_until_disconnected(self):
        """Read from serial until connection is lost or stopped."""
        buffer = ""

        while self.running and self.serial and self.serial.is_open:
            try:
                if self.serial.in_waiting:
                    chunk = self.serial.read(self.serial.in_waiting).decode('utf-8', errors='ignore')
                    buffer += chunk

                    # Process complete lines
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        line = line.strip()
                        if line:
                            self._handle_line(line)
                else:
                    time.sleep(0.01)  # Avoid busy loop

            except serial.SerialException as e:
                logger.warning(f"Serial disconnected: {e}")
                return
            except Exception as e:
                logger.error(f"Error reading serial: {e}")
                time.sleep(0.1)

    # Known message prefixes that should start a line
    MESSAGE_PREFIXES = [
        "HEARTBEAT ", "SENSOR_BATCH ", "SENSOR_RECORD ",
        "BATCH_COMPLETE ", "SENSOR_DATA ", "HUB_READY",
        "NODE_LIST ", "NODE ", "GET_DATETIME", "QUEUE ",
        "QUEUED ", "ERROR ", "DELETED_NODE ", "BATCH_ACK_SENT ",
        "EVENT_LOG ", "EVENT ",
    ]

    def _handle_line(self, line: str):
        """Process a line received from the hub.

        Args:
            line: Complete line from hub
        """
        logger.debug(f"Hub: {line}")

        # Check if line contains an embedded message (corruption recovery).
        # If UART corruption merges two messages on one line, split them so
        # the embedded message is still processed.
        for prefix in self.MESSAGE_PREFIXES:
            idx = line.find(prefix, 1)  # Skip pos 0 (that's the normal start)
            if idx > 0:
                logger.warning(f"Detected embedded message at pos {idx}, splitting: {line[:60]}...")
                self._handle_line(line[idx:])  # Process the embedded message first
                line = line[:idx].rstrip()
                if not line:
                    return
                break

        # Proactively send time when hub signals it's ready
        # This ensures hub gets time regardless of boot order
        if line == "HUB_READY":
            logger.info("Hub ready - proactively sending datetime")
            self._handle_datetime_query()
            return

        # Check if this is a query from hub
        if line == "GET_DATETIME":
            self._handle_datetime_query()
            return

        # Handle heartbeat messages
        if line.startswith("HEARTBEAT "):
            self._handle_heartbeat(line)
            return

        # Handle event log batch records (check before EVENT to avoid prefix collision)
        if line.startswith("EVENT_LOG "):
            self._handle_event_log(line)
            return

        # Handle event messages
        if line.startswith("EVENT "):
            self._handle_event(line)
            return

        # Handle sensor data messages
        if line.startswith("SENSOR_DATA "):
            self._handle_sensor_data(line)
            return

        if line.startswith("SENSOR_BATCH "):
            self._handle_sensor_batch_start(line)
            return

        if line.startswith("SENSOR_RECORD "):
            self._handle_sensor_record(line)
            return

        if line.startswith("BATCH_COMPLETE "):
            self._handle_batch_complete(line)
            return

        # Handle batch acknowledgment confirmations from hub
        if line.startswith("BATCH_ACK_SENT "):
            logger.debug(f"Batch ACK confirmed: {line}")
            return

        # Otherwise, it's a response to our command
        logger.info(f"Response queued: {line}")
        self.response_queue.put(line)

    def _handle_datetime_query(self):
        """Respond to hub's GET_DATETIME query with current system time."""
        now = datetime.now()
        weekday = now.weekday()  # 0=Monday in Python
        # Convert to hub format: 0=Sunday, 1=Monday, ..., 6=Saturday
        hub_weekday = (weekday + 1) % 7

        # Format: DATETIME YYYY-MM-DD HH:MM:SS DOW (ISO 8601 + day of week)
        response = f"DATETIME {now.strftime('%Y-%m-%d %H:%M:%S')} {hub_weekday}\n"

        try:
            with self.write_lock:
                self.serial.write(response.encode('utf-8'))
                self.serial.flush()
            logger.info(f"Responded to datetime query: {response.strip()}")
        except Exception as e:
            logger.error(f"Failed to send datetime response: {e}")

    def _handle_heartbeat(self, line: str):
        """Handle heartbeat status from hub.

        Format: HEARTBEAT <node_addr> <device_id> <battery> <error_flags> <signal> <uptime> [<pending_records>]
        """
        if not self.database:
            logger.warning("Received heartbeat but no database configured")
            return

        try:
            parts = line.split()
            if len(parts) < 7:
                logger.error(f"Invalid HEARTBEAT format: {line}")
                return

            node_addr = int(parts[1])
            device_id = int(parts[2])
            battery_level = int(parts[3])
            error_flags = int(parts[4])
            signal_strength = int(parts[5])
            uptime_seconds = int(parts[6])
            # pending_records is optional for backwards compatibility with older firmware
            pending_records = int(parts[7]) if len(parts) > 7 else None

            if device_id == 0:
                logger.warning(f"Heartbeat from node {node_addr} has no device_id, skipping")
                return

            # Store in database using device_id as primary key
            self.database.update_node_status(
                device_id=device_id,
                address=node_addr,
                battery_level=battery_level,
                error_flags=error_flags,
                signal_strength=signal_strength,
                uptime_seconds=uptime_seconds,
                pending_records=pending_records
            )

            pending_str = f", pending={pending_records}" if pending_records is not None else ""
            logger.info(f"Updated node status: device_id={device_id}, addr={node_addr}, battery={battery_level}, "
                       f"errors=0x{error_flags:02X}, rssi={signal_strength}dBm, uptime={uptime_seconds}s{pending_str}")

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse HEARTBEAT: {e}")

    def send_command(self, command: str, timeout: float = Config.COMMAND_TIMEOUT) -> list[str]:
        """Send command to hub and wait for response.

        Args:
            command: Command string (e.g., "LIST_NODES")
            timeout: Maximum time to wait for response

        Returns:
            List of response lines from hub

        Raises:
            TimeoutError: If no response received within timeout
            RuntimeError: If serial connection is not open
        """
        if not self.serial or not self.serial.is_open:
            raise RuntimeError("Serial connection not open")

        with self.command_lock:
            # Clear any stale responses
            while not self.response_queue.empty():
                try:
                    self.response_queue.get_nowait()
                except Empty:
                    break

            # Send command
            cmd_line = command.strip() + '\n'
            try:
                with self.write_lock:
                    self.serial.write(cmd_line.encode('utf-8'))
                    self.serial.flush()
                logger.info(f"Sent command: {command}")
            except Exception as e:
                logger.error(f"Failed to send command: {e}")
                raise RuntimeError(f"Failed to send command: {e}")

            # Collect response lines
            responses = []
            start_time = time.time()

            while time.time() - start_time < timeout:
                try:
                    line = self.response_queue.get(timeout=0.1)
                    responses.append(line)

                    # Check if this is the end of a multi-line response
                    # Most commands have a final summary line or specific format
                    if self._is_response_complete(command, responses):
                        break

                except Empty:
                    # If we already have responses and haven't seen new ones, consider complete
                    if responses and (time.time() - start_time) > 0.5:
                        break
                    continue

            if responses:
                return responses

            raise TimeoutError(f"No response received for command: {command}")

    def _is_response_complete(self, command: str, responses: list[str]) -> bool:
        """Determine if we've received a complete response.

        Args:
            command: Original command sent
            responses: Response lines received so far

        Returns:
            True if response is complete
        """
        if not responses:
            return False

        last_line = responses[-1]

        # Error responses from garbled commands should fail fast
        if last_line.startswith('ERROR'):
            return True

        # Single-line responses
        if command.startswith('SET_SCHEDULE') or command.startswith('REMOVE_SCHEDULE'):
            return last_line.startswith('QUEUED')

        if command.startswith('SET_WAKE_INTERVAL') or command.startswith('SET_DATETIME'):
            return last_line.startswith('QUEUED')

        # Multi-line responses - check for summary line
        if command == 'LIST_NODES':
            # Format: NODE_LIST <count> followed by NODE lines
            if len(responses) == 1 and responses[0].startswith('NODE_LIST'):
                count = int(responses[0].split()[1])
                return count == 0  # No nodes
            # We have header + node lines
            if responses[0].startswith('NODE_LIST'):
                expected_count = int(responses[0].split()[1])
                node_count = len([r for r in responses[1:] if r.startswith('NODE ')])
                return node_count >= expected_count

        if command.startswith('GET_QUEUE'):
            # Format: QUEUE <addr> <count> followed by UPDATE lines
            if len(responses) == 1 and responses[0].startswith('QUEUE'):
                parts = responses[0].split()
                count = int(parts[2])
                return count == 0  # No updates
            if responses[0].startswith('QUEUE'):
                expected_count = int(responses[0].split()[2])
                update_count = len([r for r in responses[1:] if r.startswith('UPDATE ')])
                return update_count >= expected_count

        # Default: wait a bit longer for more lines
        return False

    # ===== Sensor Data Handling =====

    def _handle_sensor_data(self, line: str):
        """Handle individual sensor data from hub.

        Format: SENSOR_DATA <node_addr> <device_id> TEMP|HUM <value>
        """
        if not self.database:
            logger.warning("Received sensor data but no database configured")
            return

        try:
            parts = line.split()
            if len(parts) < 5:
                logger.error(f"Invalid SENSOR_DATA format: {line}")
                return

            node_addr = int(parts[1])
            device_id = int(parts[2])
            sensor_type = parts[3]
            value = int(parts[4])
            timestamp = int(time.time())

            if device_id == 0:
                logger.warning(f"Sensor data from node {node_addr} has no device_id, skipping")
                return

            # For individual readings, we only have one value at a time
            # Store with placeholder for the missing value
            if sensor_type == "TEMP":
                reading = SensorReading(
                    device_id=device_id,
                    address=node_addr,
                    timestamp=timestamp,
                    temperature_centidegrees=value,
                    humidity_centipercent=0,  # Will be updated if humidity follows
                    received_at=timestamp
                )
            elif sensor_type == "HUM":
                reading = SensorReading(
                    device_id=device_id,
                    address=node_addr,
                    timestamp=timestamp,
                    temperature_centidegrees=0,
                    humidity_centipercent=value,
                    received_at=timestamp
                )
            else:
                logger.warning(f"Unknown sensor type: {sensor_type}")
                return

            if self.database.insert_reading(reading):
                logger.info(f"Stored sensor data: device_id={device_id}, addr={node_addr}, {sensor_type}={value}")
            else:
                logger.debug(f"Duplicate sensor data: device_id={device_id}, addr={node_addr}, {sensor_type}={value}")

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse SENSOR_DATA: {e}")

    def _handle_event(self, line: str):
        """Handle event notification from hub.

        Format: EVENT <node_addr> <device_id> <event_code> <data_hex>
        """
        if not self.database:
            logger.warning("Received event but no database configured")
            return

        try:
            parts = line.split()
            if len(parts) < 4:
                logger.error(f"Invalid EVENT format: {line}")
                return

            device_id = int(parts[2])
            event_code = int(parts[3])
            data_hex = parts[4] if len(parts) >= 5 else ""
            timestamp = int(time.time())

            if device_id == 0:
                logger.warning(f"Event from node {parts[1]} has no device_id, skipping")
                return

            if self.database.insert_event(device_id, timestamp, event_code, data_hex):
                logger.info(f"Stored event: device_id={device_id}, code=0x{event_code:04X}, data={data_hex}")
            else:
                logger.debug(f"Duplicate event: device_id={device_id}, code=0x{event_code:04X}")

            # Match pending curtain commands. EVENT_MOTOR_ERROR fails any
            # pending curtain command; the directional/calibration events
            # confirm the one whose action matches.
            self._reconcile_curtain_event(device_id, event_code, timestamp)

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse EVENT: {e}")

    def _handle_event_log(self, line: str):
        """Handle event log record from hub.

        Format: EVENT_LOG <node_addr> <device_id> <unix_ts> <event_type> <severity> <detail>
        """
        if not self.database:
            logger.warning("Received event log but no database configured")
            return

        try:
            parts = line.split()
            if len(parts) < 7:
                logger.error(f"Invalid EVENT_LOG format: {line}")
                return

            device_id = int(parts[2])
            unix_ts = int(parts[3])
            event_type = int(parts[4])
            severity = int(parts[5])
            detail = int(parts[6])

            if device_id == 0:
                logger.warning(f"Event log from node {parts[1]} has no device_id, skipping")
                return

            # Store as a node event (event_code = event_type, data_hex = severity:detail)
            data_hex = f"{severity:02X}{detail:04X}"
            self.database.insert_event(device_id, unix_ts, event_type, data_hex)

            # For schedule events the firmware packs the PMU error code into
            # the high byte of `detail` and the schedule index into the low
            # byte. APPLIED/REMOVED carry error_code=0, so detail==index for
            # success; FAILED carries the actual ErrorCode.
            schedule_index = detail & 0xFF
            pmu_error_code = (detail >> 8) & 0xFF

            # Handle schedule confirmation events
            if event_type == EventType.SCHEDULE_APPLIED:
                self.database.confirm_schedule(device_id, schedule_index, unix_ts)
                logger.info(f"Schedule confirmed: device_id={device_id}, index={schedule_index}")
            elif event_type == EventType.SCHEDULE_REMOVED:
                self.database.confirm_schedule_removed(device_id, schedule_index, unix_ts)
                logger.info(
                    f"Schedule removal confirmed: device_id={device_id}, index={schedule_index}"
                )
            elif event_type == EventType.SCHEDULE_FAILED:
                self.database.fail_schedule(device_id, schedule_index, unix_ts)
                logger.warning(
                    f"Schedule failed: device_id={device_id}, index={schedule_index}, "
                    f"pmu_error=0x{pmu_error_code:02X}"
                )
            else:
                logger.debug(
                    f"Event log: device_id={device_id}, type=0x{event_type:02X}, "
                    f"severity={severity}, detail={detail}"
                )

            # Match pending ad-hoc valve commands (still uses raw detail —
            # valve events encode the valve id directly, not packed).
            self._reconcile_valve_event(device_id, event_type, detail, unix_ts)

            # Match pending ad-hoc schedule commands. Pass the raw `detail`
            # so the failure path keeps the PMU error code in the audit row.
            self._reconcile_schedule_event(
                device_id, event_type, schedule_index, detail, unix_ts
            )

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse EVENT_LOG: {e}")

    def _reconcile_valve_event(self, device_id: int, event_type: int,
                               valve_id: int, unix_ts: int):
        """Confirm pending valve commands when matching events arrive.

        Matches oldest-pending-wins within (device_id, command_type, valve).
        Ambiguous if two opens for the same valve are queued in rapid
        succession — accepted limitation (see plan).
        """
        if event_type in (EventType.VALVE_OPEN, EventType.VALVE_TIMER_SET):
            cmd_type = 'valve_open'
        elif event_type in (EventType.VALVE_CLOSE, EventType.VALVE_TIMER_CLOSE):
            cmd_type = 'valve_close'
        else:
            return

        cmd = self.database.find_pending_command(
            device_id, cmd_type, param_filter={'valve': valve_id}
        )
        if cmd:
            self.database.update_command_status(
                cmd['id'], 'confirmed', unix_ts,
                event_code=event_type, event_detail=valve_id,
            )
            logger.info(
                f"Command confirmed: id={cmd['id']}, type={cmd_type}, "
                f"valve={valve_id}, device_id={device_id}"
            )

    def _reconcile_schedule_event(self, device_id: int, event_type: int,
                                  index: int, raw_detail: int, unix_ts: int):
        """Confirm or fail pending schedule_set / schedule_remove commands.

        `raw_detail` is the full 16-bit event detail (PMU error code in the
        high byte, schedule index in the low byte). Stored on failure so the
        dashboard can show why the PMU rejected the schedule.
        """
        if event_type == EventType.SCHEDULE_APPLIED:
            cmd_type = 'schedule_set'
            status = 'confirmed'
        elif event_type == EventType.SCHEDULE_REMOVED:
            cmd_type = 'schedule_remove'
            status = 'confirmed'
        elif event_type == EventType.SCHEDULE_FAILED:
            for candidate in ('schedule_set', 'schedule_remove'):
                cmd = self.database.find_pending_command(
                    device_id, candidate, param_filter={'index': index}
                )
                if cmd:
                    self.database.update_command_status(
                        cmd['id'], 'failed', unix_ts,
                        event_code=event_type, event_detail=raw_detail,
                    )
                    logger.info(
                        f"Command failed: id={cmd['id']}, type={candidate}, "
                        f"index={index}, pmu_error=0x{(raw_detail >> 8) & 0xFF:02X}, "
                        f"device_id={device_id}"
                    )
                    return
            return
        else:
            return

        cmd = self.database.find_pending_command(
            device_id, cmd_type, param_filter={'index': index}
        )
        if cmd:
            self.database.update_command_status(
                cmd['id'], status, unix_ts,
                event_code=event_type, event_detail=index,
            )
            logger.info(
                f"Command confirmed: id={cmd['id']}, type={cmd_type}, "
                f"index={index}, device_id={device_id}"
            )

    def _reconcile_curtain_event(self, device_id: int, event_code: int,
                                 unix_ts: int):
        """Confirm or fail pending curtain commands."""
        action_for_code = {
            int(EventCode.EVENT_CURTAIN_OPENING): 'open',
            int(EventCode.EVENT_CURTAIN_CLOSING): 'close',
            int(EventCode.EVENT_CURTAIN_STOPPED): 'stop',
            int(EventCode.EVENT_CALIBRATION_COMPLETE): 'calibrate',
        }

        if event_code == int(EventCode.EVENT_MOTOR_ERROR):
            cmd = self.database.find_pending_command(device_id, 'curtain')
            if cmd:
                self.database.update_command_status(
                    cmd['id'], 'failed', unix_ts,
                    event_code=event_code,
                )
                logger.info(
                    f"Curtain command failed (motor error): id={cmd['id']}, "
                    f"device_id={device_id}"
                )
            return

        action = action_for_code.get(event_code)
        if not action:
            return

        cmd = self.database.find_pending_command(
            device_id, 'curtain', param_filter={'action': action}
        )
        if cmd:
            self.database.update_command_status(
                cmd['id'], 'confirmed', unix_ts,
                event_code=event_code,
            )
            logger.info(
                f"Curtain command confirmed: id={cmd['id']}, action={action}, "
                f"device_id={device_id}"
            )

    def _handle_sensor_batch_start(self, line: str):
        """Handle start of sensor batch from hub.

        Format: SENSOR_BATCH <node_addr> <device_id> <count>
        """
        try:
            parts = line.split()
            if len(parts) < 4:
                logger.error(f"Invalid SENSOR_BATCH format: {line}")
                return

            node_addr = int(parts[1])
            device_id = int(parts[2])
            count = int(parts[3])

            if device_id == 0:
                logger.warning(f"Batch from node {node_addr} has no device_id, skipping")
                return

            self._batch_state = {
                'node_addr': node_addr,
                'device_id': device_id,
                'expected_count': count,
                'records': [],
                'start_time': time.time()
            }

            logger.info(f"Starting batch receive: device_id={device_id}, addr={node_addr}, expected={count}")

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse SENSOR_BATCH: {e}")

    def _handle_sensor_record(self, line: str):
        """Handle individual record in a batch.

        Format: SENSOR_RECORD <node_addr> <device_id> <timestamp> <temp> <humidity> <flags>
        """
        if not self._batch_state:
            logger.warning("Received SENSOR_RECORD without active batch")
            return

        try:
            parts = line.split()
            if len(parts) < 7:
                logger.error(f"Invalid SENSOR_RECORD format: {line}")
                return

            node_addr = int(parts[1])
            device_id = int(parts[2])
            timestamp = int(parts[3])
            temperature = int(parts[4])
            humidity = int(parts[5])
            flags = int(parts[6])

            reading = SensorReading(
                device_id=device_id,
                address=node_addr,
                timestamp=timestamp,
                temperature_centidegrees=temperature,
                humidity_centipercent=humidity,
                flags=flags,
                received_at=int(time.time())
            )

            self._batch_state['records'].append(reading)
            logger.debug(f"Batch record: device_id={device_id}, ts={timestamp}, temp={temperature}, hum={humidity}")

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse SENSOR_RECORD: {e}")

    def _handle_batch_complete(self, line: str):
        """Handle end of sensor batch.

        Format: BATCH_COMPLETE <node_addr> <device_id> <count>
        """
        if not self._batch_state:
            logger.warning("Received BATCH_COMPLETE without active batch")
            return

        try:
            parts = line.split()
            if len(parts) < 4:
                logger.error(f"Invalid BATCH_COMPLETE format: {line}")
                return

            node_addr = int(parts[1])
            device_id = int(parts[2])
            count = int(parts[3])

            # Verify batch integrity
            actual_count = len(self._batch_state['records'])
            if actual_count != count:
                logger.warning(f"Batch count mismatch: expected={count}, actual={actual_count}")

            # Store batch in database
            if self.database and self._batch_state['records']:
                inserted, duplicates = self.database.insert_batch(self._batch_state['records'])
                logger.info(f"Batch stored: inserted={inserted}, duplicates={duplicates}")

                # Send acknowledgment to hub
                # Success (status=0) if any records were processed (inserted OR duplicates)
                # Duplicates mean we already have the data, so sensor should advance read_index
                total_processed = inserted + duplicates
                status = 0 if total_processed > 0 else 1
                self._send_batch_ack(node_addr, total_processed, status)
            else:
                logger.warning("No records to store or database not configured")
                self._send_batch_ack(node_addr, 0, 1)

            # Clear batch state
            elapsed = time.time() - self._batch_state['start_time']
            logger.info(f"Batch complete: device_id={device_id}, addr={node_addr}, records={actual_count}, elapsed={elapsed:.2f}s")
            self._batch_state = None

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse BATCH_COMPLETE: {e}")
            self._batch_state = None

    def _send_batch_ack(self, node_addr: int, count: int, status: int):
        """Send batch acknowledgment to hub.

        Args:
            node_addr: Node address to acknowledge
            count: Number of records received
            status: 0 for success, non-zero for error
        """
        if not self.serial or not self.serial.is_open:
            logger.error("Cannot send BATCH_ACK: serial not open")
            return

        try:
            response = f"BATCH_ACK {node_addr} {count} {status}\n"
            with self.write_lock:
                self.serial.write(response.encode('utf-8'))
                self.serial.flush()
            logger.info(f"Sent BATCH_ACK: node={node_addr}, count={count}, status={status}")
        except Exception as e:
            logger.error(f"Failed to send BATCH_ACK: {e}")
