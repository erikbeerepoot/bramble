import type {
  Node,
  NodesResponse,
  NodeMetadata,
  SensorReading,
  SensorDataResponse,
  NodeStatistics,
  Zone,
  ZonesResponse,
  NodeEventsResponse,
  IrrigationSchedulesResponse,
} from '../types';

const API_URL_KEY = 'bramble_api_url';

function getBaseUrl(): string {
  const stored = localStorage.getItem(API_URL_KEY);
  if (stored) {
    return stored;
  }
  return import.meta.env.VITE_API_URL || '';
}

export function setApiUrl(url: string): void {
  if (url) {
    localStorage.setItem(API_URL_KEY, url);
  } else {
    localStorage.removeItem(API_URL_KEY);
  }
}

export function getApiUrl(): string {
  return getBaseUrl();
}

async function fetchApi<T>(endpoint: string, options?: RequestInit): Promise<T> {
  const baseUrl = getBaseUrl();
  const url = `${baseUrl}${endpoint}`;
  const isReadOnly = !options?.method || options.method === 'GET';

  const attempt = async (): Promise<Response> =>
    fetch(url, {
      ...options,
      credentials: 'include',
      headers: {
        'Content-Type': 'application/json',
        ...options?.headers,
      },
    });

  let response: Response;
  try {
    response = await attempt();
    // Retry once on gateway errors for read-only requests (e.g. container restart mid-request)
    if (
      isReadOnly &&
      (response.status === 502 || response.status === 503 || response.status === 504)
    ) {
      await new Promise((resolve) => setTimeout(resolve, 800));
      response = await attempt();
    }
  } catch (networkError) {
    if (!isReadOnly) throw networkError;
    await new Promise((resolve) => setTimeout(resolve, 800));
    response = await attempt();
  }

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: 'Unknown error' }));
    throw new Error(error.error || `HTTP ${response.status}`);
  }

  return response.json();
}

export async function getNodes(): Promise<NodesResponse> {
  return fetchApi<NodesResponse>('/api/nodes');
}

export async function getNode(deviceId: string): Promise<Node> {
  return fetchApi<Node>(`/api/nodes/${deviceId}`);
}

export async function getNodeMetadata(deviceId: string): Promise<NodeMetadata> {
  return fetchApi<NodeMetadata>(`/api/nodes/${deviceId}/metadata`);
}

export async function deleteNode(deviceId: string): Promise<void> {
  await fetchApi<{ message: string }>(`/api/nodes/${deviceId}`, {
    method: 'DELETE',
  });
}

export async function updateNodeMetadata(
  deviceId: string,
  metadata: Partial<Pick<NodeMetadata, 'name' | 'notes'>>
): Promise<NodeMetadata> {
  return fetchApi<NodeMetadata>(`/api/nodes/${deviceId}/metadata`, {
    method: 'PUT',
    body: JSON.stringify(metadata),
  });
}

export async function getNodeSensorData(
  deviceId: string,
  options?: {
    startTime?: number;
    endTime?: number;
    limit?: number;
    downsample?: number;
    signal?: AbortSignal;
  }
): Promise<SensorDataResponse> {
  const params = new URLSearchParams();
  if (options?.startTime) params.set('start_time', options.startTime.toString());
  if (options?.endTime) params.set('end_time', options.endTime.toString());
  if (options?.limit) params.set('limit', options.limit.toString());
  if (options?.downsample) params.set('downsample', options.downsample.toString());

  const query = params.toString();
  const endpoint = `/api/nodes/${deviceId}/sensor-data${query ? `?${query}` : ''}`;
  return fetchApi<SensorDataResponse>(endpoint, { signal: options?.signal });
}

export async function getNodeLatestReading(deviceId: string): Promise<SensorReading | null> {
  try {
    return await fetchApi<SensorReading>(`/api/nodes/${deviceId}/sensor-data/latest`);
  } catch {
    return null;
  }
}

export async function getNodeStatistics(
  deviceId: string,
  options?: {
    startTime?: number;
    endTime?: number;
    signal?: AbortSignal;
  }
): Promise<NodeStatistics> {
  const params = new URLSearchParams();
  if (options?.startTime) params.set('start_time', options.startTime.toString());
  if (options?.endTime) params.set('end_time', options.endTime.toString());

  const query = params.toString();
  const endpoint = `/api/nodes/${deviceId}/statistics${query ? `?${query}` : ''}`;
  return fetchApi<NodeStatistics>(endpoint, { signal: options?.signal });
}

export async function checkHealth(): Promise<{
  status: string;
  serial_connected: boolean;
  serial_port: string;
}> {
  return fetchApi('/api/health');
}

// Zone API methods

export async function getZones(): Promise<ZonesResponse> {
  return fetchApi<ZonesResponse>('/api/zones');
}

export async function createZone(zone: {
  name: string;
  color: string;
  description?: string;
}): Promise<Zone> {
  return fetchApi<Zone>('/api/zones', {
    method: 'POST',
    body: JSON.stringify(zone),
  });
}

export async function updateZone(
  zoneId: number,
  updates: Partial<Pick<Zone, 'name' | 'color' | 'description'>>
): Promise<Zone> {
  return fetchApi<Zone>(`/api/zones/${zoneId}`, {
    method: 'PUT',
    body: JSON.stringify(updates),
  });
}

export async function deleteZone(zoneId: number): Promise<void> {
  await fetchApi<{ message: string }>(`/api/zones/${zoneId}`, {
    method: 'DELETE',
  });
}

export async function setNodeZone(deviceId: string, zoneId: number | null): Promise<NodeMetadata> {
  return fetchApi<NodeMetadata>(`/api/nodes/${deviceId}/zone`, {
    method: 'PUT',
    body: JSON.stringify({ zone_id: zoneId }),
  });
}

export async function rebootNode(
  address: number
): Promise<{ status: string; task_id: string; message: string }> {
  return fetchApi(`/api/nodes/${address}/reboot`, {
    method: 'POST',
  });
}

export async function getNodeEvents(
  deviceId: string,
  options?: {
    startTime?: number;
    endTime?: number;
    limit?: number;
  }
): Promise<NodeEventsResponse> {
  const params = new URLSearchParams();
  if (options?.startTime) params.set('start_time', options.startTime.toString());
  if (options?.endTime) params.set('end_time', options.endTime.toString());
  if (options?.limit) params.set('limit', options.limit.toString());

  const query = params.toString();
  return fetchApi<NodeEventsResponse>(`/api/nodes/${deviceId}/events${query ? `?${query}` : ''}`);
}

// Irrigation API methods
//
// Schedules are stored on the node's PMU in UTC (the PMU's RTC is synced to
// UTC via the hub heartbeat response). The dashboard always works in the
// user's local time, so we convert at the boundary:
//   - localScheduleToUtc() on POST  (writing to API)
//   - utcScheduleToLocal() on GET   (reading from API)
//
// When the local→UTC conversion crosses midnight, the day-of-week bitmask
// shifts too. We snapshot the current DST offset; schedules will drift by
// an hour at DST boundaries until re-saved.

function shiftSchedule(
  hour: number,
  minute: number,
  days: number,
  toUtc: boolean
): { hour: number; minute: number; days: number } {
  const date = new Date();
  if (toUtc) {
    date.setHours(hour, minute, 0, 0);
    const newHour = date.getUTCHours();
    const newMinute = date.getUTCMinutes();
    const shift = (date.getUTCDay() - date.getDay() + 7) % 7;
    let newDays = 0;
    for (let i = 0; i < 7; i++) {
      if (days & (1 << i)) newDays |= 1 << ((i + shift) % 7);
    }
    return { hour: newHour, minute: newMinute, days: newDays };
  } else {
    date.setUTCHours(hour, minute, 0, 0);
    const newHour = date.getHours();
    const newMinute = date.getMinutes();
    const shift = (date.getDay() - date.getUTCDay() + 7) % 7;
    let newDays = 0;
    for (let i = 0; i < 7; i++) {
      if (days & (1 << i)) newDays |= 1 << ((i + shift) % 7);
    }
    return { hour: newHour, minute: newMinute, days: newDays };
  }
}

export async function getIrrigationSchedules(
  deviceId: string
): Promise<IrrigationSchedulesResponse> {
  const response = await fetchApi<IrrigationSchedulesResponse>(`/api/nodes/${deviceId}/schedules`);
  // Convert UTC → local for display
  response.schedules = response.schedules.map((s) => {
    const local = shiftSchedule(s.hour, s.minute, s.days, false);
    return { ...s, hour: local.hour, minute: local.minute, days: local.days };
  });
  return response;
}

export async function addIrrigationSchedule(
  deviceId: string,
  schedule: {
    index: number;
    hour: number;
    minute: number;
    duration: number;
    days: number;
    valve: number;
  }
): Promise<{ status: string; task_id: string; message: string }> {
  // Convert local → UTC before sending
  const utc = shiftSchedule(schedule.hour, schedule.minute, schedule.days, true);
  return fetchApi(`/api/nodes/${deviceId}/schedules`, {
    method: 'POST',
    body: JSON.stringify({
      ...schedule,
      hour: utc.hour,
      minute: utc.minute,
      days: utc.days,
    }),
  });
}

export async function deleteIrrigationSchedule(
  deviceId: string,
  index: number
): Promise<{ status: string; task_id: string; message: string }> {
  return fetchApi(`/api/nodes/${deviceId}/schedules/${index}`, {
    method: 'DELETE',
  });
}

export async function runValve(
  deviceId: string,
  valve: number,
  durationSeconds: number
): Promise<{ status: string; task_id: string; message: string }> {
  return fetchApi(`/api/nodes/${deviceId}/valve`, {
    method: 'POST',
    body: JSON.stringify({ valve, duration_seconds: durationSeconds }),
  });
}

export async function stopValve(
  deviceId: string,
  valve: number
): Promise<{ status: string; task_id: string; message: string }> {
  return fetchApi(`/api/nodes/${deviceId}/valve/stop`, {
    method: 'POST',
    body: JSON.stringify({ valve }),
  });
}

export async function setWakeInterval(
  deviceId: string,
  intervalSeconds: number
): Promise<{ status: string; task_id: string; message: string }> {
  return fetchApi(`/api/nodes/${deviceId}/wake-interval`, {
    method: 'POST',
    body: JSON.stringify({ interval_seconds: intervalSeconds }),
  });
}

export async function controlCurtain(
  address: number,
  action: 'open' | 'close' | 'stop' | 'calibrate'
): Promise<{ status: string; task_id: string; action: string; message: string }> {
  return fetchApi(`/api/nodes/${address}/curtain`, {
    method: 'POST',
    body: JSON.stringify({ action }),
  });
}
