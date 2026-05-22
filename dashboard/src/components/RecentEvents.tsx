import {
  Calendar,
  CalendarX,
  AlertTriangle,
  Timer,
  TimerOff,
  Droplet,
  DropletOff,
  Power,
  RotateCcw,
  Clock,
  Activity,
  Wifi,
  WifiOff,
  Moon,
  Info,
  CheckCircle2,
  Square,
  ArrowUpFromLine,
  ArrowDownFromLine,
  Copy,
  Check,
  Loader2,
  XCircle,
  Settings2,
} from 'lucide-react';
import { format, isToday, isYesterday } from 'date-fns';
import { useState } from 'react';
import { motion, AnimatePresence } from 'motion/react';
import type { NodeEvent, NodeCommand, NodeCommandType } from '../types';
import {
  getEventName,
  getEventDetail,
  parseEventDetail,
  formatDuration,
  EventType,
  EventCode,
} from '../types';
import type { CurtainAction } from './CurtainControl';

export interface PendingEvent {
  action: CurtainAction;
  expectedEventCode: number;
  createdAt: number;
  status: 'pending' | 'confirmed';
}

const PENDING_EVENT_LABELS: Record<CurtainAction, string> = {
  open: 'Curtain Open',
  close: 'Curtain Close',
  stop: 'Curtain Stop',
};

interface RecentEventsProps {
  events: NodeEvent[];
  commands?: NodeCommand[];
  loading: boolean;
  pendingEvent?: PendingEvent | null;
  onLoadMore?: () => void;
  hasMore?: boolean;
  loadingMore?: boolean;
}

type IconComponent = typeof Calendar;

interface EventVisual {
  icon: IconComponent;
  color: string;
  bgColor: string;
}

const DEFAULT_VISUAL: EventVisual = {
  icon: Info,
  color: 'text-gray-600',
  bgColor: 'bg-gray-50',
};

// Visual config per event code. Keys match values in src/util/event_record.h
// (EventType, 1-byte) and src/lora/message.h (EventCode, 2-byte).
const EVENT_VISUALS: Record<number, EventVisual> = {
  // System
  [EventType.BOOT_COLD]: { icon: Power, color: 'text-gray-600', bgColor: 'bg-gray-50' },
  [EventType.BOOT_WATCHDOG]: {
    icon: RotateCcw,
    color: 'text-amber-600',
    bgColor: 'bg-amber-50',
  },
  [EventType.WAKE]: { icon: Moon, color: 'text-gray-500', bgColor: 'bg-gray-50' },
  [EventType.SLEEP_ENTER]: { icon: Moon, color: 'text-gray-500', bgColor: 'bg-gray-50' },
  // Sensor
  [EventType.SENSOR_INIT_OK]: {
    icon: Activity,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventType.SENSOR_INIT_FAIL]: {
    icon: AlertTriangle,
    color: 'text-red-600',
    bgColor: 'bg-red-50',
  },
  [EventType.SENSOR_READ_OK]: {
    icon: Activity,
    color: 'text-gray-600',
    bgColor: 'bg-gray-50',
  },
  [EventType.SENSOR_READ_FAIL]: {
    icon: AlertTriangle,
    color: 'text-red-600',
    bgColor: 'bg-red-50',
  },
  // Network
  [EventType.REGISTRATION_OK]: {
    icon: Wifi,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventType.REGISTRATION_FAIL]: {
    icon: WifiOff,
    color: 'text-red-600',
    bgColor: 'bg-red-50',
  },
  [EventType.TIME_SYNC_OK]: {
    icon: Clock,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventType.TIME_SYNC_TIMEOUT]: {
    icon: Clock,
    color: 'text-amber-600',
    bgColor: 'bg-amber-50',
  },
  [EventType.TX_BATCH_OK]: {
    icon: CheckCircle2,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventType.TX_BATCH_FAIL]: {
    icon: AlertTriangle,
    color: 'text-red-600',
    bgColor: 'bg-red-50',
  },
  // Irrigation — schedules
  [EventType.SCHEDULE_APPLIED]: {
    icon: Calendar,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventType.SCHEDULE_REMOVED]: {
    icon: CalendarX,
    color: 'text-amber-600',
    bgColor: 'bg-amber-50',
  },
  [EventType.SCHEDULE_FAILED]: {
    icon: AlertTriangle,
    color: 'text-red-600',
    bgColor: 'bg-red-50',
  },
  // Irrigation — valve
  [EventType.VALVE_TIMER_SET]: {
    icon: Timer,
    color: 'text-blue-600',
    bgColor: 'bg-blue-50',
  },
  [EventType.VALVE_TIMER_CLOSE]: {
    icon: TimerOff,
    color: 'text-gray-600',
    bgColor: 'bg-gray-50',
  },
  [EventType.VALVE_OPEN]: {
    icon: Droplet,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventType.VALVE_CLOSE]: {
    icon: DropletOff,
    color: 'text-gray-600',
    bgColor: 'bg-gray-50',
  },
  // Curtain
  [EventCode.EVENT_CURTAIN_OPENING]: {
    icon: ArrowUpFromLine,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventCode.EVENT_CURTAIN_CLOSING]: {
    icon: ArrowDownFromLine,
    color: 'text-gray-600',
    bgColor: 'bg-gray-50',
  },
  [EventCode.EVENT_CURTAIN_STOPPED]: {
    icon: Square,
    color: 'text-amber-600',
    bgColor: 'bg-amber-50',
  },
  [EventCode.EVENT_MOTOR_ERROR]: {
    icon: AlertTriangle,
    color: 'text-red-600',
    bgColor: 'bg-red-50',
  },
  [EventCode.EVENT_CALIBRATION_COMPLETE]: {
    icon: CheckCircle2,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
};

function CopyableTimestamp({ timestamp }: { timestamp: number }) {
  const [copied, setCopied] = useState(false);
  const date = new Date(timestamp * 1000);
  const isoString = date.toISOString();

  const handleCopy = async () => {
    await navigator.clipboard.writeText(isoString);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  return (
    <button
      onClick={handleCopy}
      className="group/ts flex items-center gap-1 text-xs text-gray-500 hover:text-gray-700 transition-colors font-mono"
      title={`Click to copy UTC: ${isoString}`}
    >
      <span>{format(date, 'HH:mm:ss')}</span>
      {copied ? (
        <Check className="w-3 h-3 text-green-600" />
      ) : (
        <Copy className="w-3 h-3 opacity-0 group-hover/ts:opacity-100 transition-opacity" />
      )}
    </button>
  );
}

function PendingEventIcon({ status }: { status: 'pending' | 'confirmed' }) {
  return (
    <AnimatePresence mode="wait">
      {status === 'pending' ? (
        <motion.div
          key="pending"
          initial={{ opacity: 0, scale: 0.8 }}
          animate={{ opacity: 1, scale: 1 }}
          exit={{ opacity: 0, scale: 0.8 }}
          transition={{ duration: 0.2 }}
          className="w-6 h-6 rounded-full bg-blue-50 flex items-center justify-center"
        >
          <Loader2 className="w-3.5 h-3.5 animate-spin text-blue-600" />
        </motion.div>
      ) : (
        <motion.div key="confirmed" className="relative w-6 h-6 flex items-center justify-center">
          <motion.div
            initial={{ scale: 0, opacity: 0.6 }}
            animate={{ scale: 1.4, opacity: 0 }}
            transition={{ duration: 0.6 }}
            className="absolute inset-0 bg-emerald-400 rounded-full"
          />
          <motion.div
            initial={{ opacity: 0, scale: 0.3, rotate: -180 }}
            animate={{ opacity: 1, scale: 1, rotate: 0 }}
            transition={{
              duration: 0.6,
              scale: { type: 'spring', stiffness: 260, damping: 12 },
              rotate: { duration: 0.5 },
            }}
          >
            <CheckCircle2 className="w-4 h-4 text-emerald-600 fill-emerald-100" />
          </motion.div>
        </motion.div>
      )}
    </AnimatePresence>
  );
}

// Visual config per command type. Reuses event icons/colors so a "Valve Open"
// command row looks consistent with the eventual "Valve Open" event row.
const COMMAND_VISUALS: Record<NodeCommandType, EventVisual> = {
  valve_open: EVENT_VISUALS[EventType.VALVE_OPEN] ?? DEFAULT_VISUAL,
  valve_close: EVENT_VISUALS[EventType.VALVE_CLOSE] ?? DEFAULT_VISUAL,
  curtain: { icon: ArrowUpFromLine, color: 'text-gray-600', bgColor: 'bg-gray-50' },
  wake_interval: { icon: Clock, color: 'text-gray-600', bgColor: 'bg-gray-50' },
  schedule_set: EVENT_VISUALS[EventType.SCHEDULE_APPLIED] ?? DEFAULT_VISUAL,
  schedule_remove: EVENT_VISUALS[EventType.SCHEDULE_REMOVED] ?? DEFAULT_VISUAL,
};

function curtainVisualForAction(action: string | undefined): EventVisual {
  switch (action) {
    case 'open':
      return EVENT_VISUALS[EventCode.EVENT_CURTAIN_OPENING] ?? DEFAULT_VISUAL;
    case 'close':
      return EVENT_VISUALS[EventCode.EVENT_CURTAIN_CLOSING] ?? DEFAULT_VISUAL;
    case 'stop':
      return EVENT_VISUALS[EventCode.EVENT_CURTAIN_STOPPED] ?? DEFAULT_VISUAL;
    case 'calibrate':
      return { icon: Settings2, color: 'text-blue-600', bgColor: 'bg-blue-50' };
    default:
      return COMMAND_VISUALS.curtain;
  }
}

// Human-readable label for the command row before the status suffix.
function commandLabel(command: NodeCommand): string {
  const params = command.params as Record<string, unknown>;
  switch (command.command_type) {
    case 'valve_open': {
      const valve = typeof params.valve === 'number' ? params.valve : 0;
      const dur = typeof params.duration_seconds === 'number'
        ? params.duration_seconds
        : null;
      return dur !== null
        ? `Valve ${valve + 1} Open · ${formatDuration(dur)}`
        : `Valve ${valve + 1} Open`;
    }
    case 'valve_close': {
      const valve = typeof params.valve === 'number' ? params.valve : 0;
      return `Valve ${valve + 1} Close`;
    }
    case 'curtain': {
      const action = typeof params.action === 'string' ? params.action : '';
      const label =
        action === 'open'
          ? 'Curtain Open'
          : action === 'close'
            ? 'Curtain Close'
            : action === 'stop'
              ? 'Curtain Stop'
              : action === 'calibrate'
                ? 'Curtain Calibrate'
                : `Curtain ${action}`;
      return label;
    }
    case 'wake_interval': {
      const interval = typeof params.interval_seconds === 'number'
        ? params.interval_seconds
        : null;
      return interval !== null ? `Wake Interval · ${interval}s` : 'Wake Interval';
    }
    case 'schedule_set': {
      const index = typeof params.index === 'number' ? params.index : 0;
      const hour = typeof params.hour === 'number' ? params.hour : 0;
      const minute = typeof params.minute === 'number' ? params.minute : 0;
      const duration = typeof params.duration === 'number' ? params.duration : null;
      const time = `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`;
      return duration !== null
        ? `Schedule #${index} · ${time} · ${formatDuration(duration)}`
        : `Schedule #${index} · ${time}`;
    }
    case 'schedule_remove': {
      const index = typeof params.index === 'number' ? params.index : 0;
      return `Remove Schedule #${index}`;
    }
  }
}

function commandVisual(command: NodeCommand): EventVisual {
  if (command.command_type === 'curtain') {
    return curtainVisualForAction(command.params.action as string | undefined);
  }
  return COMMAND_VISUALS[command.command_type];
}

// Suffix shown after the command label to indicate lifecycle status.
function commandStatusSuffix(command: NodeCommand): string {
  switch (command.status) {
    case 'pending':
      return ' · Pending';
    case 'confirmed': {
      if (command.confirmed_at) {
        const delta = command.confirmed_at - command.created_at;
        return delta > 0 ? ` · Confirmed · ${formatDuration(delta)}` : ' · Confirmed';
      }
      return ' · Confirmed';
    }
    case 'failed':
      return ' · Failed';
    case 'expired':
      return command.command_type === 'wake_interval'
        ? ' · Sent (no confirmation available)'
        : ' · Expired (no response)';
    case 'cancelled':
      return ' · Cancelled';
  }
}

// Status icon overlay rendered next to the row's event/command icon.
function CommandStatusBadge({ status }: { status: NodeCommand['status'] }) {
  if (status === 'pending') {
    return (
      <div className="w-6 h-6 rounded-full bg-blue-50 flex items-center justify-center">
        <Loader2 className="w-3.5 h-3.5 animate-spin text-blue-600" />
      </div>
    );
  }
  if (status === 'failed') {
    return (
      <div className="w-6 h-6 rounded-full bg-red-50 flex items-center justify-center">
        <XCircle className="w-3.5 h-3.5 text-red-600" />
      </div>
    );
  }
  if (status === 'expired' || status === 'cancelled') {
    return (
      <div className="w-6 h-6 rounded-full bg-gray-50 flex items-center justify-center">
        <Clock className="w-3.5 h-3.5 text-gray-400" />
      </div>
    );
  }
  // confirmed: use the existing animated check from PendingEventIcon.
  return <PendingEventIcon status="confirmed" />;
}

// Event codes that collapse into summary rows (per-wake-cycle noise).
const WAKE_CYCLE_CODES = new Set<number>([EventType.WAKE, EventType.SLEEP_ENTER]);

// Pair VALVE_OPEN events with the next matching VALVE_CLOSE / VALVE_TIMER_CLOSE
// (same valve_id) to derive a duration. Events arrive newest-first.
// Returns a map keyed by the OPEN event's timestamp -> duration in seconds.
function annotateDurations(events: NodeEvent[]): Map<number, number> {
  const durationByOpenTs = new Map<number, number>();
  const openByValve = new Map<number, NodeEvent>();
  // Walk oldest -> newest so opens come before closes.
  for (let i = events.length - 1; i >= 0; i--) {
    const event = events[i];
    const valveId = parseEventDetail(event.data_hex);
    if (valveId === null) continue;
    if (event.event_code === EventType.VALVE_OPEN) {
      openByValve.set(valveId, event);
    } else if (
      event.event_code === EventType.VALVE_CLOSE ||
      event.event_code === EventType.VALVE_TIMER_CLOSE
    ) {
      const open = openByValve.get(valveId);
      if (open) {
        const duration = event.timestamp - open.timestamp;
        if (duration > 0) {
          durationByOpenTs.set(open.timestamp, duration);
        }
        openByValve.delete(valveId);
      }
    }
  }
  return durationByOpenTs;
}

// One row of the timeline — events (single or collapsed run of WAKE/SLEEP_ENTER)
// or commands. Commands use their created_at as the timeline timestamp so the
// row stays in place across pending → confirmed transitions.
type TimelineItem =
  | { kind: 'event'; ts: number; event: NodeEvent }
  | { kind: 'command'; ts: number; command: NodeCommand };

type RenderItem =
  | { kind: 'event'; event: NodeEvent }
  | { kind: 'command'; command: NodeCommand }
  | {
      kind: 'collapsed';
      events: NodeEvent[]; // consecutive run of WAKE/SLEEP_ENTER
    };

function mergeTimeline(events: NodeEvent[], commands: NodeCommand[]): TimelineItem[] {
  const items: TimelineItem[] = [];
  for (const event of events) items.push({ kind: 'event', ts: event.timestamp, event });
  for (const command of commands) {
    items.push({ kind: 'command', ts: command.created_at, command });
  }
  items.sort((a, b) => b.ts - a.ts);
  return items;
}

function collapseWakeCycles(dayItems: TimelineItem[]): RenderItem[] {
  const items: RenderItem[] = [];
  let run: NodeEvent[] = [];
  const flush = () => {
    if (run.length === 0) return;
    if (run.length === 1) {
      items.push({ kind: 'event', event: run[0] });
    } else {
      items.push({ kind: 'collapsed', events: run });
    }
    run = [];
  };
  for (const item of dayItems) {
    if (item.kind === 'event' && WAKE_CYCLE_CODES.has(item.event.event_code)) {
      run.push(item.event);
    } else if (item.kind === 'event') {
      flush();
      items.push({ kind: 'event', event: item.event });
    } else {
      flush();
      items.push({ kind: 'command', command: item.command });
    }
  }
  flush();
  return items;
}

// Compose the trailing detail string for an event row — valve/schedule id and,
// for an OPEN with a known matching close, the watering duration.
function eventLabelSuffix(
  event: NodeEvent,
  durationByOpenTs: Map<number, number>
): string {
  const parts: string[] = [];
  const detail = getEventDetail(event.event_code, event.data_hex);
  if (detail) parts.push(detail);
  if (event.event_code === EventType.VALVE_OPEN) {
    const dur = durationByOpenTs.get(event.timestamp);
    if (dur !== undefined) parts.push(formatDuration(dur));
  }
  return parts.length ? ` · ${parts.join(' · ')}` : '';
}

function formatCollapsedLabel(events: NodeEvent[]): string {
  const wakes = events.filter((e) => e.event_code === EventType.WAKE).length;
  const sleeps = events.filter((e) => e.event_code === EventType.SLEEP_ENTER).length;
  const fullCycles = Math.min(wakes, sleeps);
  const leftover = events.length - fullCycles * 2;
  if (fullCycles === 0) {
    // Only wakes or only sleeps — unusual; just show count.
    return `${events.length} wake/sleep events`;
  }
  const cycleLabel = fullCycles === 1 ? '1 wake/sleep cycle' : `${fullCycles} wake/sleep cycles`;
  return leftover > 0 ? `${cycleLabel} + ${leftover}` : cycleLabel;
}

export function RecentEvents({
  events,
  commands,
  loading,
  pendingEvent,
  onLoadMore,
  hasMore,
  loadingMore,
}: RecentEventsProps) {
  const [expandedRuns, setExpandedRuns] = useState<Set<string>>(new Set());
  const toggleRun = (key: string) => {
    setExpandedRuns((prev) => {
      const next = new Set(prev);
      if (next.has(key)) {
        next.delete(key);
      } else {
        next.add(key);
      }
      return next;
    });
  };

  // Filter out the real event that matches the pending one to avoid duplicates
  const filteredEvents = pendingEvent
    ? events.filter(
        (e) =>
          !(
            e.event_code === pendingEvent.expectedEventCode &&
            e.timestamp >= pendingEvent.createdAt - 5
          )
      )
    : events;

  // Computed once over all events so pairs spanning day boundaries still resolve.
  const durationByOpenTs = annotateDurations(filteredEvents);

  // Merge events + commands into a single timeline, then bucket by day.
  const timeline = mergeTimeline(filteredEvents, commands ?? []);

  const groupedItems = timeline.reduce(
    (acc, item) => {
      const date = new Date(item.ts * 1000);
      let dayLabel: string;
      if (isToday(date)) {
        dayLabel = 'Today';
      } else if (isYesterday(date)) {
        dayLabel = 'Yesterday';
      } else {
        dayLabel = format(date, 'MMM d');
      }
      if (!acc[dayLabel]) {
        acc[dayLabel] = [];
      }
      acc[dayLabel].push(item);
      return acc;
    },
    {} as Record<string, TimelineItem[]>
  );

  // Ensure "Today" group exists if we have a pending event
  if (pendingEvent && !groupedItems['Today']) {
    groupedItems['Today'] = [];
  }

  // Put "Today" first when we have a pending event
  const dayEntries = Object.entries(groupedItems);
  if (pendingEvent) {
    const todayIdx = dayEntries.findIndex(([day]) => day === 'Today');
    if (todayIdx > 0) {
      const [todayEntry] = dayEntries.splice(todayIdx, 1);
      dayEntries.unshift(todayEntry);
    }
  }

  const totalRows = events.length + (commands?.length ?? 0);

  return (
    <div className="bg-white rounded-xl border border-gray-200 shadow-sm">
      <div className="px-5 py-4 border-b border-gray-200">
        <h2 className="font-semibold text-gray-900">Recent Activity</h2>
        <p className="text-xs text-gray-500 mt-0.5">{totalRows} entries</p>
      </div>

      {loading ? (
        <div className="p-4 animate-pulse space-y-2">
          <div className="h-4 bg-gray-200 rounded w-3/4" />
          <div className="h-4 bg-gray-200 rounded w-1/2" />
        </div>
      ) : totalRows === 0 && !pendingEvent ? (
        <div className="px-5 py-4">
          <p className="text-sm text-gray-400">No activity recorded yet.</p>
        </div>
      ) : (
        <div className="px-4 py-2.5 max-h-[600px] overflow-y-auto">
          {dayEntries.map(([day, dayItems], dayIndex) => (
            <div key={day}>
              {dayIndex > 0 && <div className="h-px bg-gray-200 my-2.5" />}

              <div className="text-xs font-medium text-gray-500 mb-1.5 px-1">{day}</div>

              <div>
                {/* Pending event row — always first in "Today" */}
                <AnimatePresence>
                  {pendingEvent && day === 'Today' && (
                    <motion.div
                      key="pending-event"
                      initial={{ opacity: 0, height: 0 }}
                      animate={{ opacity: 1, height: 'auto' }}
                      exit={{ opacity: 0, height: 0 }}
                      transition={{ duration: 0.25 }}
                      className="overflow-hidden"
                    >
                      <div className="event-item group relative flex items-start gap-2.5 py-1.5 px-1.5 rounded-md">
                        <div className="relative flex-shrink-0 mt-px">
                          <PendingEventIcon status={pendingEvent.status} />
                          {dayItems.length > 0 && (
                            <div className="absolute top-7 left-1/2 -translate-x-px w-px h-2.5 bg-gray-200" />
                          )}
                        </div>
                        <div className="flex-1 min-w-0">
                          <div className="flex items-start justify-between gap-2 mb-px">
                            <span className="text-sm font-medium text-gray-900">
                              {PENDING_EVENT_LABELS[pendingEvent.action]}
                            </span>
                            <span className="text-xs text-gray-400">
                              {pendingEvent.status === 'pending' ? 'Pending...' : 'Confirmed'}
                            </span>
                          </div>
                        </div>
                      </div>
                    </motion.div>
                  )}
                </AnimatePresence>

                {(() => {
                  const renderItems = collapseWakeCycles(dayItems);
                  return renderItems.map((item, index) => {
                    const adjustedIndex =
                      pendingEvent && day === 'Today' ? index + 1 : index;
                    const totalItems =
                      renderItems.length + (pendingEvent && day === 'Today' ? 1 : 0);
                    const isLast = adjustedIndex >= totalItems - 1;

                    if (item.kind === 'command') {
                      const cmd = item.command;
                      const visual = commandVisual(cmd);
                      const Icon = visual.icon;
                      return (
                        <div
                          key={`cmd-${cmd.id}`}
                          className="event-item group relative flex items-start gap-2.5 py-1.5 px-1.5 rounded-md hover:bg-gray-50 transition-colors"
                          style={{ animationDelay: `${index * 30}ms` }}
                        >
                          <div className="relative flex-shrink-0 mt-px">
                            {cmd.status === 'pending' ||
                            cmd.status === 'failed' ||
                            cmd.status === 'expired' ||
                            cmd.status === 'cancelled' ? (
                              <CommandStatusBadge status={cmd.status} />
                            ) : (
                              <div
                                className={`w-6 h-6 rounded-full ${visual.bgColor} flex items-center justify-center`}
                              >
                                <Icon className={`w-3 h-3 ${visual.color}`} />
                              </div>
                            )}
                            {!isLast && (
                              <div className="absolute top-7 left-1/2 -translate-x-px w-px h-2.5 bg-gray-200" />
                            )}
                          </div>
                          <div className="flex-1 min-w-0">
                            <div className="flex items-start justify-between gap-2 mb-px">
                              <span className="text-sm font-medium text-gray-900">
                                {commandLabel(cmd)}
                                <span className="text-gray-400">
                                  {commandStatusSuffix(cmd)}
                                </span>
                              </span>
                              <CopyableTimestamp timestamp={cmd.created_at} />
                            </div>
                          </div>
                        </div>
                      );
                    }

                    if (item.kind === 'collapsed') {
                      const first = item.events[0];
                      const last = item.events[item.events.length - 1];
                      const olderTs = Math.min(first.timestamp, last.timestamp);
                      const newerTs = Math.max(first.timestamp, last.timestamp);
                      const runKey = `collapsed-${olderTs}-${newerTs}-${index}`;
                      const expanded = expandedRuns.has(runKey);
                      const visual = EVENT_VISUALS[EventType.WAKE] ?? DEFAULT_VISUAL;
                      const Icon = visual.icon;
                      return (
                        <div key={runKey}>
                          <button
                            type="button"
                            onClick={() => toggleRun(runKey)}
                            className="event-item group relative flex items-start gap-2.5 py-1.5 px-1.5 rounded-md hover:bg-gray-50 transition-colors w-full text-left"
                            style={{ animationDelay: `${index * 30}ms` }}
                            aria-expanded={expanded}
                          >
                            <div className="relative flex-shrink-0 mt-px">
                              <div
                                className={`w-6 h-6 rounded-full ${visual.bgColor} flex items-center justify-center`}
                              >
                                <Icon className={`w-3 h-3 ${visual.color}`} />
                              </div>
                              {!isLast && !expanded && (
                                <div className="absolute top-7 left-1/2 -translate-x-px w-px h-2.5 bg-gray-200" />
                              )}
                            </div>

                            <div className="flex-1 min-w-0">
                              <div className="flex items-start justify-between gap-2 mb-px">
                                <span className="text-sm font-medium text-gray-500 flex items-center gap-1">
                                  {formatCollapsedLabel(item.events)}
                                  <span className="text-xs text-gray-400">
                                    {expanded ? '▾' : '▸'}
                                  </span>
                                </span>
                                <span
                                  className="text-xs text-gray-400 font-mono"
                                  title={`${new Date(olderTs * 1000).toISOString()} – ${new Date(
                                    newerTs * 1000
                                  ).toISOString()}`}
                                >
                                  {format(new Date(olderTs * 1000), 'HH:mm:ss')} –{' '}
                                  {format(new Date(newerTs * 1000), 'HH:mm:ss')}
                                </span>
                              </div>
                            </div>
                          </button>

                          <AnimatePresence initial={false}>
                            {expanded && (
                              <motion.div
                                initial={{ opacity: 0, height: 0 }}
                                animate={{ opacity: 1, height: 'auto' }}
                                exit={{ opacity: 0, height: 0 }}
                                transition={{ duration: 0.18 }}
                                className="overflow-hidden"
                              >
                                {item.events.map((event, subIndex) => {
                                  const subVisual =
                                    EVENT_VISUALS[event.event_code] ?? DEFAULT_VISUAL;
                                  const SubIcon = subVisual.icon;
                                  const isLastInRun = subIndex === item.events.length - 1;
                                  return (
                                    <div
                                      key={`${runKey}-${event.timestamp}-${subIndex}`}
                                      className="event-item group relative flex items-start gap-2.5 py-1 pl-7 pr-1.5 rounded-md hover:bg-gray-50 transition-colors"
                                    >
                                      <div className="relative flex-shrink-0 mt-px">
                                        <div
                                          className={`w-5 h-5 rounded-full ${subVisual.bgColor} flex items-center justify-center`}
                                        >
                                          <SubIcon className={`w-2.5 h-2.5 ${subVisual.color}`} />
                                        </div>
                                        {(!isLast || !isLastInRun) && (
                                          <div className="absolute top-6 left-1/2 -translate-x-px w-px h-1.5 bg-gray-200" />
                                        )}
                                      </div>
                                      <div className="flex-1 min-w-0">
                                        <div className="flex items-start justify-between gap-2 mb-px">
                                          <span className="text-xs text-gray-600">
                                            {getEventName(event.event_code)}
                                            {eventLabelSuffix(event, durationByOpenTs)}
                                          </span>
                                          <CopyableTimestamp timestamp={event.timestamp} />
                                        </div>
                                      </div>
                                    </div>
                                  );
                                })}
                              </motion.div>
                            )}
                          </AnimatePresence>
                        </div>
                      );
                    }

                    const event = item.event;
                    const visual = EVENT_VISUALS[event.event_code] ?? DEFAULT_VISUAL;
                    const Icon = visual.icon;
                    return (
                      <div
                        key={`${event.timestamp}-${event.event_code}-${index}`}
                        className="event-item group relative flex items-start gap-2.5 py-1.5 px-1.5 rounded-md hover:bg-gray-50 transition-colors"
                        style={{ animationDelay: `${index * 30}ms` }}
                      >
                        <div className="relative flex-shrink-0 mt-px">
                          <div
                            className={`w-6 h-6 rounded-full ${visual.bgColor} flex items-center justify-center`}
                          >
                            <Icon className={`w-3 h-3 ${visual.color}`} />
                          </div>
                          {!isLast && (
                            <div className="absolute top-7 left-1/2 -translate-x-px w-px h-2.5 bg-gray-200" />
                          )}
                        </div>

                        <div className="flex-1 min-w-0">
                          <div className="flex items-start justify-between gap-2 mb-px">
                            <span className="text-sm font-medium text-gray-900">
                              {getEventName(event.event_code)}
                              {eventLabelSuffix(event, durationByOpenTs)}
                            </span>
                            <CopyableTimestamp timestamp={event.timestamp} />
                          </div>
                        </div>
                      </div>
                    );
                  });
                })()}
              </div>
            </div>
          ))}

          {onLoadMore && hasMore && (
            <div className="pt-2 pb-1 flex justify-center">
              <button
                type="button"
                onClick={onLoadMore}
                disabled={loadingMore}
                className="text-xs text-gray-600 hover:text-gray-900 disabled:opacity-50 px-3 py-1 rounded-md hover:bg-gray-100 transition-colors"
              >
                {loadingMore ? 'Loading…' : 'Load older'}
              </button>
            </div>
          )}
        </div>
      )}
    </div>
  );
}
