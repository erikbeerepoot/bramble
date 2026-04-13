import { useState, useRef } from 'react';
import { format } from 'date-fns';
import type { SensorReading } from '../types';

interface SparklineProps {
  readings: SensorReading[];
  dataKey: 'temperature_celsius' | 'humidity_percent';
  color?: string;
  variant?: 'inline' | 'backdrop';
  /** Height in pixels (default: 40 for inline, 100% for backdrop) */
  height?: number;
  /** Show hover dot + value tooltip on pointer move (inline only) */
  interactive?: boolean;
  /** Unit label for hover tooltip (e.g. "°C", "%") */
  unit?: string;
  /** Optional fixed time window for x-axis scaling (unix seconds).
   *  When provided, the chart x-axis spans this window instead of
   *  the data's own min/max timestamps. */
  startTime?: number;
  endTime?: number;
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
  paddingY: number,
  tStart?: number,
  tEnd?: number
): PathSegment[] {
  if (readings.length < 2) return [];

  const sorted = [...readings].sort((a, b) => a.timestamp - b.timestamp);
  const values = sorted.map((r) => r[dataKey]);
  const min = Math.min(...values);
  const max = Math.max(...values);
  const range = max - min || 1;

  const tMin = tStart ?? sorted[0].timestamp;
  const tMax = tEnd ?? sorted[sorted.length - 1].timestamp;
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
  interactive = false,
  unit = '',
  startTime,
  endTime,
}: SparklineProps) {
  const width = 200;
  const paddingY = 4;
  const segments = buildSegments(readings, dataKey, width, height, paddingY, startTime, endTime);
  const containerRef = useRef<HTMLDivElement | null>(null);
  const [hoverIdx, setHoverIdx] = useState<number | null>(null);

  if (segments.length === 0) return null;

  // Sorted readings mirror buildSegments ordering so indices align for hover lookup
  const sorted = [...readings].sort((a, b) => a.timestamp - b.timestamp);
  const values = sorted.map((r) => r[dataKey]);
  const valMin = Math.min(...values);
  const valMax = Math.max(...values);
  const valRange = valMax - valMin || 1;
  const tMin = startTime ?? sorted[0].timestamp;
  const tMax = endTime ?? sorted[sorted.length - 1].timestamp;
  const tRange = tMax - tMin || 1;

  const pointFor = (idx: number) => {
    const x = ((sorted[idx].timestamp - tMin) / tRange) * width;
    const y = height - paddingY - ((values[idx] - valMin) / valRange) * (height - paddingY * 2);
    return { x, y };
  };

  const handlePointerMove = (event: React.PointerEvent<HTMLDivElement>) => {
    if (!interactive || !containerRef.current) return;
    const rect = containerRef.current.getBoundingClientRect();
    const px = event.clientX - rect.left;
    const hoverX = (px / rect.width) * width;
    // Find nearest point by x
    let nearest = 0;
    let nearestDist = Infinity;
    for (let i = 0; i < sorted.length; i++) {
      const x = ((sorted[i].timestamp - tMin) / tRange) * width;
      const dist = Math.abs(x - hoverX);
      if (dist < nearestDist) {
        nearestDist = dist;
        nearest = i;
      }
    }
    setHoverIdx(nearest);
  };

  const handlePointerLeave = () => setHoverIdx(null);

  const hover = hoverIdx !== null ? pointFor(hoverIdx) : null;
  const hoverReading = hoverIdx !== null ? sorted[hoverIdx] : null;
  const hoverValue = hoverReading ? hoverReading[dataKey] : null;
  // Position tooltip: flip to left edge if hover is in the right 40%
  const tooltipLeftPct = hover ? (hover.x / width) * 100 : 0;
  const tooltipAlignRight = tooltipLeftPct > 60;

  return (
    <div
      ref={containerRef}
      className="relative w-full"
      style={{ height }}
      onPointerMove={interactive ? handlePointerMove : undefined}
      onPointerLeave={interactive ? handlePointerLeave : undefined}
    >
      <svg
        viewBox={`0 0 ${width} ${height}`}
        preserveAspectRatio="none"
        className="w-full h-full block"
      >
        {segments.map((seg, i) => (
          <g key={i}>
            <path d={segmentToAreaPath(seg, height)} fill={color} opacity={0.12} />
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
        {interactive && hover && (
          <>
            <line
              x1={hover.x}
              x2={hover.x}
              y1={0}
              y2={height}
              stroke={color}
              strokeOpacity={0.3}
              strokeWidth={1}
              vectorEffect="non-scaling-stroke"
            />
            <circle
              cx={hover.x}
              cy={hover.y}
              r={3}
              fill="white"
              stroke={color}
              strokeWidth={2}
              vectorEffect="non-scaling-stroke"
            />
          </>
        )}
      </svg>
      {interactive && hover && hoverReading && hoverValue !== null && (
        <div
          className="pointer-events-none absolute top-0 z-10 bg-white/95 border border-gray-200 shadow-sm rounded px-2 py-1 text-xs whitespace-nowrap"
          style={
            tooltipAlignRight
              ? { right: `${100 - tooltipLeftPct}%`, marginRight: 6 }
              : { left: `${tooltipLeftPct}%`, marginLeft: 6 }
          }
        >
          <div className="font-medium text-gray-900">
            {hoverValue.toFixed(1)}
            {unit}
          </div>
          <div className="text-gray-500">
            {format(new Date(hoverReading.timestamp * 1000), 'MMM d, HH:mm')}
          </div>
        </div>
      )}
    </div>
  );
}

/** Backdrop sparkline: a semi-transparent area chart filling the card background */
function BackdropSparkline({
  readings,
  dataKey,
  color = '#6366f1',
  startTime,
  endTime,
}: SparklineProps) {
  const width = 400;
  const height = 120;
  const segments = buildSegments(readings, dataKey, width, height, 8, startTime, endTime);

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
          <path d={segmentToAreaPath(seg, height)} fill={`url(#${gradientId})`} />
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
