import SwiftUI
import UIKit

/// The M1 self-test (mirrors tests/test_m1.c) plus environment facts.
struct DiagnosticsView: View {
    @EnvironmentObject private var runner: M1Runner
    @State private var debuggerAttached = DeviceInfo.debuggerAttached
    @State private var copied = false

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
            List {
                Section {
                    StatusHeader(state: runner.state, report: runner.report, runCount: runner.runCount)
                }

                ForEach(M1Check.Group.allCases, id: \.self) { group in
                    let checks = runner.report.checks.filter { $0.group == group }
                    if !checks.isEmpty {
                        Section(group.rawValue) {
                            ForEach(checks) { CheckRow(check: $0) }
                        }
                    }
                }

                Section {
                    LabeledContent("OS", value: DeviceInfo.osVersion)
                    LabeledContent("Runtime", value: DeviceInfo.runtimeKind)
                    LabeledContent("CPU", value: DeviceInfo.cpuArch)
                    LabeledContent("Debugger") {
                        HStack(spacing: 6) {
                            Image(systemName: debuggerAttached ? "ladybug.fill" : "ladybug")
                                .accessibilityHidden(true)
                            Text(debuggerAttached ? "Attached" : "Detached")
                        }
                        .foregroundStyle(debuggerAttached ? .orange : .secondary)
                    }
                    if let path = runner.report.dylibPath {
                        LabeledContent("Dylib platform",
                                       value: DeviceInfo.machOPlatform(atPath: path) ?? "unknown")
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Dylib path")
                            Text(path)
                                .font(.caption.monospaced())
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                    }
                } header: {
                    Text("Environment")
                } footer: {
                    Text("M1 is only done once the test also passes with the debugger detached.")
                }
                .id("environment")
            }
            // Launch with `-diagnosticsScroll environment` to scroll there once results are in (screenshots).
            .onChange(of: runner.state) { state in
                if state == .finished, let target = UserDefaults.standard.string(forKey: "diagnosticsScroll") {
                    proxy.scrollTo(target, anchor: .top)
                }
            }
            }
            .navigationTitle("Diagnostics")
            .refreshable { rerun() }
            .toolbar {
                ToolbarItemGroup(placement: .navigationBarTrailing) {
                    Menu {
                        ShareLink(item: runner.logText) {
                            Label("Share Log", systemImage: "square.and.arrow.up")
                        }
                        Button {
                            UIPasteboard.general.string = runner.logText
                        } label: {
                            Label("Copy Log", systemImage: "doc.on.doc")
                        }
                    } label: {
                        Label("Log", systemImage: "square.and.arrow.up")
                    }
                    .disabled(runner.state != .finished)

                    Button(action: rerun) {
                        Label("Run Again", systemImage: "arrow.clockwise")
                    }
                    .disabled(runner.state == .running)
                }
            }
        }
    }

    private func rerun() {
        debuggerAttached = DeviceInfo.debuggerAttached
        runner.run()
    }
}

private struct StatusHeader: View {
    let state: M1Runner.State
    let report: M1Report
    let runCount: Int

    var body: some View {
        HStack(spacing: 16) {
            icon
                .font(.system(size: 44))
                .frame(width: 52)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.title3.bold())
                Text(subtitle)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 8)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder private var icon: some View {
        switch state {
        case .idle, .running:
            ProgressView().controlSize(.large)
        case .finished:
            Image(systemName: passed ? "checkmark.seal.fill" : "xmark.seal.fill")
                .foregroundStyle(passed ? .green : .red)
        }
    }

    private var passed: Bool { report.failures == 0 }

    private var title: String {
        guard state == .finished else { return "Running self-test…" }
        let total = report.checks.count
        return "\(total - report.failures)/\(total) passed"
    }

    private var subtitle: String {
        guard state == .finished else { return "M1: libadd.so converted to Mach-O" }
        var s = report.summaryLine
        if runCount > 1 { s += " · run \(runCount)" }
        return s
    }
}

private struct CheckRow: View {
    let check: M1Check

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Image(systemName: check.pass ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(check.pass ? .green : .red)
                .accessibilityLabel(check.pass ? "Passed" : "Failed")
            VStack(alignment: .leading, spacing: 3) {
                Text(check.title)
                    .font(.callout.monospaced().weight(.medium))
                Text(check.detail)
                    .font(.footnote.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(3)
                    .truncationMode(.middle)
                if let expected = check.expected {
                    Text("expected \(expected)")
                        .font(.footnote.monospaced())
                        .foregroundStyle(.red)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }
}
