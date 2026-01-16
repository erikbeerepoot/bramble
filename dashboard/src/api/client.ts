import type {
  Node,
  NodesResponse,
  NodeMetadata,
  SensorDataResponse,
  NodeStatistics,
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

export async function updateNodeMetadata(
  address: number,
  metadata: Partial<Pick<NodeMetadata, 'name' | 'location' | 'notes'>>
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
  }
): Promise<SensorDataResponse> {
  const params = new URLSearchParams();
  if (options?.startTime) params.set('start_time', options.startTime.toString());
  if (options?.endTime) params.set('end_time', options.endTime.toString());
  if (options?.limit) params.set('limit', options.limit.toString());

  const query = params.toString();
  const endpoint = `/api/nodes/${address}/sensor-data${query ? `?${query}` : ''}`;
  return fetchApi<SensorDataResponse>(endpoint);
}

export async function getNodeStatistics(address: number): Promise<NodeStatistics> {
  return fetchApi<NodeStatistics>(`/api/nodes/${address}/statistics`);
}

export async function checkHealth(): Promise<{
  status: string;
  serial_connected: boolean;
  serial_port: string;
}> {
  return fetchApi('/api/health');
}
