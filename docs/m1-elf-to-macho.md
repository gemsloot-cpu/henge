# M1: ELF → Mach-O 変換とミニリンカ

M1 のゴール: NDK でビルドした `libadd.so`（`int add(int, int)` だけを持つ）を変換し、iPhone 上の
ホストアプリで呼び出して結果を画面に出す。**JIT もデバッガも使わない**ことを確認する。

## 1. 入力の前提

- ELF64、`EM_AARCH64`、`ET_DYN`
- `-Wl,-z,max-page-size=16384` でリンクされている（NDK r27 以降は標準で 16 KiB）
- `DT_TEXTREL` がない

テスト用の `.so` は自分でビルドするので、M1 ではこの前提を満たせる。

## 2. 典型的な ELF のレイアウト

```
vaddr
0x00000  PT_LOAD  R    .dynsym .dynstr .gnu.hash .rela.dyn .rodata ...
0x04000  PT_LOAD  R X  .text .plt
0x08000  PT_LOAD  RW   .data.rel.ro .dynamic .got .got.plt   ← PT_GNU_RELRO
0x0C000  PT_LOAD  RW   .data .bss
```

（16 KiB 境界でリンクすると、各 `PT_LOAD` は 0x4000 の倍数から始まる）

## 3. 出力する Mach-O のレイアウト

```
vmaddr (dylib 内の相対)
0x00000  __TEXT      r-x   Mach-O ヘッダとロードコマンド（16 KiB 確保）
0x04000                    ELF の R と RX セグメント（vaddr + 0x4000）
0x0C000  __DATA      rw-   ELF の RW セグメント（vaddr + 0x4000）、bss 分は vmsize > filesize
0x14000  __LINKEDIT  r--   シンボル表、文字列表、コード署名
```

- **全体を `H = 0x4000` だけずらす**。ELF の vaddr `V` は Mach-O の vmaddr `V + H` になる。
  `H` が 4 KiB の倍数なので、`adrp`（4 KiB ページ相対）の計算結果は変わらない。
- `__TEXT` はファイルオフセット 0 から始まる必要がある（Mach-O ヘッダを含むため）。
- ELF の R と RX はどちらも書き込み不可なので、まとめて `__TEXT`（r-x）に入れる。読み取り専用データが実行可能になるだけで、動作に影響はない。
- ELF の RELRO 領域（`.data.rel.ro`、`.got`）は、relocation 後も `__DATA` のまま書き込み可能にしておく。M1 では保護しない（ELF では relocation 後に読み取り専用にする）。
- 隣り合う `PT_LOAD` の隙間はゼロで埋め、相対位置を保つ。

### ロードコマンド

| ロードコマンド | 内容 |
| --- | --- |
| `LC_SEGMENT_64` × 3 | `__TEXT`、`__DATA`、`__LINKEDIT` |
| `LC_ID_DYLIB` | `@rpath/<package>/libadd.dylib` |
| `LC_BUILD_VERSION` | platform = iOS、minos = 16.0 |
| `LC_SYMTAB` / `LC_DYSYMTAB` | 目印のシンボル `_henge_image_marker` を 1 つだけ公開する |
| `LC_DYLD_CHAINED_FIXUPS` | 空。dyld に relocation させない |
| `LC_UUID` | ELF の build-id から作る |
| `LC_CODE_SIGNATURE` | 署名器が追加する。変換器は `__LINKEDIT` に場所だけ空けておく |

dyld はこの dylib を「依存なし、fixup なしのデータの塊」として読み込むだけになる。

> **要検証**: `LC_LOAD_DYLIB` で libSystem に依存しない dylib を dyld が受け付けるか。だめなら
> `/usr/lib/libSystem.B.dylib` への依存を形だけ入れる。

## 4. 変換手順（henge-convert）

```
1. ELF ヘッダと PT_LOAD、PT_DYNAMIC を読む
2. 前提を確認する（arm64、ET_DYN、TEXTREL なし、RX と RW が別の 16 KiB ページ）
3. コードをスキャンしてパッチを当てる
     svc #0           → bl __henge_svc_stub（変換器が __TEXT の末尾にスタブを足す）
     mrs xN, tpidr_el0 → M1 ではスキャンして数えるだけ（要検証項目の結果を待つ）
     x18 の使用       → 警告を出す
4. Mach-O を組み立てる（§3）
5. マニフェストを出力する（元の ELF の情報、H、NEEDED、パッチの一覧）→ manifest.json
```

`bl` の届く範囲は ±128 MiB。これより大きい `.so` は M1 では扱わない。

## 5. ミニリンカ

`dlopen` の後、ホストアプリから `henge_link(path)` を呼ぶ。

```
1. _dyld_image_count / _dyld_get_image_name でパスが一致するイメージを探し、
   ヘッダのアドレスを得る
2. base = header - H          （ELF の vaddr 0 に当たるアドレス）
3. base + PT_DYNAMIC の vaddr から .dynamic を読む
4. DT_NEEDED を先に読み込む（M1 は libc.so だけ。中身はシム）
5. relocation を処理する
6. DT_INIT → DT_INIT_ARRAY を呼ぶ
7. henge_dlsym(handle, "add") で .dynsym と DT_GNU_HASH を引く
```

### 処理する relocation の種類

| 種類 | 値 | 処理 |
| --- | --- | --- |
| `R_AARCH64_RELATIVE` | 1027 | `*P = base + A` |
| `R_AARCH64_GLOB_DAT` | 1025 | `*P = S + A` |
| `R_AARCH64_JUMP_SLOT` | 1026 | `*P = S`（遅延バインドはせず、すべて即時解決） |
| `R_AARCH64_ABS64` | 257 | `*P = S + A` |
| `R_AARCH64_TLS_TPREL64` | 1030 | M2 で対応 |
| `R_AARCH64_TLSDESC` | 1031 | M2 で対応 |
| `R_AARCH64_IRELATIVE` | 1032 | M2 で対応（ifunc のリゾルバを呼ぶ） |

relocation 表の形式:

| 形式 | 動的タグ | 対応時期 |
| --- | --- | --- |
| 通常の RELA | `DT_RELA` / `DT_JMPREL` | M1 |
| RELR | `DT_RELR`（Android API 28 以降、NDK が使う） | M1 |
| Android packed relocations（APS2） | `DT_ANDROID_RELA` | M2 |

すべての書き込み先 `P` が `__DATA` の範囲に入ることを確認し、外れたら読み込みを中止する。

## 6. 署名

M1 では Mac 上で `codesign` を使ってよい（Xcode のビルドでアプリに同梱する）。オンデバイス署名は M2 で入れる。

| 段階 | dylib の置き場所 | 署名 |
| --- | --- | --- |
| M1a | アプリバンドル内（`Frameworks/`） | Xcode が署名する |
| M1b | コンテナの `Library/` にコピー | Mac で署名したものをコピーする（コンテナからの `dlopen` を確認する） |
| M2 | コンテナの `Library/` | iPhone 上で署名する |

## 7. 完了条件

- [ ] `libadd.so` が `henge-convert` で変換できる
- [ ] macOS 上のテストで、変換した dylib のレイアウト（H のずれ、セグメントの権限）を確認できる
- [ ] iPhone 上で `dlopen` → `henge_link` → `add(2, 3)` が 5 を返す
- [ ] グローバル変数を参照する関数（`adrp` + GOT 経由）も正しく動く
- [ ] Xcode のデバッガを外して起動しても動く（JIT を使っていないことの確認）
