import type { TimeRange } from '../types';
import { TIME_RANGES } from '../types';

interface TimeRangeSelectorProps {
  value: TimeRange;
  onChange: (value: TimeRange) => void;
}

function TimeRangeSelector({ value, onChange }: TimeRangeSelectorProps) {
  const ranges = Object.entries(TIME_RANGES) as [Exclude<TimeRange, 'custom'>, typeof TIME_RANGES[keyof typeof TIME_RANGES]][];

  return (
    <div className="flex items-center space-x-1">
      {ranges.map(([key, config]) => (
        <button
          key={key}
          onClick={() => onChange(key)}
          className={`px-3 py-1 text-sm rounded-md transition-colors ${
            value === key
              ? 'bg-bramble-600 text-white'
              : 'bg-gray-100 text-gray-600 hover:bg-gray-200'
          }`}
        >
          {config.label}
        </button>
      ))}
    </div>
  );
}

export default TimeRangeSelector;
