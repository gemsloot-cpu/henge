# 設計

## 1. 目的と前提

iPhone 上で Android アプリのネイティブコード（`lib/arm64-v8a/*.so`）を、ネイティブ速度で動かす。

iOS の制約:

- 実行時にコードを生成して実行する（RWX / JIT）のは、デバッガ接続時を除いて禁止。
- **署名済みのコードなら実行できる**。ホストアプリと同じ Team ID で署名された dylib は `dlopen` できる（LiveContainer が実証済み）。

Android と iPhone の共通点:

- どちらも ARM64。`.so` の機械語は iPhone の CPU でそのまま動く。
- 違うのはファイル形式（ELF / Mach-O）、libc（bionic / libSystem）、ABI の細部、OS API。

よって henge は **命令を翻訳しない**。ELF を Mach-O に詰め替え、iPhone 上で署名し、その下に Android の実行環境（bionic・Android API）の互換レイヤーを敷く。

### 他方式との比較

| 方式 | 例 | 速度 | iOS 更新への強さ | 欠点 |
| --- | --- | --- | --- | --- |
| JIT で ELF を直接マップ | Husk | ◎ | × | デバッガ経由の JIT に依存。Apple が塞げば終わる |
| インタプリタ | LiquidAPK | △ | ◎ | 遅い（約 80〜90M 命令/秒） |
| CPU エミュレーション | Husk (QEMU) | × | ○ | 非常に遅い |
| **事前変換 + 署名（henge）** | — | ◎ | ◎ | 実行時に生成・ダウンロードされるコードに弱い（§7） |

## 2. 全体像

```
┌──────────────────────── henge ホストアプリ（SideStore 等で 1 回だけインストール） ────────────────────────┐
│                                                                                                         │
│  ① 取り込み        ② 変換                ③ 署名                 ④ 読み込み            ⑤ 実行             │
│  APK を受け取る → ELF → Mach-O dylib → 自分の証明書で署名 → dlopen + ミニリンカ → NativeActivity 起動  │
│  (共有メニュー)    (henge-convert)        (オンデバイス署名)      (relocation, init)   (シム経由で UIKit 等) │
│                                                                                                         │
│  ┌──────────────────────────────── runtime ────────────────────────────────┐                            │
│  │ libhenge-bionic   libc / libm / libdl / liblog 互換                     │                            │
│  │ libhenge-android  NativeActivity / AAsset / AInput / ANativeWindow / JNI │                            │
│  │ EGL / GLES        ANGLE (Metal バックエンド)                              │                            │
│  │ Audio             OpenSL ES / AAudio → AVAudioEngine                     │                            │
│  └─────────────────────────────────────────────────────────────────────────┘                            │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### コンポーネント

| コンポーネント | 言語 | 役割 |
| --- | --- | --- |
| `henge-convert` | C | ELF `.so` を Mach-O dylib に変換する。テキストのパッチ（svc、TLS レジスタ）もここで行う。iPhone 上と macOS 上の両方で動かす |
| 署名器 | C/Swift | 変換した dylib に、ホストアプリと同じ証明書で署名する。既存実装（zsign 系）を候補にする。ライセンスは要確認 |
| ミニリンカ | C | `dlopen` 後に ELF の動的情報を読み、relocation を解決し、初期化関数を呼ぶ |
| `libhenge-bionic` | C + asm | bionic の関数名を公開し、Darwin の libSystem に橋渡しする。ABI 差分を吸収する（[abi-differences.md](abi-differences.md)） |
| `libhenge-android` | C/ObjC | NDK の API（`libandroid.so` など）を UIKit / Metal / AVFoundation で実装する |
| ホストアプリ | Swift | APK 管理 UI、変換と署名の実行、ゲーム画面のホスト |

## 3. 変換（henge-convert）

詳細は [m1-elf-to-macho.md](m1-elf-to-macho.md)。要点:

1. ELF の `PT_LOAD` セグメントを、**相対位置を保ったまま** Mach-O の `__TEXT`（R / RX）と `__DATA`（RW）に詰め替える。Android の `.so` は位置独立コードで、`adrp` によるページ相対参照を使うため、相対位置さえ保てばどこに読み込まれても動く。
2. Mach-O ヘッダ分（16 KiB）だけ全体をずらす。4 KiB の倍数のずれなら `adrp` の結果は変わらない。
3. コード部分のパッチ（署名前にやるので自由に書き換えられる）:
   - Linux の `svc #0` → シムの syscall スタブへの `bl`
   - `mrs xN, tpidr_el0` → 必要ならシム経由の TLS 取得に置き換える（要検証項目の結果次第）
4. ELF の動的情報（`.dynamic`、`.dynsym`、relocation 表）はそのまま残す。dyld には触らせず、ミニリンカが読む。
5. Mach-O には初期化関数を登録しない。ELF の `DT_INIT` / `DT_INIT_ARRAY` は relocation 後にミニリンカが呼ぶ。

**変換できない条件（M1 では拒否する）**

- コードとデータが同じ 16 KiB ページに乗っている（4 KiB 境界でリンクされた古い `.so`）
- `DT_TEXTREL` がある（API 23 以降の Android では禁止されているので、まず無い）
- 32bit ARM しか入っていない APK

Google Play は 2025 年 11 月以降、Android 15 以降を対象にした新規アプリとアップデートに 16 KiB ページ対応を必須にしている。新しい APK ほど変換できる見込みが高い。

## 4. 署名

- 証明書と秘密鍵は、SideStore / AltStore がインストール時に使ったものを取り込む（LiveContainer と同じ方式）。
- 変換した dylib をアプリのコンテナ（`Library/Application Support/henge/<package>/`）に置き、その場で署名する。
- ホストアプリと dylib の Team ID が一致するので、library validation を通る。
- 無料 Apple ID の証明書は 7 日で失効するが、署名し直すのはホストアプリだけでよい。変換済み dylib も同じ証明書で署名し直す（ホストアプリ起動時に自動で行う）。

## 5. 読み込みと実行

1. **NEEDED の解決**: ELF の `DT_NEEDED` を見て、依存ライブラリを先に読み込む。
   - APK 同梱のもの（`libc++_shared.so`、`libmain.so` など）→ 変換済み dylib
   - システムのもの（`libc.so`、`libm.so`、`libdl.so`、`liblog.so`、`libandroid.so`、`libEGL.so`、`libGLESv2.so`、`libGLESv3.so`、`libOpenSLES.so`、`libaaudio.so`、`libz.so`）→ henge のシム
2. **dlopen**: 変換済み dylib を `dlopen`。dyld はコードとデータをマップし、署名を検証するだけで、ELF の中身は解釈しない。
3. **ベースアドレスの取得**: dyld のイメージ一覧（`_dyld_get_image_header`）から、パスで自分の dylib を探す。
4. **relocation**: ミニリンカが `__DATA` 上の GOT やポインタを書き換える。シンボルは「シム → 読み込み済みの dylib」の順で探す（ELF のグローバルな探索順に合わせる）。
5. **初期化**: `DT_INIT` → `DT_INIT_ARRAY` の順に呼ぶ。
6. **起動**: `ANativeActivity_onCreate`（NativeActivity）または `SDL_main` などのエントリを呼ぶ。

アプリが実行中に `dlopen("libfoo.so")` した場合は、シムの `dlopen` が上と同じ手順で変換済み dylib を返す。

## 6. Android 互換レイヤー

### bionic（libc）

- bionic の関数名で公開し、中身は Darwin の libSystem を呼ぶ。
- 構造体の差分（`stat`、`dirent`、`sigaction`、`pthread_mutex_t` など）、定数の差分（`errno`、`open` / `mmap` のフラグ、シグナル番号）を変換する。
- **ABI の差分が一番の落とし穴**。可変長引数、`va_list`、`long double` などは Darwin とまったく違う。詳細は [abi-differences.md](abi-differences.md)。

### NDK API

| Android | iOS 側の実装 |
| --- | --- |
| `ANativeActivity` とライフサイクルのコールバック | `UIViewController` のライフサイクル |
| `ANativeWindow` | `CAMetalLayer` を持つ `UIView` |
| EGL / GLES 2・3 | ANGLE（Metal バックエンド）。`EGLNativeWindowType` に `CAMetalLayer` を渡す |
| Vulkan | MoltenVK（M4 以降） |
| `AInputQueue` / `AInputEvent` | `UITouch`、`GCController` |
| `AAssetManager` | 展開した APK の `assets/` を読む |
| OpenSL ES / AAudio | `AVAudioEngine` / AudioUnit |
| `__android_log_print` | `os_log` |
| `AConfiguration`、ディスプレイ情報 | `UIScreen`、`UITraitCollection` |

### JNI

M1〜M4 では ART を持たない。`JavaVM` / `JNIEnv` は **偽物の関数テーブル** を渡す。

- よく使われる呼び出し（`GetStaticMethodID` でディスプレイ解像度や言語を取るなど）は、henge 側で値を返す。
- 知らない呼び出しはログに出して、無難な値（`NULL`、0）を返す。ログが、次に何を実装すべきかの一覧になる。

ART は M5 で扱う（§8）。

## 7. 実行時に追加されるコード

ネイティブコードを後からダウンロードするゲームは、事前変換だけでは動かない。ただし henge は署名器を iPhone 上に持っているので、次の方法が考えられる（要検証）。

- シムの `dlopen` が未変換の ELF を受け取ったら、その場で変換・署名して読み込む。
- 署名済みファイルを同じプロセスで `dlopen` できるなら、再起動もいらない。

自己展開する packer や、メモリ上でコードを生成する DRM は対象外。

## 8. ART（Java / Kotlin）への拡張（M5）

- ART を `arm64-apple-ios` 向けにビルドし、ホストアプリに組み込む。
- `dex2oat` も iOS 向けにビルドし、**取り込み時に iPhone 上で** dex を AOT コンパイルする。出力の oat ファイルは ELF なので、`.so` と同じく Mach-O に変換して署名する。
- 実行時の JIT は無効にする（`-Xusejit:false`）。AOT されていないメソッドはインタプリタで動く。
- ART は null チェックやスタックオーバーフローの検出に SIGSEGV を使う。iOS で同じことをするより、dex2oat で暗黙チェックを無効にする方向で検討する（要検証）。
- フレームワーク（`android.app` など）は、未変更の AOSP `framework.jar` に binder やシステムサービスを偽装して動かす（LiquidAPK 方式）か、ATL のように再実装するかを M5 の開始時に決める。

## 9. 制約とリスク

| リスク | 影響 | 対策 |
| --- | --- | --- |
| Apple が自前署名 dylib の `dlopen` を塞ぐ | 方式そのものが成立しない | LiveContainer など同方式のプロジェクトの動向を見る。最悪の場合はインタプリタに切り替え |
| メモリ上限（jetsam） | 大きいゲームが落ちる | `increased-memory-limit` が無料アカウントで使えるか確認する（要検証） |
| ABI 差分の見落とし | 原因の分かりにくいクラッシュ | 境界をまたぐ関数を一覧化し、テストを書く（[abi-differences.md](abi-differences.md)） |
| Google Play Services 依存 | ログインや課金が動かない | 範囲外。偽の GMS で「未ログイン」として起動だけ通す |
| ライセンス | ANGLE（BSD）、MoltenVK（Apache-2.0）は問題ない。署名器や ART を取り込むときに確認する | コンポーネントを追加するたびに確認する |
