import SwiftUI

struct LibraryView: View {
    @EnvironmentObject private var library: APKLibrary
    @State private var importing = false

    var body: some View {
        NavigationStack {
            Group {
                if library.apks.isEmpty {
                    EmptyStateView(
                        title: "No APKs Yet",
                        systemImage: "shippingbox",
                        message: "henge runs the native code inside Android apps on iPhone. "
                            + "It converts each .so to Mach\u{2011}O, signs it, and loads it with a small linker. "
                            + "Import an APK to add it to your library."
                    ) {
                        Button {
                            importing = true
                        } label: {
                            Label("Import APK", systemImage: "square.and.arrow.down")
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.large)
                    }
                } else {
                    List {
                        ForEach(library.apks) { apk in
                            NavigationLink(value: apk) {
                                APKRow(apk: apk)
                            }
                        }
                        .onDelete { offsets in
                            offsets.map { library.apks[$0] }.forEach(library.delete)
                        }
                    }
                }
            }
            .navigationTitle("Library")
            .navigationDestination(for: APKFile.self) { APKDetailView(apk: $0) }
            .toolbar {
                if !library.apks.isEmpty {
                    ToolbarItem(placement: .navigationBarTrailing) {
                        Button {
                            importing = true
                        } label: {
                            Label("Import APK", systemImage: "plus")
                        }
                    }
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.apk],
                          allowsMultipleSelection: true) { result in
                switch result {
                case .success(let urls): urls.forEach(library.importFile)
                case .failure(let error): library.lastError = error.localizedDescription
                }
            }
            .alert("Import Failed", isPresented: Binding(
                get: { library.lastError != nil },
                set: { if !$0 { library.lastError = nil } }
            )) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(library.lastError ?? "")
            }
            .onAppear { library.reload() }
        }
    }
}

private struct APKRow: View {
    let apk: APKFile

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "shippingbox.fill")
                .font(.title2)
                .foregroundStyle(.green)
                .frame(width: 36)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(apk.name)
                    .font(.headline)
                    .lineLimit(1)
                Text("\(apk.size.formatted(.byteCount(style: .file))) · \(apk.dateAdded.formatted(date: .abbreviated, time: .shortened))")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }
}
