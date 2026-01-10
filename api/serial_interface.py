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
        """Background thread to read lines from serial port."""
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

            except Exception as e:
                logger.error(f"Error in read loop: {e}")
                time.sleep(0.1)

    def _handle_line(self, line: str):
        """Process a line received from the hub.

        Args:
            line: Complete line from hub
        """
        logger.debug(f"Hub: {line}")

        # Check if this is a query from hub
        if line == "GET_DATETIME":
            self._handle_datetime_query()
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

        # Otherwise, it's a response to our command
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
            self.serial.write(response.encode('utf-8'))
            self.serial.flush()
            logger.info(f"Responded to datetime query: {response.strip()}")
        except Exception as e:
            logger.error(f"Failed to send datetime response: {e}")

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
                self.serial.write(cmd_line.encode('utf-8'))
                self.serial.flush()
                logger.debug(f"Sent: {command}")
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

            if not responses:
                raise TimeoutError(f"No response received for command: {command}")

            return responses

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

        Format: SENSOR_DATA <node_addr> TEMP|HUM <value>
        """
        if not self.database:
            logger.warning("Received sensor data but no database configured")
            return

        try:
            parts = line.split()
            if len(parts) < 4:
                logger.error(f"Invalid SENSOR_DATA format: {line}")
                return

            node_addr = int(parts[1])
            sensor_type = parts[2]
            value = int(parts[3])
            timestamp = int(time.time())

            # For individual readings, we only have one value at a time
            # Store with placeholder for the missing value
            if sensor_type == "TEMP":
                reading = SensorReading(
                    node_address=node_addr,
                    timestamp=timestamp,
                    temperature_centidegrees=value,
                    humidity_centipercent=0,  # Will be updated if humidity follows
                    received_at=timestamp
                )
            elif sensor_type == "HUM":
                reading = SensorReading(
                    node_address=node_addr,
                    timestamp=timestamp,
                    temperature_centidegrees=0,
                    humidity_centipercent=value,
                    received_at=timestamp
                )
            else:
                logger.warning(f"Unknown sensor type: {sensor_type}")
                return

            if self.database.insert_reading(reading):
                logger.info(f"Stored sensor data: node={node_addr}, {sensor_type}={value}")
            else:
                logger.debug(f"Duplicate sensor data: node={node_addr}, {sensor_type}={value}")

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse SENSOR_DATA: {e}")

    def _handle_sensor_batch_start(self, line: str):
        """Handle start of sensor batch from hub.

        Format: SENSOR_BATCH <node_addr> <count>
        """
        try:
            parts = line.split()
            if len(parts) < 3:
                logger.error(f"Invalid SENSOR_BATCH format: {line}")
                return

            node_addr = int(parts[1])
            count = int(parts[2])

            self._batch_state = {
                'node_addr': node_addr,
                'expected_count': count,
                'records': [],
                'start_time': time.time()
            }

            logger.info(f"Starting batch receive: node={node_addr}, expected={count}")

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse SENSOR_BATCH: {e}")

    def _handle_sensor_record(self, line: str):
        """Handle individual record in a batch.

        Format: SENSOR_RECORD <node_addr> <timestamp> <temp> <humidity> <flags>
        """
        if not self._batch_state:
            logger.warning("Received SENSOR_RECORD without active batch")
            return

        try:
            parts = line.split()
            if len(parts) < 6:
                logger.error(f"Invalid SENSOR_RECORD format: {line}")
                return

            node_addr = int(parts[1])
            timestamp = int(parts[2])
            temperature = int(parts[3])
            humidity = int(parts[4])
            flags = int(parts[5])

            reading = SensorReading(
                node_address=node_addr,
                timestamp=timestamp,
                temperature_centidegrees=temperature,
                humidity_centipercent=humidity,
                flags=flags,
                received_at=int(time.time())
            )

            self._batch_state['records'].append(reading)
            logger.debug(f"Batch record: ts={timestamp}, temp={temperature}, hum={humidity}")

        except (ValueError, IndexError) as e:
            logger.error(f"Failed to parse SENSOR_RECORD: {e}")

    def _handle_batch_complete(self, line: str):
        """Handle end of sensor batch.

        Format: BATCH_COMPLETE <node_addr> <count>
        """
        if not self._batch_state:
            logger.warning("Received BATCH_COMPLETE without active batch")
            return

        try:
            parts = line.split()
            if len(parts) < 3:
                logger.error(f"Invalid BATCH_COMPLETE format: {line}")
                return

            node_addr = int(parts[1])
            count = int(parts[2])

            # Verify batch integrity
            actual_count = len(self._batch_state['records'])
            if actual_count != count:
                logger.warning(f"Batch count mismatch: expected={count}, actual={actual_count}")

            # Store batch in database
            if self.database and self._batch_state['records']:
                inserted, duplicates = self.database.insert_batch(self._batch_state['records'])
                logger.info(f"Batch stored: inserted={inserted}, duplicates={duplicates}")

                # Send acknowledgment to hub
                self._send_batch_ack(node_addr, inserted, 0 if inserted > 0 else 1)
            else:
                logger.warning("No records to store or database not configured")
                self._send_batch_ack(node_addr, 0, 1)

            # Clear batch state
            elapsed = time.time() - self._batch_state['start_time']
            logger.info(f"Batch complete: node={node_addr}, records={actual_count}, elapsed={elapsed:.2f}s")
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
            self.serial.write(response.encode('utf-8'))
            self.serial.flush()
            logger.info(f"Sent BATCH_ACK: node={node_addr}, count={count}, status={status}")
        except Exception as e:
            logger.error(f"Failed to send BATCH_ACK: {e}")
