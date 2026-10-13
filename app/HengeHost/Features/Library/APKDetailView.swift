import SwiftUI

struct APKDetailView: View {
    let apk: APKFile

    var body: some View {
        List {
            Section {
                VStack(spacing: 10) {
                    Image(systemName: "shippingbox.fill")
                        .font(.system(size: 52))
                        .foregroundStyle(.green)
                        .accessibilityHidden(true)
                    Text(apk.name)
                        .font(.title2.bold())
                        .multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
                .listRowBackground(Color.clear)
            }

            Section("File") {
                LabeledContent("Name", value: apk.fileName)
                LabeledContent("Size", value: apk.size.formatted(.byteCount(style: .file)))
                LabeledContent("Imported", value: apk.dateAdded.formatted(date: .long, time: .shortened))
                LabeledContent("Location") {
                    Text("Application Support/henge/apks")
                        .font(.footnote.monospaced())
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.trailing)
                }
            }

            Section {
                Button {} label: {
                    VStack(spacing: 2) {
                        Label("Convert & Run", systemImage: "play.fill")
                        Text("Coming in M2")
                            .font(.caption)
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(true)
                .listRowInsets(EdgeInsets())
                .listRowBackground(Color.clear)
            } footer: {
                Text("M2 will extract lib/arm64-v8a/*.so from the APK, convert and sign it, then launch it.")
                    .frame(maxWidth: .infinity)
                    .multilineTextAlignment(.center)
                    .padding(.top, 4)
            }
        }
        .navigationTitle(apk.name)
        .navigationBarTitleDisplayMode(.inline)
    }
}
