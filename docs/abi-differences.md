# Android と Darwin arm64 の ABI 差分

Android のコード（AAPCS64 + bionic）と henge のシム（Darwin arm64 + libSystem）の境界で問題になる差分の一覧。
シムは Darwin 向けにコンパイルされるので、**Android 側から呼ばれる関数はすべて Android の ABI で引数を受け取る必要がある**。

## 1. 呼び出し規約

| 項目 | Android（AAPCS64） | Darwin arm64 | henge での対処 |
| --- | --- | --- | --- |
| 可変長引数 | 名前付き引数と同じくレジスタ（x0-x7、v0-v7）で渡す | **すべてスタックで渡す** | 可変長関数（`printf`、`open`、`ioctl`、`fcntl` など）は asm の入口でレジスタを保存し、AAPCS 形式の `va_list` を組み立てる |
| `va_list` | 構造体（`__stack`、`__gr_top`、`__vr_top`、`__gr_offs`、`__vr_offs`） | `char *` | `vprintf` 系は AAPCS 形式の `va_list` を読む自前の実装にする |
| `long double` | 128bit（IEEE quad） | 64bit（`double` と同じ） | `strtold`、`%Lf` などは compiler-rt のソフト浮動小数点（`__addtf3` など）で処理する |
| 32bit 未満の整数引数 | 呼ばれる側が拡張する | 呼ぶ側が 32bit に拡張する | `bool`、`char`、`short` を受け取るシム関数は、入口で明示的にマスクまたは拡張する |
| スタック上の引数の配置 | 8 バイト単位 | 型の自然なアラインメントで詰める | 9 個目以降の引数を受け取る関数で問題になる。そうした関数を一覧化して個別に対処する |
| `char` の符号 | 符号なし | 符号あり | 境界をまたぐ `char` は上の「32bit 未満の整数」と同じ扱い |
| x18 | 一般のコードは使わない。Shadow Call Stack を有効にしたコードは使う | プラットフォーム予約。OS が書き換えることがある | 変換器が x18 の使用を検出して警告する。SCS を使う `.so` は対象外 |

シムから Android のコードを呼び戻す（`qsort` の比較関数、`pthread_create` の開始関数、`atexit` など）場合も同じ差分が逆向きに効く。
固定長の引数でポインタと `int` だけなら差分は出ないので、まずはそれ以外を一覧化する。

## 2. TLS とスタック保護

- Android の Clang は、スタック保護の値を `TPIDR_EL0 + 0x28` から読む（bionic の TLS スロット）。
- Darwin は自分のスレッドポインタを `TPIDRRO_EL0` に置いている。`TPIDR_EL0` が空いていて、コンテキストスイッチやシグナルをまたいで保たれるなら、henge が bionic 形式の TLS 領域をそこに置ける。
- 保たれない場合は、変換器が `mrs xN, tpidr_el0` をシムの関数呼び出しに書き換える。署名前に書き換えるので可能。

→ [roadmap.md](roadmap.md) の要検証項目 V1。

ELF の TLS（`__thread` 変数、`R_AARCH64_TLSDESC`）は、henge が bionic と同じ配置の静的 TLS ブロックを自前で管理する（M2）。

## 3. 構造体と定数

| 対象 | 差分 | 対処 |
| --- | --- | --- |
| `pthread_mutex_t` | bionic は 40 バイト、Darwin は 64 バイト | bionic の領域の中に `os_unfair_lock`（4 バイト）と状態を直接置く。bionic の静的初期化子はすべて 0 なので、0 なら未初期化として扱う |
| `pthread_cond_t`、`pthread_rwlock_t` | 同上 | `__ulock_wait` / `__ulock_wake` 相当で自前実装するか、別に確保したものを指すポインタを入れる |
| `struct stat` | フィールドの並びとサイズ | 変換関数を書く |
| `struct dirent` | 同上 | 同上 |
| `struct sigaction` | 同上。`sa_flags` の値も違う | 同上 |
| `errno` | 値が違う（`EAGAIN` は Linux 11 / Darwin 35 など） | 表で変換する。`__errno()` はシムが返す |
| `open` / `mmap` / `fcntl` のフラグ | 値が違う | 表で変換する |
| シグナル番号 | 一部違う（`SIGUSR1` は Linux 10 / Darwin 30 など） | 表で変換する |
| `ucontext_t` / `mcontext_t` | 中身がまったく違う | シグナルハンドラに渡すときに Linux 形式に変換する。クラッシュレポーターが使う（M3 以降） |
| `__sF`（`stdin` / `stdout` / `stderr`） | bionic の古い ABI | シムで `FILE` の配列を公開する |

## 4. スレッドとメモリ

| 対象 | 差分 | 対処 |
| --- | --- | --- |
| スレッドのスタックサイズ | Android は 1 MiB、iOS の 2 本目以降のスレッドは 512 KiB | `pthread_create` のシムで、指定がなければ 1 MiB にする |
| ページサイズ | Android は 4 KiB を想定するコードがある。iOS は 16 KiB | `getpagesize()` / `sysconf(_SC_PAGESIZE)` は 16 KiB を返す。4 KiB を決め打ちしているコードは個別に対処 |
| `gettid`、`futex` などの Linux 固有 API | Darwin に無い | `pthread_threadid_np`、`__ulock_*` などで実装する |
| `/proc/self/*` | Darwin に無い | `/proc/self/maps` などよく読まれるものだけ、それらしい内容を返す |

## 5. 確認方法

- 境界をまたぐ関数ごとに、NDK でビルドしたテスト用 `.so` から呼んで結果を比べる。
- 可変長引数、`long double`、9 個以上の引数、`char` / `short` 引数は、テストを必ず書く。
