"""Data models for Bramble API."""
from dataclasses import dataclass
from typing import Optional


@dataclass
class Node:
    """Represents a LoRa node in the network."""
    address: int
    node_type: str
    online: bool
    last_seen_seconds: int

    @classmethod
    def from_hub_response(cls, addr: int, node_type: str, online: str, last_seen: str):
        """Create Node from hub LIST_NODES response line."""
        return cls(
            address=addr,
            node_type=node_type,
            online=online == '1',
            last_seen_seconds=int(last_seen)
        )

    def to_dict(self):
        """Convert to dictionary for JSON serialization."""
        return {
            'address': self.address,
            'type': self.node_type,
            'online': self.online,
            'last_seen_seconds': self.last_seen_seconds
        }


@dataclass
class Schedule:
    """Represents an irrigation schedule entry."""
    index: int
    hour: int
    minute: int
    duration: int  # seconds
    days: int  # bitmask: 127 = all days
    valve: int

    def validate(self):
        """Validate schedule parameters."""
        errors = []

        if not 0 <= self.index <= 7:
            errors.append("index must be 0-7")
        if not 0 <= self.hour <= 23:
            errors.append("hour must be 0-23")
        if not 0 <= self.minute <= 59:
            errors.append("minute must be 0-59")
        if not 0 <= self.duration <= 65535:
            errors.append("duration must be 0-65535 seconds")
        if not 0 <= self.days <= 127:
            errors.append("days must be 0-127 (bitmask)")
        if self.valve < 0:
            errors.append("valve must be >= 0")

        return errors

    def to_hub_command(self, node_addr: int) -> str:
        """Format as hub SET_SCHEDULE command."""
        return f"SET_SCHEDULE {node_addr} {self.index} {self.hour}:{self.minute:02d} {self.duration} {self.days} {self.valve}"


@dataclass
class QueuedUpdate:
    """Represents a pending update in the hub queue."""
    sequence: int
    update_type: str
    age_seconds: int

    @classmethod
    def from_hub_response(cls, seq: str, update_type: str, age: str):
        """Create QueuedUpdate from hub GET_QUEUE response line."""
        return cls(
            sequence=int(seq),
            update_type=update_type,
            age_seconds=int(age)
        )

    def to_dict(self):
        """Convert to dictionary for JSON serialization."""
        return {
            'sequence': self.sequence,
            'type': self.update_type,
            'age_seconds': self.age_seconds
        }
