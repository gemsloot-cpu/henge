import Foundation
import os

/// An APK copied into the app's library.
struct APKFile: Identifiable, Hashable {
    let url: URL
    let size: Int64
    let dateAdded: Date
    var id: URL { url }
    var name: String { url.deletingPathExtension().lastPathComponent }
    var fileName: String { url.lastPathComponent }
}

/// Imported APKs, stored as plain files in Application Support/henge/apks/.
@MainActor
final class APKLibrary: ObservableObject {
    @Published private(set) var apks: [APKFile] = []
    @Published var lastError: String?

    private let log = Logger(subsystem: Bundle.main.bundleIdentifier ?? "henge", category: "library")
    private let fm = FileManager.default

    let directory: URL = {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("henge/apks", isDirectory: true)
    }()

    init() { reload() }

    func reload() {
        try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let keys: [URLResourceKey] = [.fileSizeKey, .creationDateKey, .isRegularFileKey]
        let urls = (try? fm.contentsOfDirectory(at: directory, includingPropertiesForKeys: keys,
                                                options: [.skipsHiddenFiles])) ?? []
        apks = urls.compactMap { url in
            guard let v = try? url.resourceValues(forKeys: Set(keys)), v.isRegularFile == true else { return nil }
            return APKFile(url: url, size: Int64(v.fileSize ?? 0), dateAdded: v.creationDate ?? .distantPast)
        }
        .sorted { $0.dateAdded > $1.dateAdded }
    }

    /// Copies a user-picked file (security-scoped URL from .fileImporter) into the library.
    func importFile(from source: URL) {
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        do {
            try fm.createDirectory(at: directory, withIntermediateDirectories: true)
            let dest = uniqueDestination(for: source.lastPathComponent)
            try fm.copyItem(at: source, to: dest)
            log.log("imported \(dest.lastPathComponent, privacy: .public)")
            reload()
        } catch {
            lastError = "Could not import \(source.lastPathComponent): \(error.localizedDescription)"
        }
    }

    func delete(_ apk: APKFile) {
        do {
            try fm.removeItem(at: apk.url)
        } catch {
            lastError = "Could not delete \(apk.fileName): \(error.localizedDescription)"
        }
        reload()
    }

    private func uniqueDestination(for fileName: String) -> URL {
        var dest = directory.appendingPathComponent(fileName)
        let stem = dest.deletingPathExtension().lastPathComponent
        let ext = dest.pathExtension
        var n = 2
        while fm.fileExists(atPath: dest.path) {
            dest = directory.appendingPathComponent("\(stem) \(n)").appendingPathExtension(ext)
            n += 1
        }
        return dest
    }
}
