import AppIntents
import WidgetKit

/// Runs a valve for its configured default duration. Bound to the "Run" button in the
/// widget via `Button(intent:)`. App Intents triggered from a widget get a short
/// execution budget (a few seconds) — enough for a single POST — after which the
/// widget timeline is reloaded to reflect the new state.
struct RunValveIntent: AppIntent {
    static var title: LocalizedStringResource = "Run Valve"
    static var isDiscoverable: Bool = false

    @Parameter(title: "Device ID")
    var deviceId: String

    @Parameter(title: "Valve")
    var valve: Int

    @Parameter(title: "Duration (seconds)")
    var durationSeconds: Int

    init() {}

    init(deviceId: String, valve: Int, durationSeconds: Int) {
        self.deviceId = deviceId
        self.valve = valve
        self.durationSeconds = durationSeconds
    }

    func perform() async throws -> some IntentResult {
        ValveStateStore.markPending(deviceId: deviceId, valve: valve)
        do {
            try await BrambleAPIClient().runValve(
                deviceId: deviceId, valve: valve, durationSeconds: durationSeconds)
            ValveStateStore.markRunning(deviceId: deviceId, valve: valve)
        } catch {
            ValveStateStore.markError(deviceId: deviceId, valve: valve, message: error.localizedDescription)
        }
        WidgetCenter.shared.reloadAllTimelines()
        return .result()
    }
}

/// Stops a running valve immediately. Bound to the "Stop" button in the widget.
struct StopValveIntent: AppIntent {
    static var title: LocalizedStringResource = "Stop Valve"
    static var isDiscoverable: Bool = false

    @Parameter(title: "Device ID")
    var deviceId: String

    @Parameter(title: "Valve")
    var valve: Int

    init() {}

    init(deviceId: String, valve: Int) {
        self.deviceId = deviceId
        self.valve = valve
    }

    func perform() async throws -> some IntentResult {
        ValveStateStore.markPending(deviceId: deviceId, valve: valve)
        do {
            try await BrambleAPIClient().stopValve(deviceId: deviceId, valve: valve)
            ValveStateStore.markIdle(deviceId: deviceId, valve: valve)
        } catch {
            ValveStateStore.markError(deviceId: deviceId, valve: valve, message: error.localizedDescription)
        }
        WidgetCenter.shared.reloadAllTimelines()
        return .result()
    }
}
