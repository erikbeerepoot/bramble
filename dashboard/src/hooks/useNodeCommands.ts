import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { NodeCommand } from '../types';
import {
  getNodeCommands,
  runValve,
  stopValve,
  controlCurtain,
  setWakeInterval,
} from '../api/client';
import type { CurtainAction } from '../components/CurtainControl';
import { REFRESH_INTERVAL_MS } from '../config';

// Server-side default TTLs from api/command_queue.py — used only to give the
// optimistic ghost row a plausible `expires_at` until the server row replaces
// it. The real TTL is enforced server-side regardless.
const OPTIMISTIC_TTL_SECONDS: Record<NodeCommand['command_type'], number> = {
  valve_open: 1800,
  valve_close: 1800,
  curtain: 600,
  wake_interval: 300,
};

interface OptimisticCommand extends NodeCommand {
  tempKey: string;
  // Set once the POST returns; until then we render the ghost row,
  // after which the next server poll replaces it.
  serverId?: number;
}

function makeTempKey(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }
  return `${Date.now()}-${Math.random().toString(36).slice(2)}`;
}

function buildOptimistic(
  deviceId: string,
  commandType: NodeCommand['command_type'],
  params: Record<string, unknown>
): OptimisticCommand {
  const now = Math.floor(Date.now() / 1000);
  return {
    id: -1,
    tempKey: makeTempKey(),
    device_id: deviceId,
    command_type: commandType,
    params,
    status: 'pending',
    created_at: now,
    confirmed_at: null,
    expires_at: now + OPTIMISTIC_TTL_SECONDS[commandType],
    huey_task_id: null,
    confirming_event_code: null,
    confirming_event_detail: null,
  };
}

export interface UseNodeCommands {
  commands: NodeCommand[];
  refresh: () => Promise<void>;
  issueRunValve: (valve: number, durationSeconds: number) => Promise<void>;
  issueStopValve: (valve: number) => Promise<void>;
  issueCurtain: (action: CurtainAction, address: number) => Promise<void>;
  issueSetWakeInterval: (intervalSeconds: number) => Promise<void>;
}

export function useNodeCommands(deviceId: string): UseNodeCommands {
  const [serverCommands, setServerCommands] = useState<NodeCommand[]>([]);
  const [optimistic, setOptimistic] = useState<OptimisticCommand[]>([]);
  // Avoid double-refresh races if the hook unmounts mid-fetch.
  const aliveRef = useRef(true);

  useEffect(() => {
    aliveRef.current = true;
    return () => {
      aliveRef.current = false;
    };
  }, []);

  const refresh = useCallback(async () => {
    try {
      const resp = await getNodeCommands(deviceId, { limit: 100 });
      if (aliveRef.current) {
        setServerCommands(resp.commands);
      }
    } catch {
      // Silently fail — commands log is informational
    }
  }, [deviceId]);

  // Initial fetch + poll loop.
  useEffect(() => {
    refresh();
    const interval = setInterval(refresh, REFRESH_INTERVAL_MS);
    return () => clearInterval(interval);
  }, [refresh]);

  // Wrap a POST in optimistic state. On success, tag the ghost with the
  // returned server command_id (next poll's response will replace it). On
  // failure, mark it as failed and auto-clear after 5s.
  const wrap = useCallback(
    async (
      ghost: OptimisticCommand,
      send: () => Promise<{ command_id?: number | null }>
    ) => {
      setOptimistic((o) => [ghost, ...o]);
      try {
        const resp = await send();
        const serverId = typeof resp.command_id === 'number' ? resp.command_id : undefined;
        setOptimistic((o) =>
          o.map((x) =>
            x.tempKey === ghost.tempKey
              ? { ...x, serverId, id: serverId ?? x.id }
              : x
          )
        );
        // Pull the canonical row right away so the merge can drop the ghost.
        refresh();
      } catch (err) {
        setOptimistic((o) =>
          o.map((x) =>
            x.tempKey === ghost.tempKey ? { ...x, status: 'failed' } : x
          )
        );
        setTimeout(() => {
          if (!aliveRef.current) return;
          setOptimistic((o) => o.filter((x) => x.tempKey !== ghost.tempKey));
        }, 5000);
        throw err;
      }
    },
    [refresh]
  );

  const issueRunValve = useCallback(
    (valve: number, durationSeconds: number) =>
      wrap(
        buildOptimistic(deviceId, 'valve_open', {
          valve,
          duration_seconds: durationSeconds,
        }),
        () => runValve(deviceId, valve, durationSeconds)
      ),
    [deviceId, wrap]
  );

  const issueStopValve = useCallback(
    (valve: number) =>
      wrap(
        buildOptimistic(deviceId, 'valve_close', { valve }),
        () => stopValve(deviceId, valve)
      ),
    [deviceId, wrap]
  );

  const issueCurtain = useCallback(
    (action: CurtainAction, address: number) =>
      wrap(
        buildOptimistic(deviceId, 'curtain', { action }),
        () => controlCurtain(address, action)
      ),
    [deviceId, wrap]
  );

  const issueSetWakeInterval = useCallback(
    (intervalSeconds: number) =>
      wrap(
        buildOptimistic(deviceId, 'wake_interval', {
          interval_seconds: intervalSeconds,
        }),
        () => setWakeInterval(deviceId, intervalSeconds)
      ),
    [deviceId, wrap]
  );

  // Merge: server data wins. Ghost rows are visible until their serverId
  // shows up in serverCommands (or the POST resolved with no command_id,
  // in which case they live until manually cleared / failed).
  const commands = useMemo<NodeCommand[]>(() => {
    const serverIds = new Set(serverCommands.map((c) => c.id));
    const visibleOptimistic = optimistic.filter(
      (o) => o.serverId === undefined || !serverIds.has(o.serverId)
    );
    return [...serverCommands, ...visibleOptimistic].sort(
      (a, b) => b.created_at - a.created_at
    );
  }, [serverCommands, optimistic]);

  return {
    commands,
    refresh,
    issueRunValve,
    issueStopValve,
    issueCurtain,
    issueSetWakeInterval,
  };
}
