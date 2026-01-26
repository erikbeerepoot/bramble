import { Database, CloudUpload } from 'lucide-react';
import { getBacklogStatus } from '../types';

interface BacklogStatusProps {
  /** Untransmitted records on sensor's flash (sensor -> hub) */
  pendingRecords: number | null | undefined;
  /** Pending commands queued at hub (hub -> sensor) */
  hubQueueCount?: number | null;
  showLabel?: boolean;
  size?: 'sm' | 'md' | 'lg';
}

function BacklogStatus({
  pendingRecords,
  hubQueueCount,
  showLabel = true,
  size = 'sm',
}: BacklogStatusProps) {
  const sensorStatus = getBacklogStatus(pendingRecords);
  const hubStatus = getBacklogStatus(hubQueueCount);

  // Size configurations
  const sizeConfig = {
    sm: { icon: 14, textSize: 'text-xs', gap: 'space-x-0.5' },
    md: { icon: 16, textSize: 'text-sm', gap: 'space-x-1' },
    lg: { icon: 20, textSize: 'text-base', gap: 'space-x-1.5' },
  };

  const config = sizeConfig[size];

  // Color mapping for Tailwind classes
  const colorMap: Record<string, string> = {
    green: 'text-green-500',
    yellow: 'text-yellow-500',
    red: 'text-red-500',
    gray: 'text-gray-400',
  };

  const sensorColorClass = colorMap[sensorStatus.color] || colorMap.gray;
  const hubColorClass = colorMap[hubStatus.color] || colorMap.gray;

  // Don't render anything if both are unknown/null
  if (pendingRecords === null && hubQueueCount === null) {
    return null;
  }

  return (
    <div className={`flex items-center ${config.gap}`}>
      {/* Sensor backlog (flash -> hub) */}
      {pendingRecords !== null && pendingRecords !== undefined && (
        <div
          className={`flex items-center ${config.gap}`}
          title="Sensor backlog (untransmitted records)"
        >
          <Database size={config.icon} className={sensorColorClass} />
          {showLabel && (
            <span className={`${config.textSize} ${sensorColorClass} font-medium`}>
              {pendingRecords}
            </span>
          )}
        </div>
      )}

      {/* Hub queue (pending commands for node) */}
      {hubQueueCount !== null && hubQueueCount !== undefined && (
        <div className={`flex items-center ${config.gap}`} title="Hub queue (pending commands)">
          <CloudUpload size={config.icon} className={hubColorClass} />
          {showLabel && (
            <span className={`${config.textSize} ${hubColorClass} font-medium`}>
              {hubQueueCount}
            </span>
          )}
        </div>
      )}
    </div>
  );
}

export default BacklogStatus;
