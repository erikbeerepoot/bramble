import { CheckCircle2, AlertCircle, Clock, Trash2, Plus, ArrowRight } from 'lucide-react';
import type { IrrigationSchedule } from '../types';
import { formatDays } from '../types';

interface SchedulesListProps {
  schedules: IrrigationSchedule[];
  loading: boolean;
  deletingIndex: number | null;
  onDelete: (index: number) => void;
  onAdd: () => void;
  valveLabel?: (valve: number) => string;  // Friendly name for a valve index
}

function formatTime(hour: number, minute: number): string {
  return `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`;
}

function getEndTime(hour: number, minute: number, durationSeconds: number): string {
  const totalMinutes = hour * 60 + minute + Math.floor(durationSeconds / 60);
  const endHours = Math.floor(totalMinutes / 60) % 24;
  const endMinutes = totalMinutes % 60;
  return `${String(endHours).padStart(2, '0')}:${String(endMinutes).padStart(2, '0')}`;
}

function getStatusStyle(status: IrrigationSchedule['status']) {
  if (status === 'failed') {
    return {
      bg: 'bg-red-50',
      border: 'border-red-300',
      icon: <AlertCircle className="w-3.5 h-3.5 text-red-600" />,
      iconBg: 'bg-red-100',
    };
  }
  if (status === 'pending') {
    return {
      bg: 'bg-amber-50',
      border: 'border-amber-300',
      icon: (
        <div className="relative">
          <Clock className="w-3.5 h-3.5 text-amber-600" />
          <div className="absolute inset-0 bg-amber-400 rounded-full animate-ping opacity-30" />
        </div>
      ),
      iconBg: 'bg-amber-100',
    };
  }
  // confirmed or undefined — treat as confirmed
  return {
    bg: 'bg-white',
    border: 'border-gray-200',
    icon: <CheckCircle2 className="w-3.5 h-3.5 text-emerald-500" />,
    iconBg: 'bg-emerald-50',
  };
}

export function SchedulesList({
  schedules,
  loading,
  deletingIndex,
  onDelete,
  onAdd,
  valveLabel,
}: SchedulesListProps) {
  const groupedSchedules = schedules.reduce(
    (acc, schedule) => {
      if (!acc[schedule.valve]) {
        acc[schedule.valve] = [];
      }
      acc[schedule.valve].push(schedule);
      return acc;
    },
    {} as Record<number, IrrigationSchedule[]>
  );

  return (
    <div className="bg-white rounded-xl border border-gray-200 shadow-sm">
      <div className="px-5 py-4 border-b border-gray-200 flex items-center justify-between">
        <div>
          <h2 className="font-semibold text-gray-900">Schedules</h2>
          <p className="text-xs text-gray-500 mt-0.5">{schedules.length} scheduled</p>
        </div>
        <button
          onClick={onAdd}
          className="px-3 py-1.5 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors flex items-center gap-1.5 text-sm font-medium shadow-sm"
        >
          <Plus className="w-4 h-4" />
          Add Schedule
        </button>
      </div>

      {loading ? (
        <div className="p-4 animate-pulse space-y-2">
          <div className="h-4 bg-gray-200 rounded w-3/4" />
          <div className="h-4 bg-gray-200 rounded w-1/2" />
        </div>
      ) : schedules.length === 0 ? (
        <div className="p-5">
          <p className="text-sm text-gray-400">No schedules configured.</p>
        </div>
      ) : (
        <div className="p-4 space-y-3">
          {Object.entries(groupedSchedules)
            .sort(([a], [b]) => Number(a) - Number(b))
            .map(([valve, valveSchedules]) => (
              <div key={valve}>
                <div className="text-xs font-medium text-gray-500 mb-1.5 px-1">
                  {valveLabel ? valveLabel(Number(valve)) : `Valve ${Number(valve) + 1}`}
                </div>
                <div className="space-y-1">
                  {valveSchedules.map((schedule) => {
                    const style = getStatusStyle(schedule.status);
                    const isDeleting = deletingIndex === schedule.index;
                    return (
                      <div
                        key={schedule.index}
                        className={`group relative ${style.bg} ${style.border} border rounded-lg p-2.5 transition-all`}
                      >
                        <div className="flex items-center gap-2.5">
                          <div className={`${style.iconBg} rounded-md p-1.5 flex-shrink-0`}>
                            {style.icon}
                          </div>

                          <div className="flex-1 min-w-0 flex items-center gap-2 flex-wrap">
                            <div className="flex items-center gap-1.5">
                              <span className="font-mono font-semibold text-sm text-gray-900">
                                {formatTime(schedule.hour, schedule.minute)}
                              </span>
                              <ArrowRight className="w-3.5 h-3.5 text-gray-400 flex-shrink-0" />
                              <span className="font-mono font-semibold text-sm text-gray-900">
                                {getEndTime(schedule.hour, schedule.minute, schedule.duration)}
                              </span>
                            </div>

                            <div className="h-3 w-px bg-gray-300 flex-shrink-0" />

                            <span className="text-xs font-medium text-gray-700 whitespace-nowrap">
                              {formatDays(schedule.days)}
                            </span>

                            <span className="text-xs text-gray-500">
                              · {Math.round(schedule.duration / 60)}min
                            </span>
                          </div>

                          <button
                            onClick={() => onDelete(schedule.index)}
                            disabled={deletingIndex !== null}
                            className="opacity-60 hover:opacity-100 transition-opacity p-1.5 hover:bg-red-50 rounded text-gray-400 hover:text-red-600 flex-shrink-0 disabled:opacity-30"
                            aria-label="Delete schedule"
                            title={isDeleting ? 'Removing...' : 'Delete schedule'}
                          >
                            <Trash2 className="w-3.5 h-3.5" />
                          </button>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>
            ))}
        </div>
      )}
    </div>
  );
}
