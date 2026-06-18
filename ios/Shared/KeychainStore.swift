import Foundation
import Security

/// Stores secrets in the Keychain, shared between the host app and the widget
/// extension via a Keychain access group (`kSecAttrAccessGroup`). The access group
/// is derived from the App Group so both targets read the same items.
///
/// Three secrets are stored:
///  - `apiToken`: the API bearer token (sent as `Authorization: Bearer ...`).
///  - `cfClientId` / `cfClientSecret`: Cloudflare Access service-token credentials
///    (sent as `CF-Access-Client-Id` / `CF-Access-Client-Secret`), needed for a
///    non-browser client to pass Cloudflare Access at the edge.
enum KeychainStore {
    enum Key: String {
        case apiToken
        case cfClientId
        case cfClientSecret
    }

    /// Keychain items are reachable after first unlock so the widget can read them
    /// while the device is locked (Lock Screen widget). Adjust if stricter access is wanted.
    private static let accessibility = kSecAttrAccessibleAfterFirstUnlock

    static func set(_ value: String, for key: Key) {
        let account = key.rawValue
        delete(key)
        guard let data = value.data(using: .utf8) else { return }

        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: account,
            kSecValueData as String: data,
            kSecAttrAccessible as String: accessibility,
        ]
        if let group = accessGroup {
            query[kSecAttrAccessGroup as String] = group
        }
        SecItemAdd(query as CFDictionary, nil)
    }

    static func get(_ key: Key) -> String? {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: key.rawValue,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        if let group = accessGroup {
            query[kSecAttrAccessGroup as String] = group
        }
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data
        else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func delete(_ key: Key) {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: key.rawValue,
        ]
        if let group = accessGroup {
            query[kSecAttrAccessGroup as String] = group
        }
        SecItemDelete(query as CFDictionary)
    }

    /// The shared Keychain access group. nil falls back to the app's default group,
    /// which still works as long as both targets share the same App ID prefix and
    /// the Keychain Sharing capability lists this group. Set via build settings if
    /// you need an explicit `$(AppIdentifierPrefix)group.ag.bramble.widget`.
    private static var accessGroup: String? { nil }
}
