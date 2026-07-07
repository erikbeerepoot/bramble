import { useState, useEffect, useCallback } from 'react';
import { Zap, Play, Square, Check, X, Pencil } from 'lucide-react';
import { motion, AnimatePresence } from 'motion/react';
import {
  runValve,
  stopValve,
  getIrrigationSchedules,
  addIrrigationSchedule,
  deleteIrrigationSchedule,
  getValveGroups,
  updateValveMetadata,
} from '../api/client';
import type { IrrigationSchedule, ValveGroup } from '../types';
import { DAY_LABELS } from '../types';
import { REFRESH_INTERVAL_MS } from '../config';
import { SchedulesList } from './SchedulesList';

type ValveActionState = 'idle' | 'sending' | 'queued' | 'error';

interface ValveState {
  state: ValveActionState;
  action?: 'run' | 'stop';
  errorMessage?: string;
}

interface IrrigationControlProps {
  deviceId: string;
  valveCount: number;
  valveNames?: Record<string, string>;  // Friendly names keyed by valve index
  onEventTriggered?: () => void;
}

// On-node schedule slots (must match PMU MAX_SCHEDULE_ENTRIES). The top slot is
// reserved for one-shot "run now"; persistent schedules use 0..MAX_SCHEDULE_SLOTS-2.
const MAX_SCHEDULE_SLOTS = 100;

// Non-linear notches: fine-grained at short durations, coarser for long runs
const DURATION_NOTCHES = [1, 5, 10, 15, 20, 30, 45, 60, 90, 120]; // minutes

const DURATION_LABELS: Record<number, string> = {
  1: '1',
  5: '5',
  10: '10',
  15: '15',
  20: '20',
  30: '30',
  45: '45',
  60: '1h',
  90: '1.5h',
  120: '2h',
};

function formatDurationShort(minutes: number): string {
  if (minutes >= 60) {
    return minutes === 60 ? '1h' : minutes === 90 ? '1.5h' : '2h';
  }
  return `${minutes}m`;
}

function IrrigationControl({ deviceId, valveCount, valveNames, onEventTriggered }: IrrigationControlProps) {
  // Valve indices [0..valveCount-1], driving every valve-dependent UI section
  const valveIndices = Array.from({ length: valveCount }, (_, i) => i);

  // Per-valve friendly names: server values (valveNames prop) with local
  // optimistic overrides applied after a rename so the UI updates immediately.
  const [valveNameOverrides, setValveNameOverrides] = useState<Record<string, string>>({});
  const [editingValve, setEditingValve] = useState<number | null>(null);
  const [editingName, setEditingName] = useState('');
  const [savingValveName, setSavingValveName] = useState(false);

  const valveLabel = useCallback(
    (valve: number): string => {
      const name = valveNameOverrides[String(valve)] ?? valveNames?.[String(valve)];
      return name && name.trim() ? name : `Valve ${valve + 1}`;
    },
    [valveNameOverrides, valveNames]
  );

  const startEditingValve = useCallback(
    (valve: number) => {
      const current = valveLabel(valve);
      // Prefill the custom name only; leave blank when it's the default label.
      setEditingName(current === `Valve ${valve + 1}` ? '' : current);
      setEditingValve(valve);
    },
    [valveLabel]
  );

  const handleSaveValveName = useCallback(
    async (valve: number) => {
      const name = editingName.trim();
      setSavingValveName(true);
      try {
        await updateValveMetadata(deviceId, valve, name);
        setValveNameOverrides((prev) => ({ ...prev, [String(valve)]: name }));
        setEditingValve(null);
      } catch {
        // Leave the editor open so the user can retry.
      } finally {
        setSavingValveName(false);
      }
    },
    [deviceId, editingName]
  );

  // Run-once state
  const [selectedDuration, setSelectedDuration] = useState<Record<number, number>>(() =>
    Object.fromEntries(valveIndices.map((v) => [v, 300]))
  );
  const [valveStates, setValveStates] = useState<Record<number, ValveState>>(() =>
    Object.fromEntries(valveIndices.map((v) => [v, { state: 'idle' as const }]))
  );

  const setValveState = useCallback((valve: number, next: ValveState) => {
    setValveStates((prev) => ({ ...prev, [valve]: next }));
  }, []);

  // Valve groups this node participates in (read-only badges)
  const [valveGroups, setValveGroups] = useState<ValveGroup[]>([]);

  // Schedule state
  const [schedules, setSchedules] = useState<IrrigationSchedule[]>([]);
  const [loadingSchedules, setLoadingSchedules] = useState(true);
  const [showAddForm, setShowAddForm] = useState(false);
  const [scheduleError, setScheduleError] = useState<string | null>(null);
  const [deletingIndex, setDeletingIndex] = useState<number | null>(null);

  // Schedule form state
  const [formValve, setFormValve] = useState(0);
  const [formHour, setFormHour] = useState(6);
  const [formMinute, setFormMinute] = useState(0);
  const [formDurationMin, setFormDurationMin] = useState(15);
  const [formDays, setFormDays] = useState(127);
  const [savingSchedule, setSavingSchedule] = useState(false);

  const fetchSchedules = useCallback(async () => {
    try {
      const response = await getIrrigationSchedules(deviceId);
      setSchedules(response.schedules);
    } catch {
      // Silently fail
    } finally {
      setLoadingSchedules(false);
    }
  }, [deviceId]);

  useEffect(() => {
    fetchSchedules();
    const interval = setInterval(fetchSchedules, REFRESH_INTERVAL_MS);
    return () => clearInterval(interval);
  }, [fetchSchedules]);

  // Load valve groups once to annotate valves that participate in a group.
  useEffect(() => {
    let cancelled = false;
    getValveGroups()
      .then((response) => {
        if (!cancelled) setValveGroups(response.groups);
      })
      .catch(() => {
        // Badges are non-essential; ignore failures.
      });
    return () => {
      cancelled = true;
    };
  }, []);

  // Returns a badge descriptor for a given valve on this node, or null if the
  // valve isn't part of any group.
  const valveGroupBadge = (
    valve: number
  ): { label: string; tone: 'master' | 'member' } | null => {
    for (const group of valveGroups) {
      if (group.master_device_id === deviceId && group.master_valve === valve) {
        return { label: `Master · ${group.name}`, tone: 'master' };
      }
      const member = group.members.find(
        (m) => m.zone_device_id === deviceId && m.zone_valve === valve
      );
      if (member) {
        return { label: `In group: ${group.name}`, tone: 'member' };
      }
    }
    return null;
  };

  const handleRun = async (valve: number) => {
    setValveState(valve, { state: 'sending', action: 'run' });
    try {
      const duration = selectedDuration[valve] || 300;
      await runValve(deviceId, valve, duration);
      setValveState(valve, { state: 'queued', action: 'run' });
      // Refresh events soon to pick up Valve Open / Valve Timer Set.
      if (onEventTriggered) setTimeout(onEventTriggered, 3000);
      // Return to idle after the Queued checkmark has been visible.
      setTimeout(() => {
        setValveStates((prev) =>
          prev[valve].state === 'queued' ? { ...prev, [valve]: { state: 'idle' } } : prev
        );
      }, 2000);
    } catch (err) {
      setValveState(valve, {
        state: 'error',
        action: 'run',
        errorMessage: err instanceof Error ? err.message : 'Failed to send run command',
      });
    }
  };

  const handleStop = async (valve: number) => {
    setValveState(valve, { state: 'sending', action: 'stop' });
    try {
      await stopValve(deviceId, valve);
      setValveState(valve, { state: 'idle' });
      if (onEventTriggered) setTimeout(onEventTriggered, 3000);
    } catch (err) {
      setValveState(valve, {
        state: 'error',
        action: 'stop',
        errorMessage: err instanceof Error ? err.message : 'Failed to send stop command',
      });
    }
  };

  const dismissValveError = (valve: number) => {
    setValveState(valve, { state: 'idle' });
  };

  const handleAddSchedule = async () => {
    setSavingSchedule(true);
    setScheduleError(null);
    try {
      // Find next available index
      const usedIndices = new Set(schedules.map((s) => s.index));
      let nextIndex = -1;
      // Reserve the top slot for run-once; persistent schedules use 0..MAX-2.
      for (let i = 0; i < MAX_SCHEDULE_SLOTS - 1; i++) {
        if (!usedIndices.has(i)) {
          nextIndex = i;
          break;
        }
      }
      if (nextIndex === -1) {
        setScheduleError(`Maximum schedules reached (${MAX_SCHEDULE_SLOTS - 1})`);
        setSavingSchedule(false);
        return;
      }

      // API client converts local→UTC for the PMU
      await addIrrigationSchedule(deviceId, {
        index: nextIndex,
        hour: formHour,
        minute: formMinute,
        duration: formDurationMin * 60,
        days: formDays,
        valve: formValve,
      });

      setShowAddForm(false);
      setFormValve(0);
      setFormHour(6);
      setFormMinute(0);
      setFormDurationMin(15);
      setFormDays(127);
      await fetchSchedules();
    } catch (err) {
      setScheduleError(err instanceof Error ? err.message : 'Failed to add schedule');
    } finally {
      setSavingSchedule(false);
    }
  };

  const handleDeleteSchedule = async (index: number) => {
    setDeletingIndex(index);
    setScheduleError(null);
    try {
      await deleteIrrigationSchedule(deviceId, index);
      await fetchSchedules();
    } catch (err) {
      setScheduleError(err instanceof Error ? err.message : 'Failed to delete schedule');
    } finally {
      setDeletingIndex(null);
    }
  };

  const toggleDay = (bit: number) => {
    setFormDays((prev) => prev ^ (1 << bit));
  };

  const renderValveRow = (valve: number) => {
    const valveState = valveStates[valve];
    const duration = selectedDuration[valve] || DURATION_NOTCHES[0] * 60;
    const minutes = duration / 60;
    const isBusy = valveState.state === 'sending' || valveState.state === 'queued';
    const isRunSending = valveState.state === 'sending' && valveState.action === 'run';
    const isStopSending = valveState.state === 'sending' && valveState.action === 'stop';

    const groupBadge = valveGroupBadge(valve);

    return (
      <div key={valve} className="space-y-3">
        {/* Header */}
        <div className="flex items-center gap-2 flex-wrap">
          {editingValve === valve ? (
            <div className="flex items-center gap-1.5">
              <input
                autoFocus
                value={editingName}
                onChange={(e) => setEditingName(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') handleSaveValveName(valve);
                  if (e.key === 'Escape') setEditingValve(null);
                }}
                maxLength={40}
                placeholder={`Valve ${valve + 1}`}
                className="px-2 py-1 text-sm border border-gray-300 rounded-md focus:outline-none focus:ring-2 focus:ring-blue-500"
              />
              <button
                onClick={() => handleSaveValveName(valve)}
                disabled={savingValveName}
                className="p-1 text-green-600 hover:bg-green-50 rounded disabled:opacity-50"
                title="Save name"
              >
                <Check className="w-4 h-4" />
              </button>
              <button
                onClick={() => setEditingValve(null)}
                className="p-1 text-gray-400 hover:bg-gray-100 rounded"
                title="Cancel"
              >
                <X className="w-4 h-4" />
              </button>
            </div>
          ) : (
            <>
              <h3 className="font-medium text-gray-900">{valveLabel(valve)}</h3>
              <button
                onClick={() => startEditingValve(valve)}
                className="p-1 text-gray-400 hover:text-gray-600 hover:bg-gray-100 rounded"
                title="Rename valve"
              >
                <Pencil className="w-3.5 h-3.5" />
              </button>
            </>
          )}
          {groupBadge && (
            <span
              className={`inline-flex items-center px-2 py-0.5 rounded-full text-xs font-medium ${
                groupBadge.tone === 'master'
                  ? 'bg-amber-100 text-amber-800'
                  : 'bg-blue-100 text-blue-800'
              }`}
              title={
                groupBadge.tone === 'master'
                  ? 'This valve is the master shutoff for a valve group.'
                  : 'This valve is a zone member of a valve group.'
              }
            >
              {groupBadge.label}
            </span>
          )}
        </div>

        {/* Duration chips */}
        <div className="overflow-x-auto scrollbar-hide">
          <div className="flex gap-2 min-w-min pb-1">
            {DURATION_NOTCHES.map((m) => (
              <button
                key={m}
                onClick={() => setSelectedDuration((prev) => ({ ...prev, [valve]: m * 60 }))}
                className={`flex-shrink-0 px-4 h-10 rounded-full text-sm font-medium transition-colors ${
                  m === minutes
                    ? 'bg-green-600 text-white'
                    : 'bg-gray-100 text-gray-700 hover:bg-gray-200 active:bg-gray-300'
                }`}
              >
                {DURATION_LABELS[m] || m}
              </button>
            ))}
          </div>
        </div>

        {/* Action buttons */}
        <div className="flex gap-3">
          <motion.button
            onClick={() => handleRun(valve)}
            disabled={isBusy}
            className="relative flex-1 h-11 rounded-xl font-medium bg-green-600 text-white hover:bg-green-700 active:bg-green-800 transition-colors disabled:cursor-not-allowed disabled:opacity-90"
            animate={{ scale: valveState.state === 'queued' ? [1, 1.03, 1] : 1 }}
            transition={{ duration: 0.3 }}
          >
            <AnimatePresence mode="wait" initial={false}>
              {isRunSending ? (
                <motion.div
                  key="sending"
                  initial={{ opacity: 0, y: 8 }}
                  animate={{ opacity: 1, y: 0 }}
                  exit={{ opacity: 0, y: -8 }}
                  transition={{ duration: 0.18 }}
                  className="flex items-center justify-center gap-2"
                >
                  <motion.div
                    animate={{ rotate: 360 }}
                    transition={{ duration: 1, repeat: Infinity, ease: 'linear' }}
                  >
                    <Zap className="w-4 h-4" />
                  </motion.div>
                  <span>Sending...</span>
                </motion.div>
              ) : valveState.state === 'queued' ? (
                <motion.div
                  key="queued"
                  initial={{ opacity: 0, scale: 0.85 }}
                  animate={{ opacity: 1, scale: 1 }}
                  exit={{ opacity: 0, scale: 0.85 }}
                  transition={{ duration: 0.25, type: 'spring' }}
                  className="flex items-center justify-center gap-2"
                >
                  <Check className="w-5 h-5 stroke-[3]" />
                  <span>Queued</span>
                </motion.div>
              ) : (
                <motion.div
                  key="idle"
                  initial={{ opacity: 0, y: 8 }}
                  animate={{ opacity: 1, y: 0 }}
                  exit={{ opacity: 0, y: -8 }}
                  transition={{ duration: 0.18 }}
                  className="flex items-center justify-center gap-2"
                >
                  <Play className="w-4 h-4 fill-current" />
                  <span>Run {formatDurationShort(minutes)}</span>
                </motion.div>
              )}
            </AnimatePresence>
          </motion.button>
          <button
            onClick={() => handleStop(valve)}
            disabled={isBusy}
            className="flex-1 flex items-center justify-center gap-2 h-11 bg-red-100 text-red-700 rounded-xl font-medium hover:bg-red-200 active:bg-red-300 transition-colors disabled:opacity-50 disabled:cursor-not-allowed"
          >
            <Square className="w-4 h-4 fill-current" />
            <span>{isStopSending ? 'Stopping...' : 'Stop'}</span>
          </button>
        </div>

        {/* Persistent error — stays until dismissed */}
        <AnimatePresence>
          {valveState.state === 'error' && valveState.errorMessage && (
            <motion.div
              initial={{ opacity: 0, height: 0 }}
              animate={{ opacity: 1, height: 'auto' }}
              exit={{ opacity: 0, height: 0 }}
              transition={{ duration: 0.2 }}
              className="bg-red-50 border border-red-200 rounded-xl p-3 flex items-start justify-between gap-3 overflow-hidden"
            >
              <div className="flex items-start gap-2 flex-1 min-w-0">
                <X className="w-4 h-4 text-red-600 mt-0.5 flex-shrink-0" />
                <p className="text-sm text-red-700 break-words">{valveState.errorMessage}</p>
              </div>
              <button
                onClick={() => dismissValveError(valve)}
                className="text-red-400 hover:text-red-600 transition-colors flex-shrink-0"
                aria-label="Dismiss error"
              >
                <X className="w-4 h-4" />
              </button>
            </motion.div>
          )}
        </AnimatePresence>
      </div>
    );
  };

  return (
    <div className="space-y-4">
      {/* Run Once */}
      <div className="bg-white rounded-2xl shadow-md p-5">
        <div className="flex items-center gap-2 mb-5">
          <Zap className="w-5 h-5 text-gray-500" />
          <h2 className="font-semibold text-gray-900">Run Once</h2>
        </div>
        <div className="space-y-5">
          {valveIndices.map((v, i) => (
            <div key={v}>
              {i > 0 && <div className="border-t border-gray-200 mb-5" />}
              {renderValveRow(v)}
            </div>
          ))}
        </div>
      </div>

      {/* Schedules */}
      <div>
        <SchedulesList
          schedules={schedules}
          loading={loadingSchedules}
          deletingIndex={deletingIndex}
          onDelete={handleDeleteSchedule}
          onAdd={() => setShowAddForm(true)}
          valveLabel={valveLabel}
        />
        {scheduleError && <p className="text-sm text-red-600 mt-2 px-1">{scheduleError}</p>}
      </div>

      {/* Add Schedule Modal */}
      {showAddForm && (
        <div className="fixed inset-0 z-40 flex items-center justify-center p-4">
          <div className="fixed inset-0 bg-black/40" onClick={() => setShowAddForm(false)} />
          <div className="relative z-50 bg-white rounded-2xl shadow-2xl w-full max-w-md max-h-[85vh] overflow-y-auto">
            <div className="px-6 pb-8 pt-6">
              <h2 className="text-xl font-semibold text-gray-900 mb-6 text-center">New Schedule</h2>

              {/* Valve Selector */}
              <div className="mb-6">
                <label className="block text-sm font-medium text-gray-700 mb-2">Valve</label>
                <div className="flex bg-gray-100 rounded-lg p-1">
                  {valveIndices.map((v) => (
                    <button
                      key={v}
                      onClick={() => setFormValve(v)}
                      className={`flex-1 py-2 px-4 rounded-md text-sm font-medium transition-all ${
                        formValve === v ? 'bg-white text-gray-900 shadow-sm' : 'text-gray-600'
                      }`}
                    >
                      {valveLabel(v)}
                    </button>
                  ))}
                </div>
              </div>

              {/* Start Time — native input, renders as wheel on iOS */}
              <div className="mb-6">
                <label className="block text-sm font-medium text-gray-700 mb-2">Start Time</label>
                <input
                  type="time"
                  value={`${String(formHour).padStart(2, '0')}:${String(formMinute).padStart(2, '0')}`}
                  onChange={(e) => {
                    const [h, m] = e.target.value.split(':').map(Number);
                    setFormHour(h);
                    setFormMinute(m);
                  }}
                  className="w-full px-4 py-3 bg-gray-50 border border-gray-200 rounded-lg text-base focus:outline-none focus:ring-2 focus:ring-blue-500"
                />
              </div>

              {/* Duration */}
              <div className="mb-6">
                <label className="block text-sm font-medium text-gray-700 mb-2">Duration</label>
                <select
                  value={formDurationMin}
                  onChange={(e) => setFormDurationMin(parseInt(e.target.value, 10))}
                  className="w-full px-4 py-3 bg-gray-50 border border-gray-200 rounded-lg text-base focus:outline-none focus:ring-2 focus:ring-blue-500 appearance-none bg-[url('data:image/svg+xml;charset=utf-8,%3Csvg%20width%3D%2212%22%20height%3D%228%22%20viewBox%3D%220%200%2012%208%22%20fill%3D%22none%22%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%3E%3Cpath%20d%3D%22M1%201.5L6%206.5L11%201.5%22%20stroke%3D%22%236B7280%22%20stroke-width%3D%221.5%22%20stroke-linecap%3D%22round%22%20stroke-linejoin%3D%22round%22%2F%3E%3C%2Fsvg%3E')] bg-no-repeat bg-[position:right_1rem_center]"
                >
                  {DURATION_NOTCHES.map((m) => (
                    <option key={m} value={m}>
                      {m >= 60
                        ? `${Math.floor(m / 60)}h${m % 60 > 0 ? ` ${m % 60}m` : ''}`
                        : `${m} minutes`}
                    </option>
                  ))}
                </select>
              </div>

              {/* Days */}
              <div className="mb-8">
                <label className="block text-sm font-medium text-gray-700 mb-3">Days</label>
                <div className="flex justify-between gap-2">
                  {DAY_LABELS.map((day, i) => {
                    const isSelected = (formDays & (1 << i)) !== 0;
                    return (
                      <button
                        key={day}
                        onClick={() => toggleDay(i)}
                        className={`w-11 h-11 rounded-full font-medium text-sm transition-all ${
                          isSelected
                            ? 'bg-blue-600 text-white'
                            : 'bg-white border-2 border-gray-300 text-gray-700 active:bg-gray-50'
                        }`}
                      >
                        {day.charAt(0)}
                      </button>
                    );
                  })}
                </div>
              </div>

              {/* Actions */}
              <div className="space-y-3">
                <button
                  onClick={handleAddSchedule}
                  disabled={savingSchedule || formDays === 0}
                  className="w-full py-4 bg-green-600 text-white font-semibold rounded-xl active:bg-green-700 disabled:opacity-50 transition-colors"
                >
                  {savingSchedule ? 'Saving...' : 'Save Schedule'}
                </button>
                <button
                  onClick={() => setShowAddForm(false)}
                  className="w-full py-2 text-gray-600 font-medium active:text-gray-800 transition-colors"
                >
                  Cancel
                </button>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default IrrigationControl;
