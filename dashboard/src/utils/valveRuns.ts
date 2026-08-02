// Derives valve run state from the node event log. A run is a VALVE_OPEN event
// paired with the VALVE_CLOSE / VALVE_TIMER_CLOSE that follows it; opens with no
// close yet are either still running or stale (the close was never logged).
//
// Shared by the activity timeline and the active-watering card so both describe
// the same run the same way.
import { useEffect, useState } from 'react';
import type { NodeEvent, NodeCommand } from '../types';
import { parseEventDetail, formatDuration, EventType } from '../types';

// A VALVE_OPEN with no matching close yet — the valve is presumed still open.
export interface ActiveRun {
  valveId: number;
  openTimestamp: number; // unix milliseconds
  plannedSeconds: number | null; // requested run time, when the command is known
}

export interface ValveRuns {
  // OPEN event timestamp -> measured open→close time in seconds.
  durationByOpenTimestamp: Map<number, number>;
  // OPEN event timestamp -> run still in progress.
  activeRunByOpenTimestamp: Map<number, ActiveRun>;
  // OPEN event timestamps whose close never arrived and is now too old to trust.
  staleOpenTimestamps: Set<number>;
}

export interface ValvePairing {
  durationByOpenTimestamp: Map<number, number>;
  // Opens with no close yet, keyed by valve — may be running or simply stale.
  unpairedOpenByValve: Map<number, NodeEvent>;
  staleOpenTimestamps: Set<number>;
}

// How long past the requested run time we keep believing a missing close means
// "still open". Past this the close event was most likely never logged.
const RUN_GRACE_SECONDS = 5 * 60;
// Same idea for opens with no known requested duration (e.g. schedule-driven).
const UNKNOWN_RUN_CAP_SECONDS = 6 * 60 * 60;

// Window around a VALVE_OPEN event in which a valve_open command counts as the
// request that produced it.
const COMMAND_MATCH_BEFORE_SECONDS = 15 * 60;
const COMMAND_MATCH_AFTER_SECONDS = 2 * 60;

// Requested run time for an open, taken from the most recent valve_open command
// for the same valve issued just before it. Null when nothing plausibly matches
// (schedule-driven opens, or commands scrolled out of the loaded window).
function plannedSecondsForOpen(
  openTimestamp: number,
  valveId: number,
  commands: NodeCommand[]
): number | null {
  let bestCreatedAt = -Infinity;
  let bestSeconds: number | null = null;
  for (const command of commands) {
    if (command.command_type !== 'valve_open') continue;
    const params = command.params as Record<string, unknown>;
    if (params.valve !== valveId) continue;
    if (typeof params.duration_seconds !== 'number') continue;
    // command.created_at is unix seconds; event timestamps are milliseconds.
    const offsetSeconds = openTimestamp / 1000 - command.created_at;
    if (offsetSeconds < -COMMAND_MATCH_AFTER_SECONDS) continue;
    if (offsetSeconds > COMMAND_MATCH_BEFORE_SECONDS) continue;
    if (command.created_at > bestCreatedAt) {
      bestCreatedAt = command.created_at;
      bestSeconds = params.duration_seconds;
    }
  }
  return bestSeconds;
}

// Pair VALVE_OPEN events with the next matching VALVE_CLOSE / VALVE_TIMER_CLOSE
// (same valve_id) to derive a duration. Events arrive newest-first. Clock-free
// so callers can test for open runs without re-running on every tick.
export function pairValveEvents(events: NodeEvent[]): ValvePairing {
  const durationByOpenTimestamp = new Map<number, number>();
  const staleOpenTimestamps = new Set<number>();
  const openByValve = new Map<number, NodeEvent>();
  // Walk oldest -> newest so opens come before closes.
  for (let i = events.length - 1; i >= 0; i--) {
    const event = events[i];
    const valveId = parseEventDetail(event.data_hex);
    if (valveId === null) continue;
    if (event.event_code === EventType.VALVE_OPEN) {
      // A second open with no close in between means the first one's close was
      // lost — it can never resolve, so retire it as stale now.
      const previous = openByValve.get(valveId);
      if (previous) staleOpenTimestamps.add(previous.timestamp);
      openByValve.set(valveId, event);
    } else if (
      event.event_code === EventType.VALVE_CLOSE ||
      event.event_code === EventType.VALVE_TIMER_CLOSE
    ) {
      const open = openByValve.get(valveId);
      if (open) {
        // event.timestamp is unix milliseconds; formatDuration expects seconds.
        const duration = Math.round((event.timestamp - open.timestamp) / 1000);
        if (duration > 0) {
          durationByOpenTimestamp.set(open.timestamp, duration);
        }
        openByValve.delete(valveId);
      }
    }
  }

  return { durationByOpenTimestamp, unpairedOpenByValve: openByValve, staleOpenTimestamps };
}

// Split the unpaired opens into runs still in progress and opens whose close is
// now too overdue to still be pending.
export function classifyValveRuns(
  pairing: ValvePairing,
  commands: NodeCommand[],
  now: number
): ValveRuns {
  const activeRunByOpenTimestamp = new Map<number, ActiveRun>();
  const staleOpenTimestamps = new Set<number>(pairing.staleOpenTimestamps);

  for (const [valveId, open] of pairing.unpairedOpenByValve) {
    const plannedSeconds = plannedSecondsForOpen(open.timestamp, valveId, commands);
    const elapsedSeconds = Math.round((now - open.timestamp) / 1000);
    const limitSeconds =
      plannedSeconds !== null ? plannedSeconds + RUN_GRACE_SECONDS : UNKNOWN_RUN_CAP_SECONDS;
    if (elapsedSeconds < 0 || elapsedSeconds > limitSeconds) {
      staleOpenTimestamps.add(open.timestamp);
    } else {
      activeRunByOpenTimestamp.set(open.timestamp, {
        valveId,
        openTimestamp: open.timestamp,
        plannedSeconds,
      });
    }
  }

  return {
    durationByOpenTimestamp: pairing.durationByOpenTimestamp,
    activeRunByOpenTimestamp,
    staleOpenTimestamps,
  };
}

// Runs in progress, newest first.
export function sortedActiveRuns(runs: ValveRuns): ActiveRun[] {
  return [...runs.activeRunByOpenTimestamp.values()].sort(
    (a, b) => b.openTimestamp - a.openTimestamp
  );
}

export function elapsedSecondsFor(run: ActiveRun, now: number): number {
  return Math.max(0, Math.round((now - run.openTimestamp) / 1000));
}

// Seconds left of the requested run, or null when the request isn't known.
// Negative once the run is overdue.
export function remainingSecondsFor(run: ActiveRun, now: number): number | null {
  if (run.plannedSeconds === null) return null;
  return run.plannedSeconds - elapsedSecondsFor(run, now);
}

// Live description of a run in progress. Never reuses the bare-duration form
// used for completed runs, so the two can't be confused at a glance.
export function runningLabel(run: ActiveRun, now: number): string {
  const elapsedSeconds = elapsedSecondsFor(run, now);
  if (run.plannedSeconds === null) {
    return `running · ${formatDuration(elapsedSeconds)} elapsed`;
  }
  const remainingSeconds = run.plannedSeconds - elapsedSeconds;
  return remainingSeconds > 0
    ? `running · ${formatDuration(remainingSeconds)} left of ${formatDuration(run.plannedSeconds)}`
    : `running · ${formatDuration(elapsedSeconds)} elapsed · past ${formatDuration(run.plannedSeconds)}`;
}

// Re-render on a fixed cadence so elapsed/remaining counters stay live. Only
// ticks while `enabled`, so an idle view doesn't repaint once a second.
export function useNow(intervalMilliseconds: number, enabled: boolean): number {
  const [now, setNow] = useState<number>(() => Date.now());
  useEffect(() => {
    if (!enabled) return;
    setNow(Date.now());
    const id = setInterval(() => setNow(Date.now()), intervalMilliseconds);
    return () => clearInterval(id);
  }, [intervalMilliseconds, enabled]);
  return now;
}
