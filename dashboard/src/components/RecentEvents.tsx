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
} from 'lucide-react';
import { format, isToday, isYesterday } from 'date-fns';
import { motion, AnimatePresence } from 'motion/react';
import { useState } from 'react';
import type { NodeEvent } from '../types';
import { getEventName, EventType, EventCode } from '../types';

interface RecentEventsProps {
  events: NodeEvent[];
  loading: boolean;
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
  [EventType.SLEEP_ENTER]: { icon: Moon, color: 'text-gray-600', bgColor: 'bg-gray-50' },
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
  [EventCode.EVENT_CURTAIN_OPENED]: {
    icon: ArrowUpFromLine,
    color: 'text-emerald-600',
    bgColor: 'bg-emerald-50',
  },
  [EventCode.EVENT_CURTAIN_CLOSED]: {
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

export function RecentEvents({ events, loading }: RecentEventsProps) {
  const groupedEvents = events.reduce(
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
      ) : events.length === 0 ? (
        <div className="px-5 py-4">
          <p className="text-sm text-gray-400">No events recorded yet.</p>
        </div>
      ) : (
        <div className="px-4 py-2.5 max-h-[600px] overflow-y-auto">
          {Object.entries(groupedEvents).map(([day, dayEvents], dayIndex) => (
            <div key={day}>
              {dayIndex > 0 && <div className="h-px bg-gray-200 my-2.5" />}

              <div className="text-xs font-medium text-gray-500 mb-1.5 px-1">{day}</div>

              <div>
                <AnimatePresence initial={false}>
                  {dayEvents.map((event, index) => {
                    const visual = EVENT_VISUALS[event.event_code] ?? DEFAULT_VISUAL;
                    const Icon = visual.icon;
                    return (
                      <motion.div
                        key={`${event.timestamp}-${event.event_code}-${index}`}
                        layout
                        initial={{ opacity: 0, y: -12, scale: 0.97 }}
                        animate={{ opacity: 1, y: 0, scale: 1 }}
                        exit={{ opacity: 0, scale: 0.97 }}
                        transition={{ duration: 0.25, ease: 'easeOut' }}
                        className="group relative flex items-start gap-2.5 py-1.5 px-1.5 rounded-md hover:bg-gray-50 transition-colors"
                      >
                        <div className="relative flex-shrink-0 mt-px">
                          <div
                            className={`w-6 h-6 rounded-full ${visual.bgColor} flex items-center justify-center`}
                          >
                            <Icon className={`w-3 h-3 ${visual.color}`} />
                          </div>
                          {index < dayEvents.length - 1 && (
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
                      </motion.div>
                    );
                  })}
                </AnimatePresence>
              </div>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
