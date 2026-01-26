import { useState } from 'react';
import { CheckCircle2, AlertTriangle, AlertCircle } from 'lucide-react';
import { parseErrorFlags, getHealthStatus } from '../types';

interface HealthStatusProps {
  errorFlags: number | null;
  size?: 'sm' | 'md' | 'lg';
  showTooltip?: boolean;
}

function HealthStatus({ errorFlags, size = 'sm', showTooltip = true }: HealthStatusProps) {
  const [tooltipVisible, setTooltipVisible] = useState(false);

  const status = getHealthStatus(errorFlags);
  const errors = parseErrorFlags(errorFlags);

  // Size configurations
  const sizeConfig = {
    sm: { icon: 16, textSize: 'text-xs' },
    md: { icon: 20, textSize: 'text-sm' },
    lg: { icon: 24, textSize: 'text-base' },
  };

  const config = sizeConfig[size];

  // Get primary error label (first error for display)
  const primaryLabel = status === 'healthy' ? 'Healthy' : errors[0]?.label || 'Unknown';

  // Get status icon and color
  const getStatusIcon = () => {
    switch (status) {
      case 'healthy':
        return <CheckCircle2 size={config.icon} className="text-green-500" />;
      case 'warning':
        return <AlertTriangle size={config.icon} className="text-yellow-500" />;
      case 'error':
        return <AlertCircle size={config.icon} className="text-red-500" />;
    }
  };

  return (
    <div
      className="relative inline-flex items-center space-x-1.5"
      onMouseEnter={() => setTooltipVisible(true)}
      onMouseLeave={() => setTooltipVisible(false)}
    >
      {getStatusIcon()}
      <span className={`${config.textSize} text-gray-600 dark:text-gray-300`}>{primaryLabel}</span>

      {/* Tooltip with full error list */}
      {showTooltip && tooltipVisible && errors.length > 0 && (
        <div className="absolute bottom-full left-0 mb-2 z-10">
          <div className="bg-gray-900 text-white text-xs rounded-md shadow-lg p-2 min-w-[140px]">
            <div className="font-medium mb-1">Active Issues:</div>
            <ul className="space-y-0.5">
              {errors.map((error) => (
                <li key={error.flag} className="flex items-center space-x-1.5">
                  {error.severity === 'error' ? (
                    <AlertCircle size={10} className="text-red-400" />
                  ) : (
                    <AlertTriangle size={10} className="text-yellow-400" />
                  )}
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
