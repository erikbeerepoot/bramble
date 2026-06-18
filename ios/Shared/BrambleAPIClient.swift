import Foundation

/// Errors surfaced by `BrambleAPIClient`.
enum BrambleAPIError: LocalizedError {
    case badURL
    case unauthorized
    case http(status: Int, message: String)
    case transport(Error)

    var errorDescription: String? {
        switch self {
        case .badURL:
            return "Invalid API base URL."
        case .unauthorized:
            return "Unauthorized — check the API token."
        case let .http(status, message):
            return "HTTP \(status): \(message)"
        case let .transport(error):
            return error.localizedDescription
        }
    }
}

/// Response body from the valve endpoints (HTTP 202).
struct ValveCommandResponse: Decodable {
    let status: String
    let taskId: String?
    let message: String?

    enum CodingKeys: String, CodingKey {
        case status
        case taskId = "task_id"
        case message
    }
}

/// Thin client over the Bramble REST API, mirroring the dashboard's `runValve` /
/// `stopValve` calls. Reads its base URL from `AppSettings` and its credentials from
/// `KeychainStore`, so both the host app and the widget extension share configuration.
struct BrambleAPIClient {
    private let session: URLSession

    init(session: URLSession = .shared) {
        self.session = session
    }

    /// Start a run-once watering: POST /api/nodes/{deviceId}/valve
    @discardableResult
    func runValve(deviceId: String, valve: Int, durationSeconds: Int) async throws -> ValveCommandResponse {
        try await post(
            path: "/api/nodes/\(deviceId)/valve",
            body: ["valve": valve, "duration_seconds": durationSeconds]
        )
    }

    /// Stop a running valve immediately: POST /api/nodes/{deviceId}/valve/stop
    @discardableResult
    func stopValve(deviceId: String, valve: Int) async throws -> ValveCommandResponse {
        try await post(
            path: "/api/nodes/\(deviceId)/valve/stop",
            body: ["valve": valve]
        )
    }

    /// Lightweight connectivity check: GET /api/health
    func checkHealth() async throws {
        guard let url = URL(string: AppSettings.baseUrl + "/api/health") else {
            throw BrambleAPIError.badURL
        }
        var request = URLRequest(url: url)
        applyAuthHeaders(to: &request)
        try await send(request)
    }

    // MARK: - Internals

    private func post(path: String, body: [String: Any]) async throws -> ValveCommandResponse {
        guard let url = URL(string: AppSettings.baseUrl + path) else {
            throw BrambleAPIError.badURL
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        applyAuthHeaders(to: &request)

        let data = try await send(request)
        do {
            return try JSONDecoder().decode(ValveCommandResponse.self, from: data)
        } catch {
            // The command was accepted but the body was unexpected; treat as queued.
            return ValveCommandResponse(status: "queued", taskId: nil, message: nil)
        }
    }

    /// Send a request, mapping non-2xx responses to `BrambleAPIError`. Returns the body.
    @discardableResult
    private func send(_ request: URLRequest) async throws -> Data {
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: request)
        } catch {
            throw BrambleAPIError.transport(error)
        }

        guard let http = response as? HTTPURLResponse else {
            return data
        }
        switch http.statusCode {
        case 200..<300:
            return data
        case 401, 403:
            throw BrambleAPIError.unauthorized
        default:
            let message = (try? JSONSerialization.jsonObject(with: data) as? [String: Any])?["error"] as? String
            throw BrambleAPIError.http(status: http.statusCode, message: message ?? "request failed")
        }
    }

    /// Attach the bearer token and Cloudflare Access service-token headers, when present.
    private func applyAuthHeaders(to request: inout URLRequest) {
        if let token = KeychainStore.get(.apiToken), !token.isEmpty {
            request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        if let clientId = KeychainStore.get(.cfClientId), !clientId.isEmpty {
            request.setValue(clientId, forHTTPHeaderField: "CF-Access-Client-Id")
        }
        if let clientSecret = KeychainStore.get(.cfClientSecret), !clientSecret.isEmpty {
            request.setValue(clientSecret, forHTTPHeaderField: "CF-Access-Client-Secret")
        }
    }
}
