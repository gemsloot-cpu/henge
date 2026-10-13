import Foundation
import os

/// One line of the M1 self-test, mirroring a PASS/FAIL line of tests/test_m1.c.
struct M1Check: Identifiable, Sendable {
    enum Group: String, CaseIterable, Sendable {
        case load = "Load"
        case symbols = "Symbols"
        case calls = "Calls"
    }

    let id = UUID()
    let group: Group
    let pass: Bool
    /// Short label, e.g. `add(2, 3)` or `henge_dlsym("add")`.
    let title: String
    /// Observed value / pointer / error message.
    let detail: String
    /// Expected value, set only for failed value checks.
    let expected: String?
    /// The exact line written to stdout and os Logger.
    let logLine: String
}

/// Result of one full run.
struct M1Report: Sendable {
    var checks: [M1Check] = []
    var dylibPath: String?
    var finalCounter: Int32?
    var failures: Int { checks.filter { !$0.pass }.count }
    /// Exactly `PASS: m1 (0 failures)` / `FAIL: m1 (N failures)`; scripts grep for it.
    var summaryLine: String {
        let n = failures
        return "\(n == 0 ? "PASS" : "FAIL"): m1 (\(n) failure\(n == 1 ? "" : "s"))"
    }
}

/// Mirrors tests/test_m1.c: dlopen the bundled converted libadd.dylib, link it with
/// the henge mini linker, and call into the original Android code.
/// The test itself runs off the main thread; results are published on the main actor.
@MainActor
final class M1Runner: ObservableObject {
    enum State { case idle, running, finished }

    @Published private(set) var state: State = .idle
    @Published private(set) var report = M1Report()
    @Published private(set) var runCount = 0

    /// `*counter` after the previous run. libadd stays loaded (dyld and henge_link both
    /// cache the image), so a re-run in the same process starts from where the last left off.
    private var carriedCounter: Int32?

    var logText: String {
        (report.checks.map(\.logLine) + (state == .finished ? [report.summaryLine] : []))
            .joined(separator: "\n")
    }

    func run() {
        guard state != .running else { return }
        state = .running
        report = M1Report()
        let startCounter = carriedCounter
        Task.detached(priority: .userInitiated) {
            let result = M1Session(startCounter: startCounter).run()
            await MainActor.run {
                self.report = result
                self.carriedCounter = result.finalCounter ?? self.carriedCounter
                self.runCount += 1
                self.state = .finished
            }
        }
    }
}

/// Performs one run synchronously on the calling (background) thread.
private final class M1Session {
    private static let log = Logger(subsystem: Bundle.main.bundleIdentifier ?? "henge", category: "m1")

    private var report = M1Report()
    private let startCounter: Int32?

    init(startCounter: Int32?) { self.startCounter = startCounter }

    private func emit(_ line: String) {
        print(line)
        Self.log.log("\(line, privacy: .public)")
    }

    private func record(_ group: M1Check.Group, _ pass: Bool, title: String, detail: String,
                        expected: String? = nil, line: String) {
        let logLine = "\(pass ? "PASS" : "FAIL")  \(line)"
        emit(logLine)
        report.checks.append(M1Check(group: group, pass: pass, title: title, detail: detail,
                                     expected: expected, logLine: logLine))
    }

    private func checkEq(_ desc: String, _ got: Int32, _ want: Int32) {
        // Same as printf("%-28s = %ld") in test_m1.c.
        let pad = desc.padding(toLength: max(desc.count, 28), withPad: " ", startingAt: 0)
        if got == want {
            record(.calls, true, title: desc, detail: "\(got)", line: "\(pad) = \(got)")
        } else {
            record(.calls, false, title: desc, detail: "\(got)", expected: "\(want)",
                   line: "\(pad) = \(got) (expected \(want))")
        }
    }

    private func sym(_ img: OpaquePointer, _ name: String) -> UnsafeMutableRawPointer? {
        let title = "henge_dlsym(\"\(name)\")"
        if let p = henge_dlsym(img, name) {
            record(.symbols, true, title: title, detail: "\(p)", line: "\(title) = \(p)")
            return p
        }
        record(.symbols, false, title: title, detail: "returned NULL", line: "\(title) returned NULL")
        return nil
    }

    func run() -> M1Report {
        body()
        emit(report.summaryLine)
        fflush(stdout)
        return report
    }

    private func body() {
        guard let fw = Bundle.main.privateFrameworksPath else {
            record(.load, false, title: "dlopen", detail: "no Frameworks path", line: "no Frameworks path")
            return
        }
        let path = fw + "/libadd.dylib"
        report.dylibPath = path

        guard dlopen(path, RTLD_NOW | RTLD_LOCAL) != nil else {
            let msg = String(cString: dlerror())
            record(.load, false, title: "dlopen", detail: msg, line: "dlopen(\(path)): \(msg)")
            return
        }
        record(.load, true, title: "dlopen", detail: "henge.app/Frameworks/libadd.dylib", line: "dlopen(\(path))")

        var err = [CChar](repeating: 0, count: 512)
        guard let img = henge_link(path, &err, err.count) else {
            let msg = String(cString: err)
            let shown = msg.isEmpty ? "(no message)" : msg
            record(.load, false, title: "henge_link", detail: shown, line: "henge_link: \(shown)")
            return
        }
        record(.load, true, title: "henge_link", detail: "relocated, initializers run", line: "henge_link")

        typealias II_I = @convention(c) (Int32, Int32) -> Int32
        typealias V_I = @convention(c) () -> Int32
        typealias III_I = @convention(c) (Int32, Int32, Int32) -> Int32

        let pAdd = sym(img, "add")
        let pBump = sym(img, "bump")
        let pApply = sym(img, "apply")
        let pInit = sym(img, "was_initialized")
        let pCounter = sym(img, "counter")
        guard let pAdd, let pBump, let pApply, let pInit, let pCounter else { return }

        let add = unsafeBitCast(pAdd, to: II_I.self)
        let bump = unsafeBitCast(pBump, to: V_I.self)
        let apply = unsafeBitCast(pApply, to: III_I.self)
        let wasInitialized = unsafeBitCast(pInit, to: V_I.self)
        let counter = pCounter.assumingMemoryBound(to: Int32.self)

        let c0 = startCounter ?? 40
        checkEq("add(2, 3)", add(2, 3), 5)
        checkEq("was_initialized()", wasInitialized(), 1)
        checkEq("*counter (initial)", counter.pointee, c0)
        checkEq("bump() #1", bump(), c0 + 1)
        checkEq("bump() #2", bump(), c0 + 2)
        checkEq("*counter (after bumps)", counter.pointee, c0 + 2)
        checkEq("apply(0, 7, 5) [add]", apply(0, 7, 5), 12)
        checkEq("apply(1, 7, 5) [sub]", apply(1, 7, 5), 2)
        report.finalCounter = counter.pointee
    }
}
