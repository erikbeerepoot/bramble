import type { SensorReading } from '../types';
import { parseErrorFlags } from '../types';

// Maximum gap (in seconds) between readings before breaking a region
const GAP_THRESHOLD_SECONDS = 10 * 60;

interface ErrorRegion {
  startTimestamp: number;
  endTimestamp: number;
  flags: number;
  severity: 'warning' | 'error';
  readingCount: number;
}

function buildErrorRegions(readings: SensorReading[]): ErrorRegion[] {
  const sorted = [...readings].sort((a, b) => a.timestamp - b.timestamp);
  const regions: ErrorRegion[] = [];
  let regionStart: number | null = null;
  let regionFlags = 0;
  let prevTimestamp = 0;
  let count = 0;

  function closeRegion() {
    if (regionStart === null) return;
    const errors = parseErrorFlags(regionFlags);
    const severity = errors.some((e) => e.severity === 'error') ? 'error' : 'warning';
    regions.push({
      startTimestamp: regionStart,
      endTimestamp: prevTimestamp,
      flags: regionFlags,
      severity,
      readingCount: count,
    });
    regionStart = null;
    regionFlags = 0;
    count = 0;
  }

  for (let i = 0; i < sorted.length; i++) {
    const reading = sorted[i];
    const flags = reading.flags ?? 0;

    if (i > 0 && regionStart !== null) {
      const gap = reading.timestamp - sorted[i - 1].timestamp;
      if (gap > GAP_THRESHOLD_SECONDS) {
        closeRegion();
      }
    }

    if (flags !== 0) {
      if (regionStart === null) {
        regionStart = reading.timestamp;
      }
      regionFlags |= flags;
      prevTimestamp = reading.timestamp;
      count++;
    } else {
      closeRegion();
    }
  }

  closeRegion();
  return regions;
}

function formatTime(timestamp: number): string {
  return new Date(timestamp * 1000).toLocaleString(undefined, {
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  });
}

function formatDuration(startTimestamp: number, endTimestamp: number): string {
  const seconds = endTimestamp - startTimestamp;
  if (seconds < 60) return '<1m';
  if (seconds < 3600) return `${Math.round(seconds / 60)}m`;
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.round((seconds % 3600) / 60);
  return minutes > 0 ? `${hours}h ${minutes}m` : `${hours}h`;
}

interface ErrorEventsTableProps {
  readings: SensorReading[];
}

function ErrorEventsTable({ readings }: ErrorEventsTableProps) {
  const regions = buildErrorRegions(readings);

  if (regions.length === 0) return null;

  return (
    <div>
      <h4 className="font-medium text-gray-700 mb-2">Error Events</h4>
      <div className="overflow-hidden border border-gray-200 rounded-lg">
        <table className="min-w-full text-sm">
          <thead className="bg-gray-50">
            <tr>
              <th className="px-3 py-2 text-left font-medium text-gray-500">Time</th>
              <th className="px-3 py-2 text-left font-medium text-gray-500">Duration</th>
              <th className="px-3 py-2 text-left font-medium text-gray-500">Flags</th>
              <th className="px-3 py-2 text-right font-medium text-gray-500">Readings</th>
            </tr>
          </thead>
          <tbody className="divide-y divide-gray-100">
            {regions.map((region, i) => {
              const errors = parseErrorFlags(region.flags);
              return (
                <tr key={i} className="hover:bg-gray-50">
                  <td className="px-3 py-2 text-gray-700 whitespace-nowrap">
                    {formatTime(region.startTimestamp)}
                    {region.endTimestamp !== region.startTimestamp && (
                      <span className="text-gray-400"> – {formatTime(region.endTimestamp)}</span>
                    )}
                  </td>
                  <td className="px-3 py-2 text-gray-500 whitespace-nowrap">
                    {formatDuration(region.startTimestamp, region.endTimestamp)}
                  </td>
                  <td className="px-3 py-2">
                    <div className="flex flex-wrap gap-1">
                      {errors.map((err) => (
                        <span
                          key={err.flag}
                          className={`inline-block px-1.5 py-0.5 rounded text-xs font-medium ${
                            err.severity === 'error'
                              ? 'bg-red-100 text-red-700'
                              : 'bg-amber-100 text-amber-700'
                          }`}
                        >
                          {err.label}
                        </span>
                      ))}
                    </div>
                  </td>
                  <td className="px-3 py-2 text-right text-gray-500">{region.readingCount}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}

export default ErrorEventsTable;
