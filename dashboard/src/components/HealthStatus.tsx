import { useState } from 'react';
import { parseErrorFlags, getHealthStatus } from '../types';

interface HealthStatusProps {
  errorFlags: number | null;
  size?: 'sm' | 'md' | 'lg';
  showTooltip?: boolean;
}

const STATUS_DOT_COLOR: Record<string, string> = {
  healthy: 'bg-green-500',
  warning: 'bg-yellow-400',
  error: 'bg-red-500',
};

const SEVERITY_DOT_COLOR: Record<string, string> = {
  error: 'bg-red-400',
  warning: 'bg-yellow-400',
};

function HealthStatus({ errorFlags, size = 'sm', showTooltip = true }: HealthStatusProps) {
  const [tooltipVisible, setTooltipVisible] = useState(false);

  const status = getHealthStatus(errorFlags);
  const errors = parseErrorFlags(errorFlags);

  // Size configurations
  const sizeConfig = {
    sm: { dot: 'w-3 h-3', textSize: 'text-xs' },
    md: { dot: 'w-3.5 h-3.5', textSize: 'text-sm' },
    lg: { dot: 'w-4 h-4', textSize: 'text-base' },
  };

  const config = sizeConfig[size];

  // Get primary error label (first error for display)
  const primaryLabel = status === 'healthy' ? 'Healthy' : errors[0]?.label || 'Unknown';

  return (
    <div
      className="relative inline-flex items-center space-x-1.5"
      onMouseEnter={() => setTooltipVisible(true)}
      onMouseLeave={() => setTooltipVisible(false)}
    >
      <span className={`${config.dot} rounded-full flex-shrink-0 ${STATUS_DOT_COLOR[status]}`} />
      <span className={`${config.textSize} text-gray-600`}>{primaryLabel}</span>

      {/* Tooltip with full error list */}
      {showTooltip && tooltipVisible && errors.length > 0 && (
        <div className="absolute bottom-full left-0 mb-2 z-10">
          <div className="bg-gray-900 text-white text-xs rounded-md shadow-lg p-2 min-w-[140px]">
            <div className="font-medium mb-1">Active Issues:</div>
            <ul className="space-y-0.5">
              {errors.map((error) => (
                <li key={error.flag} className="flex items-center space-x-1.5">
                  <span className={`w-2 h-2 rounded-full flex-shrink-0 ${SEVERITY_DOT_COLOR[error.severity]}`} />
                  <span>{error.label}</span>
                </li>
              ))}
            </ul>
            {/* Tooltip arrow */}
            <div className="absolute top-full left-4 w-0 h-0 border-l-4 border-r-4 border-t-4 border-transparent border-t-gray-900" />
          </div>
        </div>
      )}
    </div>
  );
}

export default HealthStatus;
