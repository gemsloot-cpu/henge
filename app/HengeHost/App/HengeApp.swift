import SwiftUI

@main
struct HengeApp: App {
    @StateObject private var runner = M1Runner()
    @StateObject private var library = APKLibrary()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(runner)
                .environmentObject(library)
                // Run the M1 self-test at every launch (scripts grep stdout for its summary line);
                // it runs off the main thread so the first frame isn't blocked.
                .task { if runner.state == .idle { runner.run() } }
        }
    }
}
