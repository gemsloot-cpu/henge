import UniformTypeIdentifiers

extension UTType {
    /// Android package. Declared as an imported type in Info.plist (UTImportedTypeDeclarations).
    static let apk = UTType(importedAs: "com.android.package-archive", conformingTo: .zip)
}
