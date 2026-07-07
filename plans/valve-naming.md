# Feature: Per-valve naming

## Context
A node can have multiple valves (identified only by 0-based index, `0 ≤ valve < valve_count`).
Today the dashboard shows bare "Valve 1 / Valve 2" and the iOS widget has a **local-only**
`label` per valve that nothing else knows about. Users need to give valves real names
("North Bed", "Greenhouse Row 3") that are consistent across the dashboard and iOS.

**Decision:** server is the source of truth. Names live in the API DB, are editable from the
dashboard and iOS, and the iOS widget's display name syncs from the server.

## Data model (mirrors `node_metadata`)
New DuckDB table (add to `SCHEMA` in `api/database.py`; `CREATE TABLE IF NOT EXISTS` runs on
every `init_db`, so existing DBs pick it up — no `_migrate_schema` entry needed):
```sql
CREATE TABLE IF NOT EXISTS valve_metadata (
    device_id UBIGINT NOT NULL,
    valve_index INTEGER NOT NULL,
    name VARCHAR,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (device_id, valve_index)
);
```
Name only (KISS — no notes/zone; valves already belong to a node).

## Backend (`api/database.py`, `api/app.py`)
- DB methods mirroring `get_node_metadata` / `get_all_node_metadata` / `update_node_metadata`
  (`database.py:877-979`):
  - `get_valve_metadata(device_id, valve_index)` → dict or None
  - `get_all_valve_metadata()` → `{device_id: {valve_index: {valve, name, updated_at}}}`
    (single query, used to embed names in `/api/nodes` without N queries)
  - `update_valve_metadata(device_id, valve_index, name)` → upsert, returns the row
- Endpoints mirroring node-metadata endpoints (`app.py:716-779`):
  - `GET  /api/nodes/<device_id>/valves/<valve_id>/metadata` → `{device_id, valve, name, updated_at}`
    (empty struct when unset)
  - `PUT  /api/nodes/<device_id>/valves/<valve_id>/metadata` → body `{ "name": "North Bed" }`;
    validate `0 ≤ valve_id < _node_valve_count(device_id)` (reuse helper at `app.py:226-253`)
- Surface names in `list_nodes` (both hub path ~`app.py:441` and `_build_nodes_from_database`
  ~`app.py:161`): add `node_dict['valves'] = {str(index): name, ...}` from one
  `get_all_valve_metadata()` call, so dashboard + iOS get names in the existing nodes fetch.

## Dashboard (`dashboard/`)
- `src/types/index.ts` — add `valves?: Record<string, string>` to the node type; `ValveMetadata`.
- `src/api/client.ts` — `updateValveMetadata(deviceId, valveIndex, name)` (PUT). Names for display
  come from the node payload's `valves` map (no extra fetch).
- `src/components/IrrigationControl.tsx` — replace hardcoded `Valve {i+1}` in the run controls,
  schedule builder, and schedule rows with the server name when present, falling back to
  `Valve {i+1}`.
- Editing: small inline editor (pattern from `NodeNameEditor.tsx`) — a pencil next to each valve
  row that PUTs the name and refreshes.

## iOS (`ios/`) — user builds/tests in Xcode
- `Shared/BrambleAPIClient.swift` — add `updateValveMetadata` (PUT) and read the `valves` map from
  the nodes response (or a per-valve GET).
- `Shared/ValveConfig.swift` — the display name derives from the server name; keep `label` as a
  local override only if the server name is absent (server = source of truth).
- `BrambleWidgetExtension/ValveWidget.swift` — show the server name.
- `BrambleWidget/SettingsView.swift` — when configuring a valve, prefill/sync the name from the
  server; editing the name writes back via `updateValveMetadata`.

## Rollout
1. **PR 1 — backend** (`database.py`, `app.py`): table, methods, endpoints, `/api/nodes` embed.
   Verifiable independently (curl the endpoints on the hub).
2. **PR 2 — dashboard**: display + edit.
3. **PR 3 — iOS**: client + widget + settings (user builds in Xcode).

## Verification
- Backend: `PUT /api/nodes/<id>/valves/0/metadata {"name":"North Bed"}` → 200; `GET` returns it;
  `/api/nodes` shows `"valves":{"0":"North Bed"}`; out-of-range valve id → 400.
- Dashboard: valve rows/schedules show the name; editing persists across reload.
- iOS: widget shows the server name; renaming in settings reflects on the dashboard.
