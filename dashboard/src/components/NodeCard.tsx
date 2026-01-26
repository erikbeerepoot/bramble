import type { Node, Zone } from '../types';
import BatteryGauge from './BatteryGauge';
import SignalStrength from './SignalStrength';
import HealthStatus from './HealthStatus';
import BacklogStatus from './BacklogStatus';

interface NodeCardProps {
  node: Node;
  zone?: Zone;
  onClick: () => void;
}

function formatLastSeen(seconds: number): string {
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
  return `${Math.floor(seconds / 86400)}d ago`;
}

function NodeCard({ node, zone, onClick }: NodeCardProps) {
  const displayName = node.metadata?.name || `Node ${node.address}`;

  return (
    <div
      onClick={onClick}
      className="card cursor-pointer hover:shadow-lg transition-shadow border border-gray-200 overflow-hidden"
      style={zone ? { borderLeftWidth: '4px', borderLeftColor: zone.color } : undefined}
    >
      <div className="flex items-start justify-between">
        <div className="flex-1 min-w-0">
          <div className="flex items-center space-x-2">
            <h3 className="text-lg font-medium text-gray-900 truncate">
              {displayName}
            </h3>
            <span className={`inline-flex items-center px-2 py-0.5 rounded text-xs font-medium ${
              node.online
                ? 'bg-green-100 text-green-800'
                : 'bg-red-100 text-red-800'
            }`}>
              {node.online ? 'Online' : 'Offline'}
            </span>
          </div>

          {node.metadata?.name && (
            <p className="text-sm text-gray-500">Address: {node.address}</p>
          )}

          {node.metadata?.location && (
            <p className="text-sm text-gray-500 mt-1 truncate">
              {node.metadata.location}
            </p>
          )}
        </div>

        <div className={`flex-shrink-0 ml-4 w-3 h-3 rounded-full ${
          node.online ? 'bg-green-500' : 'bg-red-500'
        }`} />
      </div>

      {/* Status row - battery, signal, health, backlog */}
      {node.status && (
        <div className="mt-3 pt-3 border-t border-gray-100 flex items-center space-x-4">
          <BatteryGauge level={node.status.battery_level} size="sm" />
          <SignalStrength rssi={node.status.signal_strength} size="sm" />
          <HealthStatus errorFlags={node.status.error_flags} size="sm" />
          <BacklogStatus
            pendingRecords={node.status.pending_records}
            hubQueueCount={node.hub_queue_count}
            size="sm"
          />
        </div>
      )}

      <div className="mt-3 flex items-center justify-between text-sm">
        <div className="text-gray-500">
          <span className="font-medium">{node.type}</span>
        </div>
        <div className="text-gray-400">
          Last seen: {formatLastSeen(node.last_seen_seconds)}
        </div>
      </div>

      <div className="mt-2 flex items-center justify-between text-xs text-gray-400 font-mono">
        {node.device_id && (
          <span className="truncate">
            ID: {node.device_id.toString(16).toUpperCase()}
          </span>
        )}
        {node.firmware_version && (
          <span className="ml-2 flex-shrink-0">
            v{node.firmware_version}
          </span>
        )}
      </div>
    </div>
  );
}

export default NodeCard;
