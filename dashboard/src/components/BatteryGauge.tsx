import {
  Battery,
  BatteryLow,
  BatteryMedium,
  BatteryFull,
  BatteryWarning,
  Plug,
} from 'lucide-react';
import { getBatteryStatus } from '../types';

interface BatteryGaugeProps {
  level: number | null;
  showLabel?: boolean;
  size?: 'sm' | 'md' | 'lg';
}

function BatteryGauge({ level, showLabel = true, size = 'sm' }: BatteryGaugeProps) {
  const status = getBatteryStatus(level);

  // Size configurations
  const sizeConfig = {
    sm: { icon: 16, textSize: 'text-xs' },
    md: { icon: 20, textSize: 'text-sm' },
    lg: { icon: 24, textSize: 'text-base' },
  };

  const config = sizeConfig[size];

  // Color mapping for Tailwind classes
  const colorMap: Record<string, string> = {
    green: 'text-green-500',
    yellow: 'text-yellow-500',
    red: 'text-red-500',
    blue: 'text-blue-500',
    gray: 'text-gray-400',
  };

  const colorClass = colorMap[status.color] || colorMap.gray;

  // Select appropriate battery icon based on level
  const getBatteryIcon = () => {
    if (level === null) {
      return <Battery size={config.icon} className="text-gray-400" />;
    }

    // External power
    if (level === 255) {
      return <Plug size={config.icon} className={colorClass} />;
    }

    // Critical - use warning icon
    if (level < 10) {
      return <BatteryWarning size={config.icon} className={colorClass} />;
    }

    // Low
    if (level < 30) {
      return <BatteryLow size={config.icon} className={colorClass} />;
    }

    // Medium
    if (level < 70) {
      return <BatteryMedium size={config.icon} className={colorClass} />;
    }

    // Full
    return <BatteryFull size={config.icon} className={colorClass} />;
  };

  return (
    <div className="flex items-center space-x-1.5">
      {getBatteryIcon()}
      {showLabel && (
        <span className={`${config.textSize} text-gray-600 font-medium`}>
          {status.isExternal ? 'EXT' : level !== null ? `${level}%` : '--'}
        </span>
      )}
    </div>
  );
}

export default BatteryGauge;
