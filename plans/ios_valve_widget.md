# iOS Valve Widget

## Goal

An **iOS Home Screen / Lock Screen widget** that lets the user start and stop irrigation
valves directly from a button tap, by calling the existing Bramble REST API
(`api.bramble.ag`). No companion screen interaction required for the common case —
tap a valve button in the widget → valve runs for a preset duration; tap stop → valve
closes.

## North star: time to task

The guiding metric is **time to task** — minimizing the time from intent to starting/
managing a watering task. Automation's value is not doing the task yourself, but you still
have to *start and manage* it, so that step must be near-instant. This is why the primary
surfaces are widgets (no app launch), with Control Center / Action Button / Siri as future
accelerators, and why the host app must launch fast and stay minimal.

**Surfaces, ranked by time-to-task:** Action Button (1 press) > Control Center control
(swipe+tap) > Lock Screen widget/control (tap, no unlock) > Home Screen widget (multi-valve
grid) > Siri/App Shortcuts > the app itself. Deployment target is **iOS 18** to keep the
Controls API (Control Center / Action Button) available for a later phase.

## Current state

- **No Apple code exists.** Only client is the React/Vite dashboard (`dashboard/`).
- **Valve API is ready** (`api/app.py`):
  - Start: `POST /api/nodes/{device_id}/valve` → `{"valve": 0, "duration_seconds": 900}` → `202` `{status, task_id, command_id, ...}`. `duration_seconds` 1–7200; `valve` range-checked against node valve count.
  - Stop: `POST /api/nodes/{device_id}/valve/stop` → `{"valve": 0}` → `202`.
  - Status (optional): `GET /api/tasks/{task_id}`, `GET /api/health`.
- **Auth today**: none at the Flask layer (`api/docs/API.md`: "No authentication required").
  Production is gated only by **Cloudflare Access** (cookie/JWT); the dashboard rides that
  cookie via `credentials: 'include'` (`dashboard/src/api/client.ts:49`). CORS is locked to
  the dashboard origin (`app.py:30`).

## Auth design (decided: bearer token in API)

Add an app-controlled, revocable bearer token at the Flask layer, while keeping the
dashboard's existing Cloudflare-Access cookie flow working.

**Backend (`api/`):**
- New config `API_TOKEN = os.getenv('API_TOKEN', '')` in `config.py`. Empty ⇒ enforcement
  off (preserves local/dev "no auth, local network" behavior).
- New `@require_token` decorator (small `auth.py` module). When `API_TOKEN` is set, a request
  is allowed if **either**:
  1. `Authorization: Bearer <token>` matches `API_TOKEN` (constant-time compare) — the widget path, **or**
  2. the request carries Cloudflare's `Cf-Access-Jwt-Assertion` header — i.e. an
     already-CF-Access-authenticated dashboard user. (v1 trusts the edge since Flask is only
     reachable through the tunnel; hardening note below to verify the JWT against CF JWKS.)
- Apply `@require_token` to the two valve routes for v1 (`run_valve`, `stop_valve`). The
  decorator is reusable for the other 17 mutating routes later.
- Return `401` JSON on failure. Keep CORS as-is (widget is not a browser; no preflight).

**Cloudflare (user-side config, documented — not code):**
- Create a **service token** in Cloudflare Access and a policy allowing it on
  `api.bramble.ag`, so the widget's non-browser request passes the edge. Widget sends
  `CF-Access-Client-Id` + `CF-Access-Client-Secret` (edge) **and** `Authorization: Bearer`
  (Flask). Defense in depth: both the edge and the app gate the request.
- Simpler alternative (documented, not chosen): a CF Access **bypass** rule for the valve
  paths, leaving the Flask bearer token as the sole gate. Less defense-in-depth.

**Secrets on device:** API token (and CF service-token id/secret) stored in the **Keychain**,
shared with the widget extension via an **App Group** + keychain access group. Never hardcoded.

## iOS app design

Greenfield Xcode project under `ios/BrambleWidget/`. Three pieces:

1. **Host app target** (`BrambleWidget`) — minimal SwiftUI app whose only jobs are:
   - Settings screen: API base URL, bearer token, CF service-token id/secret → Keychain.
   - Valve configuration: an editable list of `{ label, device_id, valve, default_duration }`
     entries persisted to the shared App Group (`UserDefaults(suiteName:)`). v1 may seed from
     a hardcoded list and allow edits.
   - "Test connection" button hitting `GET /api/health`.
2. **Widget extension target** (`BrambleWidgetExtension`, WidgetKit) — **iOS 17+ interactive
   widget**:
   - Timeline shows the configured valves as a grid of buttons with current state
     (idle / running / error).
   - Each Run/Stop button is a SwiftUI `Button(intent:)` bound to an **App Intent**
     (`RunValveIntent`, `StopValveIntent`). The intent performs the network POST (App Intents
     get a few seconds of execution — enough for one request) and calls
     `WidgetCenter.shared.reloadAllTimelines()`.
3. **Shared layer** (Swift package or shared group) — used by both targets:
   - `BrambleAPIClient`: `URLSession`-based, mirrors `runValve`/`stopValve`; injects
     `Authorization: Bearer` + CF service-token headers; decodes the `202` response.
   - `KeychainStore` + `ValveConfig` (Codable, App Group).

**Widget interaction model:** iOS 17 `AppIntent` + `Button(intent:)` is the only way to act
from a widget without launching the app. Pre-iOS-17 fallback is out of scope (deep-link into
app only) — target iOS 17+.

## Touch points

1. `api/config.py` — add `API_TOKEN`.
2. `api/auth.py` (new) — `require_token` decorator + constant-time compare + CF assertion check.
3. `api/app.py` — import + decorate `run_valve`, `stop_valve`.
4. `api/tests/` — tests: no token configured ⇒ allowed; token set + valid bearer ⇒ allowed;
   token set + missing/wrong bearer ⇒ 401; token set + CF assertion header ⇒ allowed.
5. `api/docs/API.md` + `api/README.md` — document `API_TOKEN`, the bearer header, and the
   Cloudflare service-token setup steps.
6. `api/docker-compose.prod.yml` / systemd env — wire `API_TOKEN` (user adds the secret).
7. `ios/BrambleWidget/` (new) — Xcode project: host app, widget extension, shared layer.
8. `ios/README.md` (new) — build/signing/Cloudflare/Keychain setup, App Group id.

## Build / signing notes

- WidgetKit interactive buttons require **iOS 17+** and the **App Intents** framework.
- A widget cannot ship alone — it needs a host app target.
- On-device install / TestFlight needs a **paid Apple Developer account** ($99/yr); free
  signing resigns every 7 days (impractical for an always-present widget).
- Xcode project generation: hand-authored `.xcodeproj` (or `xcodegen` if available). The repo
  CI does not build iOS; this target is built locally in Xcode.

## Risks / notes

- **Auth correctness is the crux.** Must verify the dashboard still works after `@require_token`
  lands (CF assertion path). Test against staging before pointing the widget at prod.
- v1 trusts the `Cf-Access-Jwt-Assertion` header without verifying its signature. Acceptable
  because Flask is only reachable via the tunnel; **hardening follow-up**: validate the JWT
  against Cloudflare's JWKS (`/cdn-cgi/access/certs`).
- App Intent execution budget is short; keep the request fast and fire-and-forget. Optional
  `task_id` polling for confirmed state is a follow-up, not v1.
- Token is a bearer secret — anyone holding it can drive valves. Keychain-only storage; make
  it rotatable via the `API_TOKEN` env.

## Phasing

- **Phase 1 (backend):** ✅ **DONE.** config `API_TOKEN` + `auth.py` `require_token` +
  decorated valve routes + `tests/test_auth.py` + docs (API.md/README/compose). Auth logic
  verified (7/7 branches) in a throwaway Flask harness; full pytest suite to run in the
  project's uv env.
- **Phase 2 (iOS shared layer + host app):** ✅ **DONE (unbuilt).** `Shared/` (API client,
  Keychain, settings, valve config, state store) + host app (settings screen). Shared layer
  type-checks against the SDK.
- **Phase 3 (Home Screen widget):** ✅ **DONE (unbuilt).** WidgetKit widget + `Button(intent:)`
  + `RunValveIntent`/`StopValveIntent` + optimistic state display. `project.yml`, Info.plist,
  entitlements committed. **First build must happen in Xcode** (this env has CLT only — no
  iOS SDK / xcodebuild). Source parses clean; expect minor SDK/signing fixups in Xcode.
- **Phase 3b (Lock Screen widget + primary-valve model):** ✅ **DONE (unbuilt).** Deployment
  target bumped to iOS 18. `ValveConfig.isPrimary` (+ backward-compatible decoding) and
  `AppSettings.primaryValves`; settings toggle to mark primaries. `ValveLockScreenWidget`
  supports `.accessoryCircular` / `.accessoryRectangular` / `.accessoryInline`, driven by a
  single-button `ToggleValveIntent` (run-if-idle / stop-if-running) for tiny surfaces.
- **Phase 4 (next time-to-task surfaces):** Control Center + Action Button (`ControlWidget`,
  iOS 18) for the primary valve; Siri / App Shortcuts (`AppShortcutsProvider`) over the
  existing intents.
- **Phase 5 (follow-up):** CF JWKS signature verification; `task_id`/node-status polling for
  authoritative widget state; richer multi-node config UI.
