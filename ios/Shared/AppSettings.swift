import Foundation

/// Non-secret configuration shared between the host app and the widget extension
/// via the App Group's shared `UserDefaults`. Secrets (the API bearer token and the
/// Cloudflare service-token credentials) live in the Keychain — see `KeychainStore`.
///
/// The App Group identifier must match the one configured on both targets'
/// entitlements (see `project.yml`).
enum AppGroup {
    static let identifier = "group.ag.bramble.widget"
}

struct AppSettings {
    private static let baseUrlKey = "baseUrl"
    private static let valvesKey = "valves"

    private static var defaults: UserDefaults {
        UserDefaults(suiteName: AppGroup.identifier) ?? .standard
    }

    /// API base URL, e.g. `https://api.bramble.ag`. No trailing slash.
    static var baseUrl: String {
        get { defaults.string(forKey: baseUrlKey) ?? "https://api.bramble.ag" }
        set { defaults.set(newValue, forKey: baseUrlKey) }
    }

    /// The valves shown in the widget. Persisted as JSON in the shared defaults.
    static var valves: [ValveConfig] {
        get {
            guard let data = defaults.data(forKey: valvesKey),
                  let decoded = try? JSONDecoder().decode([ValveConfig].self, from: data)
            else { return [] }
            return decoded
        }
        set {
            let data = try? JSONEncoder().encode(newValue)
            defaults.set(data, forKey: valvesKey)
        }
    }

    /// Valves marked primary, in configured order — shown on single-action surfaces
    /// (Lock Screen accessory widgets, later Control Center / Action Button).
    static var primaryValves: [ValveConfig] {
        valves.filter(\.isPrimary)
    }
}
