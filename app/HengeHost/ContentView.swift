import SwiftUI

struct ContentView: View {
    @ObservedObject var runner: M1Runner

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Text(runner.summary)
                        .font(.headline)
                        .foregroundStyle(runner.failures == 0 ? .green : .red)
                }
                Section("M1a: libadd.dylib") {
                    ForEach(runner.results) { r in
                        HStack(alignment: .firstTextBaseline) {
                            Text(r.pass ? "PASS" : "FAIL")
                                .font(.system(.body, design: .monospaced).bold())
                                .foregroundStyle(r.pass ? .green : .red)
                            Text(r.text)
                                .font(.system(.footnote, design: .monospaced))
                        }
                    }
                }
            }
            .navigationTitle("henge")
        }
    }
}
