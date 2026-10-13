import Foundation
import os

/// Mirrors tests/test_m1.c: dlopen the bundled converted libadd.dylib, link it with
/// the henge mini linker, and call into the original Android code.
@MainActor
final class M1Runner: ObservableObject {
    struct Result: Identifiable {
        let id = UUID()
        let pass: Bool
        let text: String
    }

    @Published private(set) var results: [Result] = []
    @Published private(set) var failures = 0
    @Published private(set) var summary = "running..."

    private let log = Logger(subsystem: Bundle.main.bundleIdentifier ?? "henge", category: "m1")

    init() { run() }

    private func record(_ pass: Bool, _ text: String) {
        let line = "\(pass ? "PASS" : "FAIL")  \(text)"
        print(line)
        log.log("\(line, privacy: .public)")
        if !pass { failures += 1 }
        results.append(Result(pass: pass, text: text))
    }

    private func checkEq(_ desc: String, _ got: Int, _ want: Int) {
        let pad = desc.padding(toLength: max(desc.count, 28), withPad: " ", startingAt: 0)
        if got == want {
            record(true, "\(pad) = \(got)")
        } else {
            record(false, "\(pad) = \(got) (expected \(want))")
        }
    }

    private func sym(_ img: OpaquePointer, _ name: String) -> UnsafeMutableRawPointer? {
        let p = henge_dlsym(img, name)
        if let p {
            record(true, "henge_dlsym(\"\(name)\") = \(p)")
        } else {
            record(false, "henge_dlsym(\"\(name)\") returned NULL")
        }
        return p
    }

    private func finish() {
        summary = "\(failures == 0 ? "PASS" : "FAIL"): m1 (\(failures) failure\(failures == 1 ? "" : "s"))"
        print(summary)
        log.log("\(self.summary, privacy: .public)")
        fflush(stdout)
    }

    private func run() {
        defer { finish() }
        guard let fw = Bundle.main.privateFrameworksPath else {
            record(false, "no Frameworks path")
            return
        }
        let path = fw + "/libadd.dylib"

        guard dlopen(path, RTLD_NOW | RTLD_LOCAL) != nil else {
            record(false, "dlopen(\(path)): \(String(cString: dlerror()))")
            return
        }
        record(true, "dlopen(\(path))")

        var err = [CChar](repeating: 0, count: 512)
        guard let img = henge_link(path, &err, err.count) else {
            let msg = String(cString: err)
            record(false, "henge_link: \(msg.isEmpty ? "(no message)" : msg)")
            return
        }
        record(true, "henge_link")

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

        checkEq("add(2, 3)", Int(add(2, 3)), 5)
        checkEq("was_initialized()", Int(wasInitialized()), 1)
        checkEq("*counter (initial)", Int(counter.pointee), 40)
        checkEq("bump() #1", Int(bump()), 41)
        checkEq("bump() #2", Int(bump()), 42)
        checkEq("*counter (after bumps)", Int(counter.pointee), 42)
        checkEq("apply(0, 7, 5) [add]", Int(apply(0, 7, 5)), 12)
        checkEq("apply(1, 7, 5) [sub]", Int(apply(1, 7, 5)), 2)
    }
}
