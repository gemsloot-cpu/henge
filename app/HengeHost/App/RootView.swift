import SwiftUI

struct RootView: View {
    enum Tab: String { case library, diagnostics }

    /// Launch with `-initialTab diagnostics` to open on the Diagnostics tab (used for screenshots).
    @State private var tab: Tab = UserDefaults.standard.string(forKey: "initialTab").flatMap(Tab.init) ?? .library

    var body: some View {
        TabView(selection: $tab) {
            LibraryView()
                .tabItem { Label("Library", systemImage: "square.stack.3d.up") }
                .tag(Tab.library)
            DiagnosticsView()
                .tabItem { Label("Diagnostics", systemImage: "stethoscope") }
                .tag(Tab.diagnostics)
        }
    }
}
