import type { SensorReading } from '../types';

interface SparklineProps {
  readings: SensorReading[];
  dataKey: 'temperature_celsius' | 'humidity_percent';
  color?: string;
  variant?: 'inline' | 'backdrop';
  /** Height in pixels (default: 40 for inline, 100% for backdrop) */
  height?: number;
}

/** Gap threshold in seconds — break the line if readings are >10 min apart */
const GAP_THRESHOLD = 10 * 60;

interface PathSegment {
  points: Array<{ x: number; y: number }>;
}

function buildSegments(
  readings: SensorReading[],
  dataKey: 'temperature_celsius' | 'humidity_percent',
  width: number,
  height: number,
  paddingY: number
): PathSegment[] {
  if (readings.length < 2) return [];

  const sorted = [...readings].sort((a, b) => a.timestamp - b.timestamp);
  const values = sorted.map((r) => r[dataKey]);
  const min = Math.min(...values);
  const max = Math.max(...values);
  const range = max - min || 1;

  const tMin = sorted[0].timestamp;
  const tMax = sorted[sorted.length - 1].timestamp;
  const tRange = tMax - tMin || 1;

  const segments: PathSegment[] = [];
  let current: PathSegment = { points: [] };

  for (let i = 0; i < sorted.length; i++) {
    // Check for gap
    if (i > 0 && sorted[i].timestamp - sorted[i - 1].timestamp > GAP_THRESHOLD) {
      if (current.points.length >= 2) segments.push(current);
      current = { points: [] };
    }

    const x = ((sorted[i].timestamp - tMin) / tRange) * width;
    const y = height - paddingY - ((values[i] - min) / range) * (height - paddingY * 2);
    current.points.push({ x, y });
  }

  if (current.points.length >= 2) segments.push(current);
  return segments;
}

function segmentToLinePath(seg: PathSegment): string {
  return seg.points.map((p, i) => `${i === 0 ? 'M' : 'L'}${p.x},${p.y}`).join(' ');
}

function segmentToAreaPath(seg: PathSegment, height: number): string {
  if (seg.points.length === 0) return '';
  const line = seg.points.map((p, i) => `${i === 0 ? 'M' : 'L'}${p.x},${p.y}`).join(' ');
  const lastX = seg.points[seg.points.length - 1].x;
  const firstX = seg.points[0].x;
  return `${line} L${lastX},${height} L${firstX},${height} Z`;
}

/** Inline sparkline: a small line chart rendered below the reading text */
function InlineSparkline({
  readings,
  dataKey,
  color = '#6366f1',
  height = 40,
}: SparklineProps) {
  const width = 200;
  const segments = buildSegments(readings, dataKey, width, height, 4);

  if (segments.length === 0) return null;

  return (
    <svg
      viewBox={`0 0 ${width} ${height}`}
      preserveAspectRatio="none"
      className="w-full"
      style={{ height }}
    >
      {segments.map((seg, i) => (
        <g key={i}>
          <path
            d={segmentToAreaPath(seg, height)}
            fill={color}
            opacity={0.12}
          />
          <path
            d={segmentToLinePath(seg)}
            fill="none"
            stroke={color}
            strokeWidth={1.5}
            strokeLinecap="round"
            strokeLinejoin="round"
            vectorEffect="non-scaling-stroke"
          />
        </g>
      ))}
    </svg>
  );
}

/** Backdrop sparkline: a semi-transparent area chart filling the card background */
function BackdropSparkline({
  readings,
  dataKey,
  color = '#6366f1',
}: SparklineProps) {
  const width = 400;
  const height = 120;
  const segments = buildSegments(readings, dataKey, width, height, 8);

  if (segments.length === 0) return null;

  const gradientId = `sparkline-grad-${dataKey}`;

  return (
    <svg
      viewBox={`0 0 ${width} ${height}`}
      preserveAspectRatio="none"
      className="absolute inset-0 w-full h-full"
      style={{ pointerEvents: 'none' }}
    >
      <defs>
        <linearGradient id={gradientId} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor={color} stopOpacity={0.18} />
          <stop offset="100%" stopColor={color} stopOpacity={0.03} />
        </linearGradient>
      </defs>
      {segments.map((seg, i) => (
        <g key={i}>
          <path
            d={segmentToAreaPath(seg, height)}
            fill={`url(#${gradientId})`}
          />
          <path
            d={segmentToLinePath(seg)}
            fill="none"
            stroke={color}
            strokeWidth={1.5}
            opacity={0.35}
            strokeLinecap="round"
            strokeLinejoin="round"
            vectorEffect="non-scaling-stroke"
          />
        </g>
      ))}
    </svg>
  );
}

function Sparkline(props: SparklineProps) {
  if (props.variant === 'backdrop') {
    return <BackdropSparkline {...props} />;
  }
  return <InlineSparkline {...props} />;
}

export default Sparkline;
