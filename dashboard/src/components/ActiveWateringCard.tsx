// Prominent status card for valves that are open right now. Sits above the
// irrigation controls so an in-progress run is the first thing on the page,
// with a live countdown and a direct stop action.
import { useState } from 'react';
import { motion, AnimatePresence } from 'motion/react';
import { Loader2 } from 'lucide-react';
import { formatDuration } from '../types';
import type { ActiveRun } from '../utils/valveRuns';
import { elapsedSecondsFor, remainingSecondsFor } from '../utils/valveRuns';

interface ActiveWateringCardProps {
  runs: ActiveRun[];
  now: number;
  valveNames?: Record<string, string>; // friendly names keyed by 0-based valve index
  onStop: (valve: number) => Promise<void>;
}

function valveLabel(index: number, valveNames?: Record<string, string>): string {
  return valveNames?.[String(index)] ?? `Valve ${index + 1}`;
}

// Countdown text — mirrors the timeline wording so both read the same.
function statusText(run: ActiveRun, now: number): string {
  const remainingSeconds = remainingSecondsFor(run, now);
  if (remainingSeconds === null) {
    return `${formatDuration(elapsedSecondsFor(run, now))} elapsed`;
  }
  return remainingSeconds > 0
    ? `${formatDuration(remainingSeconds)} left`
    : `past ${formatDuration(run.plannedSeconds ?? 0)} — awaiting close`;
}

function ActiveWateringRow({
  run,
  now,
  valveNames,
  onStop,
}: {
  run: ActiveRun;
  now: number;
  valveNames?: Record<string, string>;
  onStop: (valve: number) => Promise<void>;
}) {
  const [stopping, setStopping] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const remainingSeconds = remainingSecondsFor(run, now);
  const elapsedSeconds = elapsedSecondsFor(run, now);
  const progressPercent =
    run.plannedSeconds !== null && run.plannedSeconds > 0
      ? Math.min(100, (elapsedSeconds / run.plannedSeconds) * 100)
      : null;
  const overdue = remainingSeconds !== null && remainingSeconds <= 0;

  const handleStop = async () => {
    setStopping(true);
    setError(null);
    try {
      await onStop(run.valveId);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to send stop command');
    } finally {
      setStopping(false);
    }
  };

  return (
    <div>
      <div className="flex items-center gap-2.5">
        <span className="relative flex h-2 w-2 flex-shrink-0">
          <span className="absolute inline-flex h-full w-full rounded-full bg-emerald-400 opacity-75 animate-ping" />
          <span className="relative inline-flex h-2 w-2 rounded-full bg-emerald-500" />
        </span>
        <span className="flex-1 min-w-0 truncate text-sm font-semibold text-emerald-800">
          {valveLabel(run.valveId, valveNames)} watering
        </span>
        <span
          className={`text-xs font-mono font-medium flex-shrink-0 ${
            overdue ? 'text-amber-700' : 'text-emerald-600'
          }`}
        >
          {statusText(run, now)}
        </span>
        <button
          type="button"
          onClick={handleStop}
          disabled={stopping}
          className="flex-shrink-0 flex items-center gap-1 px-2.5 py-1 rounded-md border border-red-300 bg-white text-xs font-semibold text-red-600 hover:bg-red-50 disabled:opacity-50 transition-colors"
        >
          {stopping && <Loader2 className="w-3 h-3 animate-spin" />}
          Stop
        </button>
      </div>

      {progressPercent !== null && (
        <div className="mt-2 h-1 rounded-full bg-emerald-100 overflow-hidden">
          <motion.div
            className={`h-full rounded-full ${overdue ? 'bg-amber-500' : 'bg-emerald-500'}`}
            animate={{ width: `${progressPercent}%` }}
            transition={{ duration: 0.4, ease: 'linear' }}
          />
        </div>
      )}

      {error && <p className="mt-1.5 text-xs text-red-600">{error}</p>}
    </div>
  );
}

export function ActiveWateringCard({ runs, now, valveNames, onStop }: ActiveWateringCardProps) {
  return (
    <AnimatePresence>
      {runs.length > 0 && (
        <motion.div
          key="active-watering"
          initial={{ opacity: 0, height: 0 }}
          animate={{ opacity: 1, height: 'auto' }}
          exit={{ opacity: 0, height: 0 }}
          transition={{ duration: 0.25 }}
          className="overflow-hidden"
        >
          <div className="rounded-xl border border-emerald-200 bg-emerald-50 px-4 py-3.5 space-y-3">
            {runs.map((run) => (
              <ActiveWateringRow
                key={run.openTimestamp}
                run={run}
                now={now}
                valveNames={valveNames}
                onStop={onStop}
              />
            ))}
          </div>
        </motion.div>
      )}
    </AnimatePresence>
  );
}
