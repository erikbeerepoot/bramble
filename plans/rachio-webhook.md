# Rachio Webhook → Bramble Valve (auto-fire)

## Goal
When a zone starts on the existing Rachio irrigation controller, have Bramble
automatically run a mapped Bramble valve for the same duration. No human in the
loop, no notifications — a pre-configured mapping drives the action.

## Flow
```
Rachio zone starts / stops
  → POST api.bramble.ag/api/integrations/rachio/webhook   (Cloudflare-Access-bypassed)
     → verify shared secret (Rachio "externalId" field, constant-time compare)
     → dedup on Rachio event "id"
     → subType == ZONE_STARTED  → look up mapping → run Bramble valve
       subType == ZONE_STOPPED/ZONE_COMPLETED → stop Bramble valve
     → log a node_event, return 200 quickly
```

## Rachio payload (ZONE_STATUS, confirmed against Rachio docs)
Relevant fields on each webhook POST:
- `type` = "ZONE_STATUS"
- `subType` = ZONE_STARTED | ZONE_STOPPED | ZONE_COMPLETED | ZONE_CYCLING
- `deviceId` (controller UUID string)
- `zoneNumber` (int, 1-based on Rachio)
- `zoneName`, `zoneId`
- `duration` (seconds), `durationInMinutes`
- `externalId` (the secret we set at registration; echoed back on every event)
- `id` (unique event id — used for dedup)
- `timestamp` (ISO 8601)

## Design decisions (defaults, confirmed with user)
1. **Run duration**: use Rachio's actual `duration` (seconds) from the payload,
   clamped 1..7200; fall back to the mapping's `duration_seconds` if absent/invalid.
2. **Zone stop**: handle ZONE_STOPPED / ZONE_COMPLETED → stop the Bramble valve
   early. Bramble also auto-closes on its own timer, so this only matters if the
   Rachio run is cut short.
3. **Mapping management**: seed the table via API/CLI for v1; dashboard Settings
   UI is a fast-follow (not in this branch).

## Components
1. **Config** (`api/config.py`): `RACHIO_WEBHOOK_SECRET` env var.
2. **DB** (`api/database.py`): new `rachio_zone_mappings` table +
   get/upsert/list/delete methods.
3. **Refactor** (`api/app.py`): extract `_run_valve_once()` / `_stop_valve_once()`
   from the existing `run_valve` / `stop_valve` handlers so the HTTP endpoints and
   the webhook share one code path (keeps master-valve group mirroring for free).
4. **Webhook route** (`api/app.py`): `POST /api/integrations/rachio/webhook` —
   NOT `@require_token` (Rachio can't send our bearer token nor pass Cloudflare
   Access). Authenticated by the `externalId` shared secret. In-memory dedup on
   event `id`.
5. **Mapping CRUD** (`api/app.py`): `GET/POST/DELETE /api/integrations/rachio/mappings`
   — `@require_token` (dashboard/CLI managed).
6. **Registration script** (`api/scripts/register_rachio_webhook.py`): one-shot,
   calls Rachio's public API with the user's Rachio token to register the webhook
   URL + ZONE_STATUS event type + externalId secret.

## Out-of-band (user action, not code)
- **Cloudflare Access bypass** for `/api/integrations/rachio/webhook` so Rachio's
  unauthenticated POST reaches Flask (the tunnel is remotely-managed).
- Set `RACHIO_WEBHOOK_SECRET` in the API container env (docker-compose.prod.yml).
- Obtain Rachio API token + controller deviceId + zone numbers (the registration
  script can list these).

## Testing
- Unit tests for mapping DB methods + webhook handler (secret reject, dedup,
  started→run, stopped→stop, unmapped→ignore) under `api/tests/`.
- Manual: `curl` a sample ZONE_STARTED payload at the local API.

## Status
- [x] Config + DB table + methods
- [x] Refactor run/stop helpers
- [x] Webhook route + dedup
- [x] Mapping CRUD
- [x] Registration script
- [x] Tests (11 rachio tests pass; valve-group endpoint tests still green)
- [x] Docs (api/README.md) + docker-compose env

## Remaining (out-of-band, user action)
- [ ] Set `RACHIO_WEBHOOK_SECRET` in the hub API container env
- [ ] Add a Cloudflare Access **bypass** policy for
      `/api/integrations/rachio/webhook`
- [ ] Run `scripts/register_rachio_webhook.py list` then `register`
- [ ] Seed mappings via `POST /api/integrations/rachio/mappings`

## Fast-follow (separate branch)
- Dashboard Settings UI for managing mappings (currently API/CLI only)
