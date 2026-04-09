// Runtime-configurable refresh interval for periodic polling in the dashboard.
// Override by setting VITE_REFRESH_INTERVAL_MS at build time.
export const REFRESH_INTERVAL_MS = Number(import.meta.env.VITE_REFRESH_INTERVAL_MS) || 10000;
