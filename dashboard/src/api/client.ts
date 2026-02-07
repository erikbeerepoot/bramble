import type {
  Node,
  NodesResponse,
  NodeMetadata,
  SensorReading,
  SensorDataResponse,
  NodeStatistics,
  Zone,
  ZonesResponse,
} from '../types';

const API_URL_KEY = 'bramble_api_url';

function getBaseUrl(): string {
  const stored = localStorage.getItem(API_URL_KEY);
  if (stored) {
    return stored;
  }
  return import.meta.env.VITE_API_URL || 'http://localhost:5000';
}

export function setApiUrl(url: string): void {
  localStorage.setItem(API_URL_KEY, url);
}

export function getApiUrl(): string {
  return getBaseUrl();
}

async function fetchApi<T>(endpoint: string, options?: RequestInit): Promise<T> {
  const baseUrl = getBaseUrl();
  const url = `${baseUrl}${endpoint}`;

  const response = await fetch(url, {
    ...options,
    headers: {
      'Content-Type': 'application/json',
      ...options?.headers,
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: 'Unknown error' }));
    throw new Error(error.error || `HTTP ${response.status}`);
  }

  return response.json();
}

export async function getNodes(): Promise<NodesResponse> {
  return fetchApi<NodesResponse>('/api/nodes');
}

export async function getNode(address: number): Promise<Node> {
  return fetchApi<Node>(`/api/nodes/${address}`);
}

export async function getNodeMetadata(address: number): Promise<NodeMetadata> {
  return fetchApi<NodeMetadata>(`/api/nodes/${address}/metadata`);
}

export async function deleteNode(address: number): Promise<void> {
  await fetchApi<{ message: string }>(`/api/nodes/${address}`, {
    method: 'DELETE',
  });
}

export async function updateNodeMetadata(
  address: number,
  metadata: Partial<Pick<NodeMetadata, 'name' | 'notes'>>
): Promise<NodeMetadata> {
  return fetchApi<NodeMetadata>(`/api/nodes/${address}/metadata`, {
    method: 'PUT',
    body: JSON.stringify(metadata),
  });
}

export async function getNodeSensorData(
  address: number,
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
  const endpoint = `/api/nodes/${address}/sensor-data${query ? `?${query}` : ''}`;
  return fetchApi<SensorDataResponse>(endpoint, { signal: options?.signal });
}

export async function getNodeLatestReading(
  address: number
): Promise<SensorReading | null> {
  try {
    return await fetchApi<SensorReading>(`/api/nodes/${address}/sensor-data/latest`);
  } catch {
    return null;
  }
}

export async function getNodeStatistics(
  address: number,
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
  const endpoint = `/api/nodes/${address}/statistics${query ? `?${query}` : ''}`;
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

export async function setNodeZone(
  address: number,
  zoneId: number | null
): Promise<NodeMetadata> {
  return fetchApi<NodeMetadata>(`/api/nodes/${address}/zone`, {
    method: 'PUT',
    body: JSON.stringify({ zone_id: zoneId }),
  });
}
