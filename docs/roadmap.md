# ロードマップ

## マイルストーン

| # | ゴール | 完了条件 | 規模の目安 |
| --- | --- | --- | --- |
| M0 | 環境を作る | Xcode、Android NDK、実機で空のホストアプリが動く。`samples/` で NDK の `.so` がビルドできる | 1〜2 日 |
| M1 | 変換した `.so` の関数を呼ぶ | [m1-elf-to-macho.md](m1-elf-to-macho.md) §7 | 数日〜1 週間 |
| M2 | libc 互換シムとオンデバイス署名 | `malloc`、`printf`、`pthread`、TLS を使う `.so` が動く。iPhone 上で変換から実行まで完結する | 数週間 |
| M3 | NativeActivity を動かす | NDK サンプルの `native-activity` が ANGLE 経由で描画し、タッチに反応する | 1〜2 ヶ月 |
| M4 | 実際のゲームを動かす | Java に依存しない SDL2 または Godot のゲームが 1 本、音声付きで遊べる | 数ヶ月 |
| M5 | ART に対応する | Java だけで書かれた簡単なアプリの Activity が表示される | 年単位 |

## 要検証項目

実機で確かめるまで設計が確定しない項目。M1 と並行して、テスト用のアプリで確認する。

| # | 項目 | 影響する設計 | 確認時期 |
| --- | --- | --- | --- |
| V1 | `TPIDR_EL0` は空いていて、コンテキストスイッチとシグナルをまたいで保たれるか | TLS の実装方法（[abi-differences.md](abi-differences.md) §2） | M1 |
| V2 | XNU は x18 を保つか、壊すか | x18 を使う `.so` を拒否するかどうか | M1 |
| V3 | iPhone 上で署名した dylib をコンテナから `dlopen` できるか（無料 Apple ID の証明書で） | 方式全体の前提 | M2 の最初 |
| V4 | 同じプロセスの中で、新しく署名した dylib を続けて `dlopen` できるか | 実行時に追加されるコードの扱い（[architecture.md](architecture.md) §7） | M2 |
| V5 | libSystem に依存せず、chained fixups が空の dylib を dyld が受け付けるか | Mach-O の組み立て方 | M1 |
| V6 | `increased-memory-limit` の entitlement は無料アカウントで使えるか | 大きいゲームを扱えるか | M3 |
| V7 | dex2oat で暗黙の null チェックとスタックチェックを無効にできるか | ART で SIGSEGV に頼らずに済むか | M5 |

## やらないこと

- 32bit ARM（`armeabi-v7a`）だけの APK
- x86 / x86_64 の `.so`
- Google Play Services の本物の機能（ログイン、課金、クラウドセーブ）
- チート対策や DRM が入ったゲーム
- App Store での配布
