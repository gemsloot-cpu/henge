import Foundation
import MachO
import UIKit

/// Facts about the process and device shown in Diagnostics > Environment.
enum DeviceInfo {
    static var osVersion: String {
        "\(UIDevice.current.systemName) \(UIDevice.current.systemVersion)"
    }

    static var isSimulator: Bool {
        #if targetEnvironment(simulator)
        return true
        #else
        return false
        #endif
    }

    static var runtimeKind: String {
        if isSimulator {
            let model = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"]
            return model.map { "Simulator (\($0))" } ?? "Simulator"
        }
        return "Device (\(machine))"
    }

    static var cpuArch: String {
        #if arch(arm64)
        return "arm64"
        #elseif arch(x86_64)
        return "x86_64"
        #else
        return "unknown"
        #endif
    }

    /// `uname -m`: hardware model on device (e.g. iPhone17,1), arch on the Simulator.
    static var machine: String {
        var u = utsname()
        uname(&u)
        return withUnsafeBytes(of: &u.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }

    /// True if a debugger is attached (P_TRACED). M1 must also pass with it detached.
    static var debuggerAttached: Bool {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&mib, u_int(mib.count), &info, &size, nil, 0) == 0 else { return false }
        return (info.kp_proc.p_flag & P_TRACED) != 0
    }

    /// Platform from the LC_BUILD_VERSION of a Mach-O file, e.g. "iOS Simulator".
    static func machOPlatform(atPath path: String) -> String? {
        guard let fh = FileHandle(forReadingAtPath: path) else { return nil }
        defer { try? fh.close() }
        guard let data = try? fh.read(upToCount: 64 * 1024), data.count >= 32 else { return nil }
        return data.withUnsafeBytes { raw -> String? in
            guard raw.load(as: UInt32.self) == MH_MAGIC_64 else { return nil }
            let ncmds = raw.load(fromByteOffset: 16, as: UInt32.self)
            var off = 32 // sizeof(mach_header_64)
            for _ in 0..<ncmds {
                guard off + 16 <= raw.count else { return nil }
                let cmd = raw.loadUnaligned(fromByteOffset: off, as: UInt32.self)
                let size = Int(raw.loadUnaligned(fromByteOffset: off + 4, as: UInt32.self))
                if cmd == UInt32(LC_BUILD_VERSION) {
                    let p = raw.loadUnaligned(fromByteOffset: off + 8, as: UInt32.self)
                    return platformName(p)
                }
                guard size > 0 else { return nil }
                off += size
            }
            return nil
        }
    }

    private static func platformName(_ p: UInt32) -> String {
        switch p {
        case 1: return "macOS"
        case 2: return "iOS"
        case 7: return "iOS Simulator"
        default: return "platform \(p)"
        }
    }
}
