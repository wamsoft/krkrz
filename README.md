# 吉里吉里Z multi platform

## 概要

マルチプラットフォーム展開を想定した吉里吉里Zです

・システム基本制御は SDL3 を使います
・OpenGLベース描画機構を持ちます Canvas/Screen/Texture/Shader
・極力外部ライブラリを参照する形で構築されています。

外部ライブラリの参照には vcpkg を利用しています。
SDL3 は最新版を利用する関係で FettchContents で処理されます。

## 開発環境準備

### Windows

Windows用に Visual Studio をインストールして
C++ コンパイラ を使える状態にしておきます。

あわせて Visual Studio 付属の Cmake / Ninja を利用します。

make を使いたい場合は、msys2 をインストールして基礎開発ツールを導入しておきます。

```bash
pacman -S base-devel
```

### Linux

整備中

### OSX

整備中

### vcpkg 環境準備

各環境に vcpkg を導入します。

※Visual Studio 2022 以降は vcpkg があわせて導入されます。
自前環境を使う場合は競合してまうのでどちらかでいれるようにしてください。

https://learn.microsoft.com/ja-jp/vcpkg/get_started/overview

vcpkg のフォルダを環境変数 VCPKG_ROOT に設定しておきます。

```bash
# dos
set VCPKG_ROOT="c:\work\vcpkg"

# msys/cygwin
export VCPKG_ROOT='c:\work\vcpkg'
```

## ビルド

### ソースのチェックアウト

git clone 後 submodule 更新しておいてください

```
git submodule update --init
```

### ビルド

CMakePresets.json 中のプリセット定義をつかってビルドします。
必要なライブラリは vcpkg.json によってセットアップされます。

ビルドフォルダはデフォルトでは build/プリセット名 になっています。
また Generator は Ninja Multi Config での生成になります。

vpkg.json で外部ライブラリを扱うため、
CMAKE_TOOLCHAIN_FILE は vcpkg のものが指定されています。

```bash
cmake --preset x86-windows --config Release
cmake --build build/x86-windows
```

ビルドに必要な定義が行われた Makefile が準備されていいます。
make が使える環境ではこちらが利用可能です


```bash
# 構築対象 preset設定（未定義時はOSで自動判定）
export PRESET=x86-windows
# ビルドタイプ指定（未定義時は Release）
export BUILD_TYPE=Release
#export BUILD_TYPE=Debug

# cmake オプション指定
# KRKRZ_USE_SJIS  デフォルトをSJIS(MBSC) にする
export CMAKEOPT="-DKRKRZ_USE_SJIS=ON"

# cmake プロジェクト生成
# この段階で vcpkg が処理されてライブラリが準備されます
make prebuild

# cmake でビルド
make build 

# サンプル実行
make run

# インストール処理
INSTALL_PREFIX=install make install

```	

### ビルド設定

処理内容詳細は Makefile と CMakeList.txt を参照して下さい。

ビルド用の以下の特殊な CMake変数があります

KRKRZ_VARIANT=WIN    旧来のWindows版準拠で構築します
KRKRZ_VARIANT=SDL    SDLバージョンで作成します（デフォルト）
KRKRZ_VARIANT=LIB    ライブラリ版KRKRZを作成します

KRKRZ_VARIANT=SDL / LIB では、旧来の Windows版固有の機能が排除
された GENERICバージョンの吉里吉里になります。

GENERICバージョンあわせのプラグインをビルドする場合は、tp_stub.h を
読み込む前に __GENERIC__ を定義しておく必要があるので注意してください。

tp_stub/krkrz.cmake を使う場合は KRKRZ_VARIANT が定義されている場合は
自動で __GENERIC__ が追加されます。

※特に変数指定がない場合、tp_stub.h は __WINVER__ を定義して
旧WIN版互換あわせでの動作になります。


### そのほか特殊変数

MASTER  
    ビルド時に定義されているとログレベルが WARNING で固定になります（INFOログがコンソール表示されなくなります）

    未定義時は、起動時ログレベルが Release 版は INFO、Debug版は DEBUG になります。
    起動時オプション -loglevel=ERROR,WARNING,INFO,DEBUG,VERBOSE で変更可能になります

KRKRZ_REPL  
    対話型 TJS REPL 機能のビルドスイッチ。Win / Mac / Linux ではデフォルト ON、
    それ以外 (Android, iOS) では OFF。詳細は [doc/REPL.md](doc/REPL.md) 参照。
    機能ON の場合は起動時オプション -repl でコンソールで　REPL が起動します。

ログ処理の仕組み、ファイル出力、TJS から見た API 等は
[doc/Logging.md](doc/Logging.md) を参照してください。

### テスト実行

Makefile にそのままトップフォルダで実行可能なルールが定義されています。

```bash
# cmake 経由で実行
make run
```

WINVER で OpenGL 機能動作時は以下のファイル構成が必要になります

    plugin/ プラグインフォルダ
      libEGL.dll        OpenGL の egl用DLL
      libGLESv2.dll     OpenGL の GLES2用DLL
    plugin64/ プラグインフォルダ 64bit
      libEGL.dll        OpenGL の egl用DLL
      libGLESv2.dll     OpenGL の GLES2用DLL

SDL 版は OS側で OpenGLES 実装が存在する場合はそれが使われますが
無い場合は同様の DLL が必要になります

### SIMDパリティテスト

`tests/simd_parity_test.cpp` に画像処理SIMD（SSE2 / AVX2 / NEON）と
C リファレンス実装の出力を byte 単位で比較する CTest テスト
（`krkrz_simd_parity_test` / テスト名 `simd_parity`）が用意されています。

このテストは `tvpgl.c` / `blend_function.cpp` / 各 `*_sse2.cpp` /
`*_avx2.cpp` / `*_neon.cpp` / `detect_cpu.cpp` 等 SIMD コアのみを直接
リンクするスタンドアロンターゲットで、SDL3 / OpenGL / vcpkg のランタイム
依存はありません。`KRKRZ_BUILD_TESTS=ON`（デフォルト）かつターゲットアーキ
テクチャが x86 系または ARM 系のときに有効化されます。

```bash
# Makefile 経由 (prebuild 済みであること)
make test

# cmake / ctest 直接実行
cmake --build $(BUILD_PATH) --config Release --target krkrz_simd_parity_test
ctest --test-dir $(BUILD_PATH) -C Release -R simd_parity --output-on-failure
```

期待される出力:

- x86 (Windows / Linux / macOS): `[SSE2 vs C reference]` と
  `[AVX2 vs C reference]` の 2 セクションが走り、それぞれ全項目 pass。
- ARM / ARM64 (Linux / Android): `[NEON vs C reference]` セクションが走る。

PsBlend ファミリは SSE2 側が 7bit 量子化のため harness 側で
`tol_alpha=-1, tol_rgb=2`（ColorDodge5 のみ `tol_rgb=8`）の tolerance
policy が適用されます。それ以外は byte-exact 比較です。

### DAP スモークテスト

`tests/dap_smoke.py` は krkrz の DAP サーバ動作を最小確認する Python
スクリプトです。`-dap=<port>` で krkrz を起動し、TCP 経由で initialize /
attach / evaluate / scopes / variables / step 系 / disconnect の往復が
正常応答することを VSCode 拡張なしで検証します。

```bash
python tests/dap_smoke.py build/x64-windows-sdl/Release/krkrz64.exe data
```

最終行に `[smoke] PASS: all phases verified` が出れば OK。

## デバッグ実行

### VisualStudio でのデバッグ

以下の手順でソースデバッグできます

- Visual Studio を起動して、プロジェクトなしの状態のウインドウに実行ファイルをドロップする
- デバッグのプロパティの作業フォルダにプロジェクトフォルダを指定（プラグインフォルダの参照先になるため）
- デバッグのプロパティの引数に data フォルダの場所をフルパスで指定（現行仕様がexe相対もしくは絶対パス）

### VSCode でのデバッグ (C++ ネイティブ)

C++ レベルでデバッグする場合は次のような launch.json を準備します。
program 部分に生成される実行ファイルのパス名を直接記載します。
args で処理対象フォルダを指定できます（フルパスになるように記載して下さい）

launch.json

```json
{
    // IntelliSense を使用して利用可能な属性を学べます。
    // 既存の属性の説明をホバーして表示します。
    // 詳細情報は次を確認してください: https://go.microsoft.com/fwlink/?linkid=830387
    "version": "0.2.0",
    "configurations": [
        {
            "name": "WINデバッグ起動",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "build/x86-windows/Debug/krkrz.exe",
            "args": ["${workspaceFolder}/data"],
            "stopAtEntry": false,
            "console":"externalTerminal",
            "cwd": "${workspaceFolder}",
            "environment": []
        }
    ]
}
```

### VSCode + DAP による TJS スクリプトデバッグ

吉里吉里Z は Debug Adapter Protocol (DAP) サーバを内蔵しており、
専用の VSCode 拡張 [krkrz-vscode](https://github.com/wamsoft/krkrz-vscode)
を使うと TJS2 スクリプトを通常のプログラミング言語と同じ感覚でデバッグ
できます (BP / ステップ実行 / コールスタック / 変数 inspect / 条件付き BP /
log point / Watch 式評価 など)。

起動例:

```bash
krkrz64.exe -dap=6635 ${workspaceFolder}/data
```

VSCode 側で `krkrz` 拡張をインストールし、`launch.json` に attach 設定を
追加するだけで接続できます。

ビルド時オプション `KRKRZ_ENABLE_DAP` (デフォルト ON) を OFF にすると
DAP 関連コードは全て `#ifdef` で除外されます。

詳細な使い方・既知制限・拡張のビルド方法は [krkrz-vscode の README](https://github.com/wamsoft/krkrz-vscode) を参照してください。

TJS2 / KAG (.ks) のシンタックスハイライトも同拡張に同梱されています。
KAG (.ks) 行への BP は仕様上対応不可ですが、`[iscript]...[endscript]` 内の
TJS なら BP 設置可能です。

# その他情報

自動生成ファイル
吉里吉里Z本体にはいくつかの自動生成ファイルが存在します。
自動生成ファイルは直接編集せず、生成元のファイルを編集します。
生成には bat ファイルと、perl(構文解析器 bison 経路 / tvpgl 生成)および python(tp_stub 生成 / メッセージ生成)が使われます。**perl と python の両方が必要です**(tp_stub とメッセージ定義系は 2026-08 に Perl+Excel から Python 標準ライブラリのみへ移行済み。bison 経路と tvpgl は引き続き perl)。
各生成ファイルを左に ':' 以降に生成元ファイルを列挙します。

tjs2/syntax/compile.bat で以下のファイルが生成されます。

tjs.tab.cpp/tjs.tab.hpp : tjs.y
tjsdate.tab.cpp/tjsdate.tab.hpp : tjsdate.y
tjspp.tab.cpp/tjspp.tab.hpp : tjspp.y
tjsDateWordMap.cc : gen_wordtable.bat

これらのファイルの生成には bison が必要です。
bison には libiconv2.dll libintl3.dll regex2.dll が必要なので一緒にインストールする必要があります。
http://gnuwin32.sourceforge.net/packages/bison.htm
http://gnuwin32.sourceforge.net/packages/libintl.htm
http://gnuwin32.sourceforge.net/packages/libiconv.htm
http://gnuwin32.sourceforge.net/packages/regex.htm

visual/glgen/gengl.bat で以下のファイルが生成されます。
tvpgl.c/tvpgl.h : maketab.c/tvpps.c

common/base/makestub.bat (中身は python gen_tpstub.py) で以下のファイルが生成されます。
gen_tpstub.py は Python 標準ライブラリ(zlib/hashlib)のみで動作し、Perl (makestub.pl + Compress::Zlib + Digest::MD5) は不要になりました。出力は旧 Perl 版とバイト単位で一致します。
FuncStubs.cpp/FuncStubs.h : gen_tpstub.py 内で指定されたヘッダーファイル内の TJS_EXP_FUNC_DEF/TVP_GL_FUNC_PTR_EXTERN_DECL マクロおよび TJS_*_METHOD_DEF* マクロで記述された関数
tp_stub.cpp/tp_stub.h : 同上 (生成後は plugins 側 tp_stub サブモジュールへ従来どおりコピーする)

common/msg/text/gen_messages.py で以下のメッセージ定義系ファイルが生成されます(Python 標準ライブラリのみ。Excel/Win32::OLE/Perl 不要)。源=CSV の common/msg/text/messages.csv を編集し `python gen_messages.py`。詳細は common/msg/text/README.md。
tjsErrorInc.h / MsgIntfInc.h / MsgImpl.h / resource/messages{,-en,-chs}.json / win32/vcproj/string_table_*.rc + string_table_resource.h / generic・win32 の MsgLoad.cpp : messages.csv

