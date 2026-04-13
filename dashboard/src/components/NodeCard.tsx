import { useEffect, useState } from 'react';
import { Droplet, Home, Thermometer } from 'lucide-react';
import type { Node, Zone, SensorReading } from '../types';
import { getOverallNodeHealth, NodeType } from '../types';
import { getNodeLatestReading, getNodeSensorData } from '../api/client';
import Sparkline from './Sparkline';

const SPARKLINE_HOURS = 6;

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

const HEALTH_LABEL: Record<string, string> = {
  red: 'Error',
  orange: 'Degraded',
  yellow: 'Warning',
  green: 'Healthy',
  gray: 'Unknown',
};

function NodeCard({ node, zone, onClick }: NodeCardProps) {
  const displayName =
    node.metadata?.name || `Node ${BigInt(node.device_id).toString(16).toUpperCase()}`;
  const health = getOverallNodeHealth(node);
  const [reading, setReading] = useState<SensorReading | null>(null);
  const [sparklineData, setSparklineData] = useState<SensorReading[]>([]);
  const [sparklineWindow, setSparklineWindow] = useState<{ start: number; end: number } | null>(
    null
  );

  useEffect(() => {
    if (node.online && (node.type === NodeType.SENSOR || node.type === NodeType.GREENHOUSE)) {
      getNodeLatestReading(node.device_id).then(setReading);

      const now = Math.floor(Date.now() / 1000);
      const startTime = now - SPARKLINE_HOURS * 3600;
      setSparklineWindow({ start: startTime, end: now });
      getNodeSensorData(node.device_id, {
        startTime,
        downsample: 120, // ~3 points per hour = ~18 points for 6h
      }).then((res) => setSparklineData(res.readings));
    }
  }, [node.device_id, node.online, node.type]);

  const hasSparkline = (node.type === NodeType.SENSOR || node.type === NodeType.GREENHOUSE) && sparklineData.length >= 2;

  const getNodeIcon = (type: string) => {
    switch (type) {
      case 'IRRIGATION':
        return <Droplet className="w-3.5 h-3.5" />;
      case 'GREENHOUSE':
        return <Home className="w-3.5 h-3.5" />;
      case 'SENSOR':
        return <Thermometer className="w-3.5 h-3.5" />;
      default:
        return null;
    }
  };

  const zoneColor = zone?.color;

  return (
    <div
      onClick={onClick}
      className="relative bg-surface-card rounded-xl shadow-sm border border-gray-100 hover:shadow-md transition-all duration-200 overflow-hidden cursor-pointer group"
      style={{
        borderLeftWidth: zoneColor ? '4px' : undefined,
        borderLeftColor: zoneColor,
        backgroundColor: zoneColor ? `${zoneColor}03` : undefined,
      }}
    >
      {/* Backdrop sparkline fills the card */}
      {hasSparkline && (
        <Sparkline
          readings={sparklineData}
          dataKey="temperature_celsius"
          variant="backdrop"
          color="#6366f1"
          startTime={sparklineWindow?.start}
          endTime={sparklineWindow?.end}
        />
      )}

      {/* Status dot */}
      <div className="absolute top-4 right-4 z-10">
        <div
          className="w-2.5 h-2.5 rounded-full shadow-sm"
          title={HEALTH_LABEL[health]}
          style={{
            backgroundColor: node.online ? zoneColor || '#22c55e' : '#9ca3af',
            boxShadow: node.online ? `0 0 6px ${zoneColor || '#22c55e'}60` : '0 0 4px #9ca3af40',
          }}
        ></div>
      </div>

      <div className="relative z-10 px-5 py-4">
        {/* Name & badge */}
        <div className="flex items-center gap-3 mb-3">
          <h3 className="text-base font-semibold text-gray-900 truncate">{displayName}</h3>
          <span
            className="px-2.5 py-0.5 rounded-full text-xs font-medium"
            style={
              zoneColor
                ? {
                    backgroundColor: `${zoneColor}15`,
                    color: zoneColor,
                  }
                : {
                    backgroundColor: node.online ? '#22c55e15' : '#ef444415',
                    color: node.online ? '#22c55e' : '#ef4444',
                  }
            }
          >
            {node.online ? 'Online' : 'Offline'}
          </span>
        </div>

        {/* Sensor readings */}
        {reading && (
          <div className="flex items-center gap-4 mb-3">
            <span className="text-lg font-medium text-gray-800">
              {reading.temperature_celsius.toFixed(1)}°C
            </span>
            <span className="text-lg font-medium text-gray-800">
              {reading.humidity_percent.toFixed(0)}%
            </span>
          </div>
        )}

        {/* Type & last seen */}
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-2">
            <div className="text-gray-500">{getNodeIcon(node.type)}</div>
            <span
              className="text-xs font-medium text-gray-600 uppercase"
              style={{ letterSpacing: '0.05em' }}
            >
              {node.type}
            </span>
          </div>
          <span className="text-xs text-gray-400">
            Last seen: {formatLastSeen(node.last_seen_seconds)}
          </span>
        </div>
      </div>
    </div>
  );
}

export default NodeCard;
