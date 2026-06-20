# Bramble Valve Widget (iOS)

An iOS 18+ Home/Lock Screen **widget** to start and stop irrigation valves via the
Bramble REST API. Each valve gets a Run and a Stop button; tapping one fires an
**App Intent** that POSTs to the API directly from the widget process.

The north star is **time to task** — minimize the steps from intent to starting a
watering task. The Home Screen widget shows all configured valves (Run/Stop each); the
Lock Screen accessory widget shows your **primary** valves with a single toggle (tap to
run, tap to stop) for the fewest possible taps. See
[`../plans/ios_valve_widget.md`](../plans/ios_valve_widget.md) for the full design.

> **Status:** source + project definition are committed and the shared networking/
> storage layer type-checks against the SDK, but the project has **not been built**
> (this repo's CI and the dev machine that scaffolded it only have Command Line Tools,
> not full Xcode). First build must happen in Xcode on a Mac — expect to resolve minor
> SDK/signing issues there.

## Layout

```
ios/
  project.yml                       XcodeGen project definition
  Shared/                           compiled into BOTH targets
    BrambleAPIClient.swift          URLSession client (runValve / stopValve / health)
    KeychainStore.swift             secrets in Keychain (shared access group)
    AppSettings.swift               base URL + valve list (App Group defaults)
    ValveConfig.swift               a configured valve (label, device id, valve, duration)
    ValveStateStore.swift           optimistic per-valve UI state
  BrambleWidget/                    host app (settings screen)
    BrambleWidgetApp.swift, SettingsView.swift
    BrambleWidget.entitlements
  BrambleWidgetExtension/           widget extension
    BrambleWidgetBundle.swift       registers both widgets
    ValveWidget.swift               Home Screen widget (all valves, Run/Stop)
    ValveLockScreenWidget.swift     Lock Screen accessory widget (primary valves, toggle)
    ValveControlIntents.swift       RunValveIntent / StopValveIntent / ToggleValveIntent
    Info.plist, BrambleWidgetExtension.entitlements
```

## Generate & open the project

The `.xcodeproj` is generated, not committed (avoids churn in `project.pbxproj`):

```bash
brew install xcodegen      # one-time
cd ios
xcodegen generate          # writes BrambleWidget.xcodeproj
open BrambleWidget.xcodeproj
```

## One-time Xcode / Apple setup (cannot be scripted here)

1. **Signing team** — select your Apple Developer team on *both* the `BrambleWidget`
   and `BrambleWidgetExtension` targets (Signing & Capabilities), or set
   `DEVELOPMENT_TEAM` in `project.yml`. A **paid** account is effectively required —
   free signing expires every 7 days, impractical for an always-present widget.
2. **App Group** — both targets already declare `group.ag.bramble.widget` in their
   entitlements. Register the group on your developer account / let Xcode auto-create it.
3. **Keychain Sharing** — both entitlements list `$(AppIdentifierPrefix)ag.bramble.widget`
   so the app and widget read the same secrets. Confirm the capability is enabled.
4. If you change the bundle id prefix, update it in `project.yml`, both `.entitlements`,
   and `AppGroup.identifier` in `Shared/AppSettings.swift`.

## Cloudflare Access (so the widget can reach the API)

`api.bramble.ag` sits behind Cloudflare Access. A widget is not a browser and can't do
the interactive login, so:

1. In Cloudflare Zero Trust, create an **Access service token**.
2. Add an Access policy on `api.bramble.ag` that allows that service token.
3. Enter the token's **Client ID** and **Client Secret** in the app's settings screen
   (stored in the Keychain, sent as `CF-Access-Client-Id` / `CF-Access-Client-Secret`).

## API token

The backend gates the valve endpoints on a bearer token when `API_TOKEN` is set
(see [`../api/docs/API.md`](../api/docs/API.md#authentication)). Enter the same token
in the app's settings screen — it is sent as `Authorization: Bearer <token>`.

## Configure & use

1. Launch the app, fill in **Base URL** (`https://api.bramble.ag`), the **API token**,
   and the **CF-Access** client id/secret. Tap **Test Connection** (hits `/api/health`).
2. Add one or more **valves**: a label, the node's `device_id` (64-bit hardware id),
   the 0-based valve index, and a default run duration. Toggle **Show on Lock Screen**
   for the one or two valves you reach for most. Tap **Save**.
3. Add the **Valves** widget to your Home Screen — tap a valve's drop icon to run it for
   its default duration, or the stop icon to close it.
4. Add the **Valves (Lock Screen)** accessory widget to your Lock Screen — it shows your
   primary valves; one tap runs an idle valve, another stops it. Circular fits one valve,
   rectangular up to two, inline shows a running-count summary.

## Known limitations / follow-ups

- Widget state is **optimistic** (written locally on tap); it does not yet reflect true
  hardware state. Follow-up: poll `GET /api/tasks/{task_id}` or node status.
- App Intent execution from a widget has a short time budget — calls are fire-and-forget.
- Backend v1 trusts the `Cf-Access-Jwt-Assertion` header without verifying its signature
  (it only arrives via the tunnel). Hardening follow-up: verify against Cloudflare JWKS.
