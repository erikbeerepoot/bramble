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
} from 'lucide-react';
import { format, isToday, isYesterday } from 'date-fns';
import { useState } from 'react';
import { motion, AnimatePresence } from 'motion/react';
import type { NodeEvent } from '../types';
import { getEventName, EventType, EventCode } from '../types';
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
  loading: boolean;
  pendingEvent?: PendingEvent | null;
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
      <span>{format(date, 'HH:mm')}</span>
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

// Event codes that collapse into summary rows (per-wake-cycle noise).
const WAKE_CYCLE_CODES = new Set<number>([EventType.WAKE, EventType.SLEEP_ENTER]);

type RenderItem =
  | { kind: 'event'; event: NodeEvent }
  | {
      kind: 'collapsed';
      events: NodeEvent[]; // consecutive run of WAKE/SLEEP_ENTER
    };

function collapseWakeCycles(dayEvents: NodeEvent[]): RenderItem[] {
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
  for (const event of dayEvents) {
    if (WAKE_CYCLE_CODES.has(event.event_code)) {
      run.push(event);
    } else {
      flush();
      items.push({ kind: 'event', event });
    }
  }
  flush();
  return items;
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

export function RecentEvents({ events, loading, pendingEvent }: RecentEventsProps) {
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

  const groupedEvents = filteredEvents.reduce(
    (acc, event) => {
      const date = new Date(event.timestamp * 1000);
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
      acc[dayLabel].push(event);
      return acc;
    },
    {} as Record<string, NodeEvent[]>
  );

  // Ensure "Today" group exists if we have a pending event
  if (pendingEvent && !groupedEvents['Today']) {
    groupedEvents['Today'] = [];
  }

  // Put "Today" first when we have a pending event
  const dayEntries = Object.entries(groupedEvents);
  if (pendingEvent) {
    const todayIdx = dayEntries.findIndex(([day]) => day === 'Today');
    if (todayIdx > 0) {
      const [todayEntry] = dayEntries.splice(todayIdx, 1);
      dayEntries.unshift(todayEntry);
    }
  }

  return (
    <div className="bg-white rounded-xl border border-gray-200 shadow-sm">
      <div className="px-5 py-4 border-b border-gray-200">
        <h2 className="font-semibold text-gray-900">Recent Events</h2>
        <p className="text-xs text-gray-500 mt-0.5">{events.length} events logged</p>
      </div>

      {loading ? (
        <div className="p-4 animate-pulse space-y-2">
          <div className="h-4 bg-gray-200 rounded w-3/4" />
          <div className="h-4 bg-gray-200 rounded w-1/2" />
        </div>
      ) : events.length === 0 && !pendingEvent ? (
        <div className="px-5 py-4">
          <p className="text-sm text-gray-400">No events recorded yet.</p>
        </div>
      ) : (
        <div className="px-4 py-2.5 max-h-[600px] overflow-y-auto">
          {dayEntries.map(([day, dayEvents], dayIndex) => (
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
                          {dayEvents.length > 0 && (
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
                  const renderItems = collapseWakeCycles(dayEvents);
                  return renderItems.map((item, index) => {
                    const adjustedIndex =
                      pendingEvent && day === 'Today' ? index + 1 : index;
                    const totalItems =
                      renderItems.length + (pendingEvent && day === 'Today' ? 1 : 0);
                    const isLast = adjustedIndex >= totalItems - 1;

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
                                  {format(new Date(olderTs * 1000), 'HH:mm')} –{' '}
                                  {format(new Date(newerTs * 1000), 'HH:mm')}
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
        </div>
      )}
    </div>
  );
}
