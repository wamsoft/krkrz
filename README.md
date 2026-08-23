# 吉里吉里Z multi platform (krkrz)

TJS2 スクリプトエンジン + ノベルゲームランタイム「吉里吉里Z」を、Windows 専用から
マルチプラットフォームへ展開した派生版のエンジン本体です。

- システム基本制御に **SDL3** を使うビルドと、従来どおり **Win32 ネイティブ**の
  ビルドを、同じソースから作り分けます
- 描画は **OpenGL ES** ベースの機構 (Canvas / Texture / Shader / Offscreen) を持ち、
  Windows ネイティブビルドは既定で **Direct3D 11** 経路です (D3D9 は撤去済み)
- 外部ライブラリは極力そのまま参照する構成で、依存解決に **vcpkg** を使います
  (SDL3 は最新版を追うため `FetchContent` で取得します)

## リポジトリの位置づけ

このリポジトリは**エンジン本体のみ**です。プラグイン・TJS2 スクリプトライブラリ
(KAG3 等)・統合ビルドは、アンブレラリポジトリ **`krkrz_dev`** が submodule として
束ねています。プラグイン込みで一式ビルドしたい場合はそちらを使ってください。

ブラウザ (Emscripten/wasm) と Android のビルドは、このリポジトリを engine ソースと
して参照する「外枠」リポジトリ (`krkrz_web` / `krkrz_android`) 側にあります。

## ビルドバリアント

| `KRKRZ_VARIANT` | 内容 |
|---|---|
| `SDL` (既定) | SDL3 ベースの汎用ビルド。Windows / Linux / macOS / Android / wasm |
| `WIN` | 従来の Windows ネイティブ (Win32 API + D3D11)。WINVER と呼びます |
| `LIB` | 汎用ビルドの静的ライブラリ版 (`libkrkrz`) |

`SDL` / `LIB` では Windows 固有機能を除いた **GENERIC バージョン**になり、
`__GENERIC__` マクロが定義されます。

> GENERIC 版向けのプラグインをビルドする場合は、`tp_stub.h` を読み込む前に
> `__GENERIC__` を定義してください。`tp_stub/krkrz.cmake` を使う場合は
> `KRKRZ_VARIANT` が定義されていれば自動で付きます。指定が無い場合の `tp_stub.h` は
> `__WINVER__` を定義した旧 Win 版互換の動作になります。

---

## 開発環境準備

### Windows

Visual Studio (2022 以降) を入れて C++ コンパイラを使える状態にします。
付属の CMake / Ninja を利用します。

`make` を使いたい場合は msys2 を入れて基礎開発ツールを導入します。

```bash
pacman -S base-devel
```

> ビルドは **Visual Studio の Developer Command Prompt から起動**してください。
> 32bit ビルド (`x86-windows`) は x86 用の Developer Command Prompt が必要です
> (アーキテクチャが食い違うと vcpkg が誤動作します)。

### Linux / macOS

整備中。

### vcpkg

各環境に vcpkg を導入し、そのフォルダを環境変数 `VCPKG_ROOT` に設定します。

※ Visual Studio 2022 以降には vcpkg が同梱されています。自前で入れたものと競合
するので、どちらか一方に統一してください。

https://learn.microsoft.com/ja-jp/vcpkg/get_started/overview

```bash
# dos
set VCPKG_ROOT="c:\work\vcpkg"

# msys/cygwin
export VCPKG_ROOT='c:\work\vcpkg'
```

---

## ビルド

### ソースの取得

clone 後に submodule を更新してください。

```bash
git submodule update --init
```

### プリセット

`CMakePresets.json` のプリセットを使ってビルドします。必要なライブラリは
`vcpkg.json` によって用意されます。ビルドフォルダは既定で `build/<プリセット名>`、
ジェネレータは Ninja Multi-Config です。

| プリセット | バリアント | 備考 |
|---|---|---|
| `x64-windows` / `x86-windows` / `arm64-windows` | SDL | **SDL3 ビルド** (名前に `-win` が付かない方) |
| `x64-windows-win` / `x86-windows-win` / `arm64-windows-win` | WIN | **Windows ネイティブ (WINVER)** |
| `x64-linux` / `arm64-linux` | SDL | Linux |
| `x64-osx` / `arm64-osx` | SDL | macOS |
| `x64-android` / `arm64-android` | SDL | Android |

> 名前が紛らわしいので注意: `x64-windows` は **SDL ビルド**、
> WINVER が欲しいときは `x64-windows-win` です。

```bash
cmake --preset x64-windows
cmake --build build/x64-windows --config Release
```

### Makefile 経由

同等の処理をまとめた Makefile があります。

```bash
# 対象プリセット (未指定時は OS から自動判定)
export PRESET=x64-windows
# ビルドタイプ (未指定時は Release)
export BUILD_TYPE=Release

# cmake オプション
export CMAKEOPT="-DKRKRZ_USE_SJIS=ON"

make prebuild            # cmake configure (ここで vcpkg が走る)
make build               # ビルド
make run                 # data/ を引数にして実行
make test                # パリティテスト (画像 SIMD / サウンド SIMD)
INSTALL_PREFIX=install make install
```

### 主なビルドオプション

`-D<名前>=<値>` または `CMAKEOPT` で指定します。全ての定義は
[PreprocessorDefinitions.md](PreprocessorDefinitions.md) を参照してください。

| オプション | 既定 | 内容 |
|---|---|---|
| `KRKRZ_VARIANT` | `SDL` | ビルドバリアント (上表) |
| `KRKRZ_USE_SJIS` | OFF | 既定のテキスト読み込みを SJIS (MBCS) にする |
| `KRKRZ_USE_OPENGL` | ON | OpenGL ES 描画機構 (Canvas/Texture/Shader 等) |
| `KRKRZ_USE_ELEMENTS` | ON | Elements ダイアログ UI (SDL/WIN)。OFF で `Dialog` クラスごと消える |
| `KRKRZ_USE_GLYPHWARE` | ON | 統一フォントエンジン glyphware |
| `KRKRZ_USE_MOVIE` | ON | 動画再生 (external/movie-player) |
| `KRKRZ_REPL` | デスクトップ ON | 対話型 TJS REPL (`-repl` / `-replfile`) |
| `KRKRZ_REPL_WEB` | `KRKRZ_REPL` に追従 | ブラウザ REPL / WebServer クラス (`-replweb`) |
| `KRKRZ_ENABLE_DAP` | ON | VSCode デバッグアダプタ (DAP) サーバ |
| `KRKRZ_BUILD_TESTS` | ON | パリティテスト (画像 / サウンド SIMD) のビルド |
| `KRKRZ_DRAW_STATS` | OFF | DrawThreadPool 利用率の計測 ([DrawStats.md](doc/DrawStats.md)) |
| `KRKRZ_RESOURCE_DIR` | `resource/` | 埋め込みリソースフォルダ (案件用に差し替え可) |
| `KRKRZ_WIN_ICON` | 無し | Windows: exe へ埋め込むアイコン (`.ico` の絶対パス) |
| `KRKRZ_VERSION_BUILD` | `0` | バージョン 4 桁目 ([Versioning.md](doc/Versioning.md)) |
| `MASTER` | OFF | ログレベルを WARNING 固定にする (INFO を出さない) |

`MASTER` 未定義時の起動時ログレベルは Release=INFO / Debug=DEBUG で、起動オプション
`-loglevel=ERROR,WARNING,INFO,DEBUG,VERBOSE` で変更できます。詳細は
[Logging.md](doc/Logging.md)。

`KRKRZ_WIN_ICON` に `.ico` の絶対パスを渡すと、生成される `.rc` へ
`MAINICON ICON <パス>` が追加され、ビルド直後から exe にアイコンが付きます
(指定しない場合、exe にはアイコンリソースが 1 つも入りません)。リソース名を
`MAINICON` にしてあるのは、後からアイコンを差し替えるツール (IconReset /
`iconreset.tjs`) がその名前を前提にしているためです。`.ico` の内容が変われば
`.res` を作り直すよう依存関係も設定されます。

---

## 実行

```bash
make run                 # data/ を引数に起動
DATAPATH=path/to/game make run
```

`data/` にはエンジン機能を確認するための**コアデモ**が入っています。
`data/gallery` はメニュー + 21 デモを 1 プロセスで切り替えるギャラリーで、
`data/` 直下を指定するとプラグイン横断デモも含むランチャが起動します。

各デモは `-demotest` を付けるとヘッドレスで数十フレーム動かして
`@demotest:` 行を出力し終了するので、CI 的な起動確認に使えます。

```bash
krkrz64.exe <repo>/data/gallery -demotest
```

### 起動時の DrawDevice

描画経路は起動オプション `-drawdevice=` で選択します。

| ビルド | 選べる値 | 既定 |
|---|---|---|
| SDL3 | `sdl` (SDL_Renderer) / `sdlogl` (OpenGL ES 直接) / `ogl` (OpenGL ES + Canvas 等フル機能) | `sdlogl` (OpenGL 無効ビルドでは `sdl`) |
| WINVER | `basic` (Direct3D 11) / `ogl` / `null` (描画なし・検証用) | `basic` |

実行中に `Window.drawDevice` へ代入して切り替えることもできますが、**SDL3 ビルドを
`-drawdevice=sdl` で起動した場合は、実行中に OpenGL 系へ切り替えることはできません**
(OpenGL の初期化が通りません)。OpenGL 機能を使う場合は最初から `sdlogl` / `ogl` で
起動してください。

### OpenGL ES の実行環境

エンジンの OpenGL 機能 (Canvas / Texture / Shader、`-drawdevice=ogl` / `sdlogl`) は
OpenGL ES で動きます。必要なランタイムはビルドによって異なります。

**WINVER ビルド**は常に EGL (ANGLE) 経由で GLES コンテキストを作るため、OpenGL
機能を使う場合は実行ファイルの隣に ANGLE の DLL が必要です (`make run` は自動で
コピーします):

```
plugin/     libEGL.dll / libGLESv2.dll        (32bit)
plugin64/   libEGL.dll / libGLESv2.dll        (64bit)
```

**SDL3 ビルド**は SDL の初期化時点で GLES コンテキスト前提の宣言を行うため、OpenGL
を使う場合は最初から GLES が利用できる環境である必要があります。Windows では GLES
コンテキストはまず OS の OpenGL ドライバ (WGL の ES プロファイル =
`WGL_EXT_create_context_es2_profile`) から作られ、使えない場合に ANGLE (上記と同じ
`libEGL.dll` / `libGLESv2.dll`) へフォールバックします。ドライバの ES プロファイル
実装に問題がある場合 (描画が乱れる・初期化に失敗する等) は、起動オプション
`-forceegl=yes` で最初から EGL (ANGLE) を使わせて切り分け・回避ができます。
Linux / Android などは OS ネイティブの EGL / GLES を使います。

コンテキストの要求バージョンは ES 3.2 で、作れない場合は 3.1 → 3.0 → 2.0 と
下げて再試行します (ANGLE の D3D11 バックエンドは ES 3.0/3.1 までのため、EGL
フォールバック時はこの再試行で成立します)。実際に得られたバージョンは起動ログの
`Loaded GLES x.y` で確認できます。

#### ANGLE DLL の入手

ANGLE の DLL はリポジトリには含まれません。かつては Chromium のインストールに
単体 DLL として含まれていましたが現在はその形では配布されていないため、
プレビルドの利用が手軽です:

- [mmozeiko/build-angle](https://github.com/mmozeiko/build-angle) —
  upstream ANGLE を毎日ビルドして Releases に置いています (x64 / arm64。zip に
  `libEGL.dll` / `libGLESv2.dll` / `d3dcompiler_47.dll` が含まれます)。
  動作確認済み: 2026-07-25 版 (upstream
  [cc226c81](https://github.com/google/angle/commit/cc226c81fda884e3b85aad1e00e581c12f8aee04))。
- 32bit (x86) のプレビルドは提供されていないため、必要な場合は自前ビルド
  (depot_tools + gn) になります。

配置は実行ファイルの隣です。アンブレラリポジトリでは、ルートの `plugin/` (32bit) /
`plugin64/` (64bit) に置いておくと `make run` がビルド出力へコピーします。

> **注意**: exe の隣に DLL が無くても、PATH 上にある別アプリ同梱の ANGLE
> (例: Oculus ランタイムの `libEGL.dll`) を拾って動いてしまうことがあります。
> バージョンが古く初期化に失敗するなど紛らわしい症状になるため、OpenGL 機能を
> 使う配布物には必ず DLL を同梱してください (ANGLE のライセンス表記は本体の
> ライセンス収集機構に収録済みです)。

### 表示言語

エンジンのメッセージ (エラー文言等) とオプション解説 (`-userconf` の設定 UI) は
多言語対応です (ja / en / chs / cht)。既定では OS の言語設定
(`System.systemLanguage`) に追従し、起動オプション `-language=<タグ>` (BCP-47 または
短縮形 ja / en / chs / cht) で明示指定できます。WINVER は PE リソースの言語解決
(スレッド UI 言語) で、SDL3 は `resource/messages*.json` / `optiondesc*.json` の
選択で切り替わります (メッセージ文字列への反映はコマンドライン直接指定のみ)。

---

## テスト

`make test` で CTest のパリティテスト 2 種 (画像 SIMD / サウンド SIMD) が走ります。

### 画像 SIMD パリティテスト

`tests/simd_parity_test.cpp` は画像処理 SIMD (SSE2 / AVX2 / NEON) と C リファレンス
実装の出力を byte 単位で比較する CTest テストです
(`krkrz_simd_parity_test` / テスト名 `simd_parity`)。

`tvpgl.c` / `blend_function.cpp` / 各 `*_sse2.cpp` / `*_avx2.cpp` / `*_neon.cpp` /
`detect_cpu.cpp` 等の SIMD コアだけを直接リンクするスタンドアロンターゲットなので、
SDL3 / OpenGL / vcpkg のランタイム依存はありません。`KRKRZ_BUILD_TESTS=ON` (既定) かつ
ターゲットが x86 系または ARM 系のときに有効です。

```bash
make test          # 画像 + サウンドの両方 (-R parity)
# または個別に
ctest --test-dir build/<preset> -C Release -R simd_parity --output-on-failure
```

期待される出力:

- x86: `[SSE2 vs C reference]` と `[AVX2 vs C reference]` の 2 セクションが全項目 pass
- ARM / ARM64: `[NEON vs C reference]` セクション

PsBlend ファミリのみ SSE2 側が 7bit 量子化のため
`tol_alpha=-1, tol_rgb=2` (ColorDodge5 のみ `tol_rgb=8`) の tolerance が適用されます。
それ以外は byte-exact 比較です。**新しい不一致は新規バグか意図的な参照変更のどちらか**で、
「既知のノイズ」ではありません。

### サウンド SIMD パリティテスト

`tests/sound_parity_test.cpp` は `common/sound/` の SSE 実装 (Ooura Real DFT /
窓関数の deinterleave・interleave 等) を C リファレンスと比較します
(`krkrz_sound_parity_test` / テスト名 `sound_parity`)。float 演算の丸め差が
避けられないため、byte-exact ではなく相対誤差 + 絶対誤差の複合トレランスで
判定します。こちらも SDL3 / 描画 / 音響デバイスに依存しません。

### DAP スモークテスト

```bash
python tests/dap_smoke.py build/x64-windows/Release/krkrz64.exe data
```

`-dap=<port>` で起動し、initialize / attach / evaluate / scopes / variables /
step 系 / disconnect の往復を VSCode 拡張なしで検証します。最終行に
`[smoke] PASS: all phases verified` が出れば OK。

---

## デバッグ

### Visual Studio (C++)

- プロジェクトなしの状態の VS ウィンドウに実行ファイルをドロップ
- デバッグのプロパティで作業フォルダにプロジェクトフォルダを指定
  (プラグインフォルダの参照先になるため)
- 引数に data フォルダをフルパスで指定 (exe 相対か絶対パスのみ対応)

### VSCode (C++ ネイティブ)

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "WINデバッグ起動",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "build/x86-windows/Debug/krkrz.exe",
            "args": ["${workspaceFolder}/data"],
            "stopAtEntry": false,
            "console": "externalTerminal",
            "cwd": "${workspaceFolder}",
            "environment": []
        }
    ]
}
```

### VSCode + DAP (TJS スクリプトデバッグ)

DAP サーバを内蔵しているので、拡張
[krkrz-vscode](https://github.com/wamsoft/krkrz-vscode) から TJS2 を通常の言語と
同じ感覚でデバッグできます (BP / ステップ / コールスタック / 変数 inspect /
条件付き BP / log point / Watch)。

```bash
krkrz64.exe -dap=6635 ${workspaceFolder}/data
```

`KRKRZ_ENABLE_DAP=OFF` にすると DAP 関連コードは全て `#ifdef` で除外されます。
TJS2 / KAG (.ks) のシンタックスハイライトも同拡張に同梱されています
(KAG 行への BP は仕様上不可。`[iscript]...[endscript]` 内の TJS なら設置可能)。

### REPL / エージェント駆動

対話型 TJS シェルを内蔵しています。詳細は [REPL.md](doc/REPL.md)。

```bash
krkrz64.exe data -repl              # コンソール REPL
krkrz64.exe data -replfile=<dir>    # ファイルチャネル (外部ツール/エージェント向け)
krkrz64.exe data -replweb=8899      # ブラウザ REPL (HTTP + SSE)
```

`Agent` クラス (入力注入 / 画面キャプチャ / ダイアログ操作) と併せると、
実画面を見ながらの自動検証ができます。

---

## バージョン番号

`CMakeLists.txt` の `PROJECT_VERSION` が単一の供給元で、そこから
`krkrz_version.h` (C++ 側) と `krkrz_version.rc` (Windows の VERSIONINFO) が
生成されます。番号を上げるときはこの 1 行だけを書き換えます。

上げ方の基準・製品名の組み立て・確認方法は [Versioning.md](doc/Versioning.md)。

---

## 自動生成ファイル

いくつかのファイルはコミット済みですが**自動生成物**です。直接編集せず、生成元を
編集して再生成してください。生成には **python** と、一部で **perl** が必要です。

### bison 経路 (perl + bison)

`common/tjs2/syntax/compile.bat` で生成:

| 生成物 | 生成元 |
|---|---|
| `tjs.tab.cpp` / `tjs.tab.hpp` | `tjs.y` |
| `tjsdate.tab.cpp` / `tjsdate.tab.hpp` | `tjsdate.y` |
| `tjspp.tab.cpp` / `tjspp.tab.hpp` | `tjspp.y` |
| `tjsDateWordMap.cc` | `gen_wordtable.bat` |

bison には `libiconv2.dll` / `libintl3.dll` / `regex2.dll` が必要です。

- http://gnuwin32.sourceforge.net/packages/bison.htm
- http://gnuwin32.sourceforge.net/packages/libintl.htm
- http://gnuwin32.sourceforge.net/packages/libiconv.htm
- http://gnuwin32.sourceforge.net/packages/regex.htm

### tvpgl (perl)

`common/visual/glgen/gengl.bat` で `tvpgl.c` / `tvpgl.h` を `maketab.c` / `tvpps.c`
から生成します。

### tp_stub (python のみ)

`common/base/makestub.bat` (中身は `python gen_tpstub.py`) で生成します。
Python 標準ライブラリ (zlib / hashlib) のみで動作し、旧 Perl 版 (`makestub.pl` +
Compress::Zlib + Digest::MD5) は不要です。出力は旧版とバイト単位で一致します。

| 生成物 | 生成元 |
|---|---|
| `FuncStubs.cpp` / `FuncStubs.h` | 各ヘッダの `TJS_EXP_FUNC_DEF` / `TVP_GL_FUNC_PTR_EXTERN_DECL` / `TJS_*_METHOD_DEF*` マクロ |
| `tp_stub.cpp` / `tp_stub.h` | 同上 |

生成後、`tp_stub.{h,cpp}` は従来どおり plugins 側の tp_stub サブモジュールへコピーします。

### メッセージ定義 (python のみ)

`common/msg/text/gen_messages.py` で生成します。源は CSV
(`common/msg/text/messages.csv`) で、これを編集して `python gen_messages.py` を実行
します。詳細は `common/msg/text/README.md`。

生成物: `tjsErrorInc.h` / `MsgIntfInc.h` / `MsgImpl.h` /
`resource/messages{,-en,-chs}.json` / `win32/vcproj/string_table_*.rc` +
`string_table_resource.h` / generic・win32 の `MsgLoad.cpp`

### バージョン (CMake)

`cmake/krkrz_version.h.in` / `cmake/krkrz_version.rc.in` から
`${CMAKE_BINARY_DIR}/krkrz_version.{h,rc}` が生成されます (上述)。

---

## ドキュメント索引 (`doc/`)

### 描画

| ドキュメント | 内容 |
|---|---|
| [ScreenTransfer.md](doc/ScreenTransfer.md) | 合成フレーム → GPU 転送のコスト。計測 (`System.renderStats`) と数値の読み方 |
| [D3D11Migration.md](doc/D3D11Migration.md) | WINVER の D3D9 → D3D11 移行と、その後の追補 (vblank / 差分転送) |
| [GLCompositor.md](doc/GLCompositor.md) | 裏 GLES 合成をレイヤへ書き戻す機構 |
| [CanvasEffect.md](doc/CanvasEffect.md) / [CanvasTransition.md](doc/CanvasTransition.md) | Canvas のポストエフェクトとトランジション |
| [Viewport.md](doc/Viewport.md) | ゲーム画面の表示画角制御 |
| [DrawStats.md](doc/DrawStats.md) | DrawThreadPool 利用率の計測 |
| [OpaqueExcludeNotPropagating.md](doc/OpaqueExcludeNotPropagating.md) | 不透明領域除外が伝播しない件 |

### UI / 入力

| ドキュメント | 内容 |
|---|---|
| [ElementsDialog.md](doc/ElementsDialog.md) | Elements ダイアログ機構 (JSON レイアウト / 入力ルーティング / overlay 描画) |
| [ModalWindow.md](doc/ModalWindow.md) | `Window.showModal` と複数ウィンドウの落とし穴 |
| [Gamepad.md](doc/Gamepad.md) | ゲームパッド入力 (論理インデックス / 軸 / 振動) |
| [PadOverlay.md](doc/PadOverlay.md) | パッド状態オーバレイ |
| [FontEngine.md](doc/FontEngine.md) | 統一フォントエンジン glyphware |

### 音・動画

| ドキュメント | 内容 |
|---|---|
| [MovieMFMigration.md](doc/MovieMFMigration.md) | 動画まわり (DirectShow 撤去 / Media Foundation / presenter) |
| [Sound3D.md](doc/Sound3D.md) | 3D 音声まわり |

### メモリ・診断

| ドキュメント | 内容 |
|---|---|
| [MemoryDesign.md](doc/MemoryDesign.md) | アロケータ設計 (プール / タグ / サウンド用フック) |
| [MemoryGuide.md](doc/MemoryGuide.md) | メモリ観測・調査の手引き |
| [LeakAudit.md](doc/LeakAudit.md) | リーク監査 |
| [TtstrDataRetention.md](doc/TtstrDataRetention.md) | `ttstr` のデータ保持に関する注意 |
| [Logging.md](doc/Logging.md) | ログ出力の仕組みと API |

### 実行・運用

| ドキュメント | 内容 |
|---|---|
| [REPL.md](doc/REPL.md) | 対話型 TJS シェル / ファイルチャネル / ブラウザ REPL / Agent 駆動 |
| [CommandLinePresets.md](doc/CommandLinePresets.md) | コマンドラインのプリセット |
| [AppEvent.md](doc/AppEvent.md) | アプリイベントの送出 |
| [LicenseSystem.md](doc/LicenseSystem.md) | ライセンス表記の収集機構 |
| [Versioning.md](doc/Versioning.md) | バージョン番号の供給元と上げ方 |
| [ModernizationRoadmap.md](doc/ModernizationRoadmap.md) | WINVER モダン化の計画と進捗 |
