import {
  Signal,
  SignalLow,
  SignalMedium,
  SignalHigh,
  SignalZero,
} from 'lucide-react';
import { getSignalQuality } from '../types';

interface SignalStrengthProps {
  rssi: number | null;
  snr?: number | null;
  showLabel?: boolean;
  size?: 'sm' | 'md' | 'lg';
}

function SignalStrength({ rssi, snr, showLabel = true, size = 'sm' }: SignalStrengthProps) {
  const quality = getSignalQuality(rssi, snr);

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
    gray: 'text-gray-400',
  };

  const colorClass = colorMap[quality.color] || colorMap.gray;

  // Select appropriate signal icon based on bars
  const getSignalIcon = () => {
    switch (quality.bars) {
      case 4:
        return <SignalHigh size={config.icon} className={colorClass} />;
      case 3:
        return <SignalMedium size={config.icon} className={colorClass} />;
      case 2:
        return <SignalLow size={config.icon} className={colorClass} />;
      case 1:
        return <SignalZero size={config.icon} className={colorClass} />;
      default:
        return <Signal size={config.icon} className="text-gray-400" />;
    }
  };

  return (
    <div className="flex items-center space-x-1.5">
      {getSignalIcon()}
      {showLabel && (
        <span className={`${config.textSize} text-gray-600 font-medium`}>
          {rssi !== null ? `${rssi}dBm` : '--'}
        </span>
      )}
    </div>
  );
}

export default SignalStrength;
