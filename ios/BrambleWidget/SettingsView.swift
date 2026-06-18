import SwiftUI
import WidgetKit

/// The host app's only screen. Configures the API connection (base URL + secrets) and
/// the list of valves the widget shows. Secrets are written to the Keychain; the base
/// URL and valve list to the shared App Group defaults so the widget can read them.
struct SettingsView: View {
    @State private var baseUrl: String = AppSettings.baseUrl
    @State private var apiToken: String = KeychainStore.get(.apiToken) ?? ""
    @State private var cfClientId: String = KeychainStore.get(.cfClientId) ?? ""
    @State private var cfClientSecret: String = KeychainStore.get(.cfClientSecret) ?? ""
    @State private var valves: [ValveConfig] = AppSettings.valves
    @State private var connectionResult: String?
    @State private var isTesting: Bool = false

    var body: some View {
        NavigationStack {
            Form {
                Section("Connection") {
                    TextField("Base URL", text: $baseUrl)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                    SecureField("API Token (Bearer)", text: $apiToken)
                    SecureField("CF-Access-Client-Id", text: $cfClientId)
                    SecureField("CF-Access-Client-Secret", text: $cfClientSecret)
                    Button(action: testConnection) {
                        HStack {
                            Text("Test Connection")
                            if isTesting { ProgressView().padding(.leading, 4) }
                        }
                    }
                    .disabled(isTesting)
                    if let connectionResult {
                        Text(connectionResult).font(.footnote).foregroundStyle(.secondary)
                    }
                }

                Section("Valves") {
                    ForEach($valves) { $valve in
                        ValveEditor(valve: $valve)
                    }
                    .onDelete { valves.remove(atOffsets: $0) }
                    Button {
                        valves.append(ValveConfig(label: "New Valve", deviceId: "", valve: 0))
                    } label: {
                        Label("Add Valve", systemImage: "plus")
                    }
                }
            }
            .navigationTitle("Bramble Valves")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save", action: save)
                }
            }
        }
    }

    private func save() {
        AppSettings.baseUrl = baseUrl.trimmingCharacters(in: .whitespaces)
        AppSettings.valves = valves
        KeychainStore.set(apiToken, for: .apiToken)
        KeychainStore.set(cfClientId, for: .cfClientId)
        KeychainStore.set(cfClientSecret, for: .cfClientSecret)
        WidgetCenter.shared.reloadAllTimelines()
    }

    private func testConnection() {
        save()
        isTesting = true
        connectionResult = nil
        Task {
            do {
                try await BrambleAPIClient().checkHealth()
                connectionResult = "Connected ✓"
            } catch {
                connectionResult = error.localizedDescription
            }
            isTesting = false
        }
    }
}

struct ValveEditor: View {
    @Binding var valve: ValveConfig

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            TextField("Label", text: $valve.label)
            TextField("Device ID", text: $valve.deviceId)
                .keyboardType(.numberPad)
            Stepper("Valve index: \(valve.valve)", value: $valve.valve, in: 0...99)
            Stepper(
                "Duration: \(valve.defaultDurationSeconds / 60) min",
                value: $valve.defaultDurationSeconds, in: 60...7200, step: 60)
        }
        .padding(.vertical, 4)
    }
}
