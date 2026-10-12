# henge

iPhone 単体で Android アプリ（APK）のネイティブコードを動かす実験プロジェクト。
JIT もデバッガも脱獄も使わず、APK を **iPhone 上で Mach-O に変換し、自分の証明書で署名して読み込む**。

設計: [docs/architecture.md](docs/architecture.md)

## 方針

- **事前変換（AOT）**: Android も iPhone も ARM64 なので、機械語は翻訳しない。ELF を Mach-O に詰め替えて署名するだけで、署名済みのネイティブコードとして実行できる。
- **オンデバイス**: 変換・署名・読み込みを iPhone 上のホストアプリで完結させる。利用者に Mac は要らない（開発には Xcode が要る）。
- **コンテナ型**: LiveContainer と同じく、ホストアプリが変換済み dylib を `dlopen` する。アプリごとの再インストールは不要。
- **範囲を絞る**: まずは Java を使わないネイティブゲーム（NativeActivity / SDL2 / Godot など）。ART（Java/Kotlin）対応は後回し。

## ドキュメント

| パス | 内容 |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | 全体設計、コンポーネント、実行時の流れ |
| [docs/m1-elf-to-macho.md](docs/m1-elf-to-macho.md) | M1: ELF → Mach-O 変換とミニリンカの詳細設計 |
| [docs/abi-differences.md](docs/abi-differences.md) | Android (AAPCS64 / bionic) と Darwin arm64 の差分と対処 |
| [docs/roadmap.md](docs/roadmap.md) | マイルストーンと完了条件、要検証項目 |
| [docs/prior-art.md](docs/prior-art.md) | 先行プロジェクトと henge との違い |

## 予定している構成

まだコードはない。M1 で次の形を作る。

| パス | 内容 |
| --- | --- |
| `convert/` | ELF → Mach-O 変換器（C、依存なし。macOS でもテストできる） |
| `runtime/` | ミニリンカ、bionic 互換シム、Android API シム（C） |
| `app/` | iOS ホストアプリ（SwiftUI）。APK 取り込み・変換・署名・起動 |
| `samples/` | NDK で作るテスト用 `.so` |
| `tests/` | 変換器とミニリンカのテスト（macOS 上で実行） |

## 注意

- 自分でビルドした `.so` や、オープンソース・無料の APK で試すこと。
- 市販アプリを変換したものを配布しない。DRM の回避は目的にしない。
