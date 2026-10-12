# 先行プロジェクト

| プロジェクト | 内容 | henge が参考にする点 | henge との違い |
| --- | --- | --- | --- |
| [Husk](https://github.com/Leviidev/Husk) | iPhone で Android アプリを動かす。翻訳レイヤー（ネイティブ実行）と QEMU によるエミュレーションの 2 モード | 翻訳レイヤーの設計（ゲームエンジンに絞る、ANGLE / MoltenVK、ページの配置、TLS や x18 の確認項目） | Husk は ELF を JIT 領域に直接マップするので、デバッガ経由の JIT が必須。henge は事前に変換して署名するので JIT が要らない |
| [apk-on-iphone（LiquidAPK）](https://github.com/heyobi/apk-on-iphone) | AArch64 インタプリタで、ART と AOSP のフレームワークを未変更のまま動かす。JIT 不要 | binder と servicemanager の自前実装、system_server なしでサービスを偽装する方法、dex2oat による AOT | LiquidAPK は全命令をインタプリタで実行する（約 80〜90M 命令/秒）。henge はネイティブ速度で実行する |
| [LiveContainer](https://github.com/LiveContainer/LiveContainer) | 1 つのホストアプリの中で、別の iOS アプリを再インストールせずに動かす | オンデバイス署名、SideStore からの証明書の取り込み、コンテナからの `dlopen` | 読み込むのは iOS アプリ。henge は Android の ELF を変換したものを読み込む |
| [SideStore](https://github.com/SideStore/SideStore) / AltStore | PC なしで（初回を除く）iOS アプリをサイドロードし、7 日ごとに再署名する | ホストアプリの配布方法と、証明書の入手方法 | — |
| [Android Translation Layer（ATL）](https://gitlab.com/android_translation_layer/android_translation_layer) | Linux の上で Android アプリを動かす。ART とフレームワークの再実装 | NDK API と JNI の実装範囲、フレームワークの再実装方針 | ATL は Linux（glibc）向け。henge は iOS 向けで、ATL のコードは使わない（GPL-3.0） |
| [libhybris](https://github.com/libhybris/libhybris) | glibc の Linux で、bionic 向けにビルドされた Android の `.so`（主に GPU ドライバ）を読み込む | 自前の ELF リンカで bionic のライブラリを読み込み、libc をシムで差し替える方法 | libhybris は ELF をそのまま mmap できる。henge は Mach-O に変換して署名する必要がある |
| Wine | Windows のプログラムを、OS API の再実装で Linux や macOS 上で動かす | 「CPU は同じで、OS の層だけを差し替える」という考え方 | — |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) / [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | ゲーム機のバイナリを静的再コンパイルして PC で動かす | 事前変換方式でも実用になるという実例 | CPU が違うので命令の翻訳が必要。henge は CPU が同じなので、ファイル形式の変換だけで済む |
