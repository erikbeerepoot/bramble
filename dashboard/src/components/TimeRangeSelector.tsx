import type { TimeRange, CustomTimeRange } from '../types';
import { TIME_RANGES } from '../types';

interface TimeRangeSelectorProps {
  value: TimeRange;
  onChange: (value: TimeRange) => void;
  customRange?: CustomTimeRange;
  onCustomRangeChange?: (range: CustomTimeRange) => void;
}

function formatDateTimeLocal(timestamp: number): string {
  const date = new Date(timestamp * 1000);
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  const hours = String(date.getHours()).padStart(2, '0');
  const minutes = String(date.getMinutes()).padStart(2, '0');
  return `${year}-${month}-${day}T${hours}:${minutes}`;
}

function parseDateTimeLocal(value: string): number {
  return Math.floor(new Date(value).getTime() / 1000);
}

function TimeRangeSelector({ value, onChange, customRange, onCustomRangeChange }: TimeRangeSelectorProps) {
  const ranges = Object.entries(TIME_RANGES) as [Exclude<TimeRange, 'custom'>, typeof TIME_RANGES[keyof typeof TIME_RANGES]][];

  const handleStartChange = (event: React.ChangeEvent<HTMLInputElement>) => {
    if (onCustomRangeChange && customRange) {
      onCustomRangeChange({
        ...customRange,
        startTime: parseDateTimeLocal(event.target.value),
      });
    }
  };

  const handleEndChange = (event: React.ChangeEvent<HTMLInputElement>) => {
    if (onCustomRangeChange && customRange) {
      onCustomRangeChange({
        ...customRange,
        endTime: parseDateTimeLocal(event.target.value),
      });
    }
  };

  return (
    <div className="flex flex-col space-y-2">
      <div className="flex flex-wrap gap-1">
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
        <button
          onClick={() => onChange('custom')}
          className={`px-3 py-1 text-sm rounded-md transition-colors ${
            value === 'custom'
              ? 'bg-bramble-600 text-white'
              : 'bg-gray-100 text-gray-600 hover:bg-gray-200'
          }`}
        >
          Custom
        </button>
      </div>
      {value === 'custom' && customRange && (
        <div className="flex flex-col sm:flex-row sm:items-center gap-2 text-sm">
          <div className="flex items-center gap-2">
            <label className="text-gray-600 shrink-0">From:</label>
            <input
              type="datetime-local"
              value={formatDateTimeLocal(customRange.startTime)}
              onChange={handleStartChange}
              className="px-2 py-1 border border-gray-300 rounded-md focus:outline-none focus:ring-2 focus:ring-bramble-500 min-w-0"
            />
          </div>
          <div className="flex items-center gap-2">
            <label className="text-gray-600 shrink-0">To:</label>
            <input
              type="datetime-local"
              value={formatDateTimeLocal(customRange.endTime)}
              onChange={handleEndChange}
              className="px-2 py-1 border border-gray-300 rounded-md focus:outline-none focus:ring-2 focus:ring-bramble-500 min-w-0"
            />
          </div>
        </div>
      )}
    </div>
  );
}

export default TimeRangeSelector;
