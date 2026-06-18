import Foundation

/// Last-known UI state for a valve, shown in the widget. This is optimistic local
/// state written by the App Intents (the API is fire-and-forget at 202 Accepted);
/// it is not authoritative hardware state. A follow-up could reconcile against
/// `GET /api/tasks/{task_id}` or node status.
enum ValveRunState: String, Codable {
    case idle
    case pending   // command sent, awaiting result
    case running
    case error
}

struct ValveState: Codable {
    var state: ValveRunState
    var message: String?
}

/// Stores per-valve `ValveState` in the App Group's shared defaults, keyed by
/// device id + valve index. Shared so the widget extension (writer) and any future
/// host-app view (reader) agree.
enum ValveStateStore {
    private static let key = "valveStates"

    private static var defaults: UserDefaults {
        UserDefaults(suiteName: AppGroup.identifier) ?? .standard
    }

    private static func mapKey(deviceId: String, valve: Int) -> String {
        "\(deviceId)#\(valve)"
    }

    static func state(deviceId: String, valve: Int) -> ValveState {
        all()[mapKey(deviceId: deviceId, valve: valve)] ?? ValveState(state: .idle, message: nil)
    }

    static func markPending(deviceId: String, valve: Int) {
        set(deviceId: deviceId, valve: valve, ValveState(state: .pending, message: nil))
    }

    static func markRunning(deviceId: String, valve: Int) {
        set(deviceId: deviceId, valve: valve, ValveState(state: .running, message: nil))
    }

    static func markIdle(deviceId: String, valve: Int) {
        set(deviceId: deviceId, valve: valve, ValveState(state: .idle, message: nil))
    }

    static func markError(deviceId: String, valve: Int, message: String) {
        set(deviceId: deviceId, valve: valve, ValveState(state: .error, message: message))
    }

    private static func set(deviceId: String, valve: Int, _ value: ValveState) {
        var map = all()
        map[mapKey(deviceId: deviceId, valve: valve)] = value
        if let data = try? JSONEncoder().encode(map) {
            defaults.set(data, forKey: key)
        }
    }

    private static func all() -> [String: ValveState] {
        guard let data = defaults.data(forKey: key),
              let decoded = try? JSONDecoder().decode([String: ValveState].self, from: data)
        else { return [:] }
        return decoded
    }
}
