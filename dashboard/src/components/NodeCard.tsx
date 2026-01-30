import type { Node, Zone } from '../types';
import { getOverallNodeHealth } from '../types';
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

const HEALTH_DOT_COLOR: Record<string, string> = {
  red: 'bg-red-500',
  orange: 'bg-orange-500',
  yellow: 'bg-yellow-400',
  green: 'bg-green-500',
  gray: 'bg-gray-400',
};

const HEALTH_LABEL: Record<string, string> = {
  red: 'Error',
  orange: 'Degraded',
  yellow: 'Warning',
  green: 'Healthy',
  gray: 'Unknown',
};

function NodeCard({ node, zone, onClick }: NodeCardProps) {
  const displayName = node.metadata?.name || `Node ${node.address}`;
  const health = getOverallNodeHealth(node);

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

        </div>

        {/* Overall health indicator */}
        <div
          className={`flex-shrink-0 ml-4 w-3 h-3 rounded-full ${HEALTH_DOT_COLOR[health]}`}
          title={HEALTH_LABEL[health]}
        />
      </div>

      <div className="mt-3 flex items-center justify-between text-sm">
        <div className="flex items-center space-x-3">
          <span className="text-gray-500 font-medium">{node.type}</span>
          <BacklogStatus pendingRecords={node.status?.pending_records} size="sm" />
        </div>
        <div className="text-gray-400">
          Last seen: {formatLastSeen(node.last_seen_seconds)}
        </div>
      </div>
    </div>
  );
}

export default NodeCard;
