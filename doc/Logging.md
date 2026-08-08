# ログ系

## 構造

ログ出力は `common/utils/LogCore.cpp` を中心に一本化されています。
全てのログは最終的に `TVPLogDispatchLine(level, utf8_line)` に集約され、
次の順番で処理されます:

1. タイムスタンプ (`HH:MM:SS`) を付与、WARNING 以上には `!` マーカー追加
2. リングバッファ (`TVPLogDeque`, 既定 2048 行) に追加
3. important log キャッシュ (WARNING 以上) に追加
4. TJS logging handler (`Debug.addLoggingHandler`) 発火
5. ファイル出力 (`krkr.console.log`, UTF-16 LE + BOM + CRLF) — 有効時
6. コンソール出力 — sink が登録されていればそちらへ、無ければ既定書き出し

LogImpl (`generic/utils/LogImpl.cpp` = plog 版、`sdl3/utils/LogImpl.cpp` =
SDL3 版) はレベル整形だけを担当し、整形済みの本文を LogCore に渡します。
タイムスタンプは LogCore 側で一元付与され、LogImpl は含めません。

## CMake オプション: KRKRZ_USE_LOGCORE

LogCore の有効/無効は CMake オプション `KRKRZ_USE_LOGCORE` で制御されます:

```bash
cmake --preset x64-windows -DKRKRZ_USE_LOGCORE=OFF  # LogCore 無効
cmake --preset x64-windows                           # デフォルト: デスクトップでは ON
```

デフォルト値は `KRKRZ_DESKTOP` に連動:
- **デスクトップ** (Windows / Linux / macOS): `ON`
- **モバイル** (Android / iOS): `OFF`

### LogCore 有効時 (`TVP_USE_LOGCORE` 定義)

- `common/utils/LogCore.cpp` がリンクされる
- SDL3 のログ出力を `SDL_SetLogOutputFunction` で差し替え、LogCore 経由で処理
- リングバッファ、ファイル出力、TJS logging handler、REPL sink 等の全機能が利用可能

### LogCore 無効時 (`TVP_USE_LOGCORE` 未定義)

- `LogCore.cpp` はリンクされない
- `sdl3/utils/LogImpl.cpp` 内のスタブ実装が使われる
- モバイル環境では SDL3 のネイティブログ出力を直接使用
- Windows では親コンソールにアタッチしてログを出力

## プラットフォーム別動作 (SDL3 版)

SDL3 版の LogImpl (`sdl3/utils/LogImpl.cpp`) はプラットフォームと
`TVP_USE_LOGCORE` の設定によって動作が異なります。

### デスクトップ環境 (Windows / Linux / macOS) + LogCore 有効

`SDL_SetLogOutputFunction` で SDL3 のログ出力を `TVPSDLLogOutput` に
差し替え、LogCore 経由で処理します。LogCore の全機能が利用可能です:

- リングバッファ (`Debug.getLastLog()`)
- ファイル出力 (`krkr.console.log`)
- TJS logging handler (`Debug.addLoggingHandler()`)
- REPL コンソール sink (色付き表示)
- important log キャッシュ

### Windows + LogCore 無効

`SDL_SetLogOutputFunction` で `TVPSDLLogOutputWin32` を設定し、
親プロセスのコンソールにアタッチしてログを出力します:

- `AttachConsole(ATTACH_PARENT_PROCESS)` で親コンソールに接続
- 真のコンソールには `WriteConsoleW` (UTF-16)
- パイプ/リダイレクト時は `WriteFile` (UTF-8)

これにより、コマンドプロンプトや PowerShell から起動した場合でも
ログがコンソールに表示されます。

### モバイル環境 (Android / iOS)

SDL3 のネイティブログ出力をそのまま使用します。LogCore への迂回は行わず、
SDL3 が各プラットフォームの標準ログ機構に直接出力します:

| プラットフォーム | 出力先 | 確認方法 |
|---|---|---|
| **Android** | logcat | `adb logcat -s SDL/APP:*` |
| **iOS** | os_log / Console.app | Xcode コンソール / Console.app |

LogCore の関数 (`TVPAddLog`, `TVPGetLastLog` 等) はスタブ実装となり、
リングバッファやファイル出力は行われません。

この設計の理由:

1. **適切なログ出力**: モバイル環境では `fprintf(stderr, ...)` ではシステム
   ログに届かない。SDL3 は内部で `__android_log_write` (Android) や
   `os_log` (iOS) を呼ぶため、正しくシステムログに出力される。

2. **機能の適合性**: デスクトップ向けの LogCore 機能 (ファイル出力、REPL
   連携、TJS handler) はモバイル環境では実質的に不要。

3. **シンプルさ**: プラットフォーム固有のログ API を LogCore 側に追加
   するより、SDL3 の抽象化に任せるほうが保守性が高い。

## API

### 出力

- `TVPLOG_VERBOSE / DEBUG / INFO / WARNING / ERROR / CRITICAL(fmt, ...)`:
  `tvpfmt` 書式 (後述) でのレベル付きログ。file/func/line が自動付与されます。
- `TVPLogMsg(level, utf8)`: シンプルな UTF-8 文字列を直接流す経路。
- `TVPAddLog(ttstr)` / `TVPAddImportantLog(ttstr)`: 旧来 API (互換目的)。
  それぞれ INFO / WARNING 相当で LogCore 経由に転送されます。
  LogCore 無効時は SDL3 のログ出力に転送されます。

## 書式整形層: tvpfmt

ログ専用のミニ書式整形器です。実体は `common/utils/LogIntf.h` (型定義) と
`common/utils/LogFormat.cpp` (`tvpfmt::vformat` 実装) に閉じており、
外部依存はありません。LogFormat.cpp は LogCore の有効/無効に関わらず
常にリンクされます。

### 経緯

もともとは fmtlib / C++20 `<format>` を切り替えて使う薄いラッパでしたが、
C++20 対応が中途半端な環境で fmtlib 9/10 も `<format>` もビルドできない
ケースが出たため、**ログ用途に必要な機能だけを自前実装** する方針に切り
替えました。fmtlib / `<format>` への依存は完全に削除されています。

UTF-16 の `tjs_char*` / `ttstr` / `tjs_string` は引数を積む時点で UTF-8
へ変換されるため、低層の `TVPLog()` が UTF-8 を受ける前提とそのまま噛み
合います。SDL3 版は整形後の UTF-8 を `SDL_LogMessage` に、plog 版は
`plog::Record` に流します。

### 使える書式

| 書式 | 意味 | 例 |
|---|---|---|
| `{}` | 既定。型から自動選択 | `TVPLOG_INFO("path: {}", path)` |
| `{:d}` `{:i}` | 10 進整数 | `TVPLOG_DEBUG("n={:d}", n)` |
| `{:x}` `{:X}` | 16 進整数 (小/大文字) | `TVPLOG_ERROR("err {:x}", code)` |
| `{:o}` | 8 進整数 | — |
| `{:u}` | 符号なし 10 進 | — |
| `{:s}` | 文字列 (既定と同じ) | — |
| `{:0Nd}` `{:0Nx}` | ゼロ埋め + 幅指定 | `"{:08x}"` で `0000beef` |
| `{:Nd}` `{:Nx}` | 幅指定のみ | — |
| `{:f}` `{:g}` `{:e}` `{:G}` `{:E}` `{:F}` | 浮動小数点 (既定は `%g`) | `"{}"` でも float/double を受け付けます |
| `{{` `}}` | 波括弧リテラル | — |

対応している引数型:

- 真偽値、整数 (`char` / `short` / `int` / `long` / `long long` と各符号なし)
- 浮動小数点 (`float` / `double` / `long double` — 内部は `double`)
- C 文字列 (`char*` / `const char*`)
- `std::string`
- `const tjs_char*` / `ttstr` / `tjs_string` (UTF-8 へ自動変換)
- 任意のポインタ型 (`%p` 相当)

### 使用例

```cpp
TVPLOG_INFO("Loaded GLES {}.{}", major, minor);
TVPLOG_ERROR("OpenGL error occurred: {:08x} {}", error_code, msg);
TVPLOG_DEBUG("rect: {},{},{},{}", left, top, right, bottom);   // float でも OK
TVPLOG_DEBUG("Opening {} (access={})", path_utf8, access);
TVPLOG_INFO("project: {}", projectPath);                        // ttstr も OK
```

### あえて対応していないもの

ログ用途では使われていないため意図的に実装していません。必要になったら
`LogFormat.cpp` の `vformat` / `format_one` を拡張する形で追加してください。

- 位置指定 (`{0}` `{1}`)
- アライン / 埋め文字 (`{:<10}` `{:*>8}`)
- 精度 (`{:.3f}`)
- `{:b}` (2 進)
- ユーザ型向け `formatter<T>` 特殊化 — 呼び出し側で `ttstr` / `std::string`
  へ明示変換してから渡してください。

書式エラー (閉じていない `{` など) は `tvpfmt::format_error` を投げますが、
`TVPLog()` は内部で捕捉して "Log Format error: ..." をメッセージ化するの
でクラッシュしません。

### 将来の再検討

C++20 `<format>` が主要ターゲット (Windows / Linux / macOS / Android /
iOS) の標準ツールチェーンで安定して使えるようになったら、`tvpfmt` を
`<format>` の薄いラッパに差し戻すことを検討してください。その際は:

- `vformat` の仕様差 (例: `{}` に `ttstr` を渡すには `formatter` 特殊化が
  必要) を吸収するアダプタを `LogIntf.h` 側に用意する
- 呼び出し側 (`TVPLOG_*` マクロおよび `tvpfmt::make_format_args` を直接
  使っている箇所 — 現状は `OpenGLError.cpp` のみ) を破壊しないこと
- fmtlib との二股復活は避ける (今回撤去した理由そのものなので)

逆にログ以外の用途で書式整形が必要になった場合は、`tvpfmt` を広げるより
呼び出し側で個別に `snprintf` / `std::to_string` を使うか、C++20
`<format>` が使える前提にビルド要件を引き上げるほうが健全です。

### レベル設定

#### 実行時 (`-loglevel`)

起動時オプション `-loglevel=<level>` で変更可能。値は大文字小文字どちらでも
受付 (`DEBUG` / `debug` どちらでも OK)。指定可能な値:

```
VERBOSE / DEBUG / INFO / WARNING / ERROR / CRITICAL / OFF
```

#### コンパイル時 (`TVPLOG_LEVEL`)

このレベル**未満**のマクロは `do {} while(0)` に展開され、引数も評価されない
(完全 strip)。実行時の `-loglevel` フィルタはこの上で動くので、コンパイル時に
strip されたレベルは実行時に有効化できない点に注意。

| ビルド | 既定 `TVPLOG_LEVEL` | 実行時 `-loglevel` で変更可能な範囲 |
|---|---|---|
| `MASTER` 定義あり | `WARNING` (3) | `WARNING` 〜 `OFF` |
| それ以外 (Release / RelWithDebInfo / Debug) | `DEBUG` (1) | `DEBUG` 〜 `OFF` |

`MASTER` ビルドでは `DEBUG` / `INFO` / `VERBOSE` のマクロは完全に消える。
Release 系はリリース成果物ではないので `DEBUG` を含めて常時 compile-in、
実行時に `-loglevel=DEBUG` を渡すと出力される設計。

#### 細かい上書き (`KRKRZ_LOG_LEVEL`)

CMake オプションで明示指定もできる:

```bash
cmake --preset x64-windows -DKRKRZ_LOG_LEVEL=VERBOSE  # 全レベル compile-in
cmake --preset x64-windows -DKRKRZ_LOG_LEVEL=INFO     # DEBUG/VERBOSE strip
```

値: `VERBOSE` / `DEBUG` / `INFO` / `WARNING` / `ERROR` / `CRITICAL` / `OFF`
(空文字列または未指定なら build type の既定に従う)。

### Sink

`TVPLogSetConsoleSink(hook)` でコンソール出力を乗っ取れます。REPL 側が
起動時にこれを登録し、icline の bbcode でレベル別に色付けしたうえで
プロンプト行に割り込み表示します。

icline (bbcode) は内部的にスレッドセーフではないため、`TVPReplLogSink` は
`g_repl_log_sink_mu` (std::mutex) で `ic_printf` 呼出をシリアライズしています。
これで複数スレッド (main + file cache thread + image load thread 等) からの
DEBUG ログが並列で来ても icline 内で race しません。

ただし REPL の `ic_readline` (input loop) は本 mutex の外側にあるため、
ユーザがキー入力中の prompt 描画と log 出力が同時に走った場合の race は
依然残っています (頻度が低く実害は限定的なため対応保留、完全対策には
icline 側の同期機構が必要)。

### ファイル出力

- `TVPStartLogToFile(bool clear)` — ログファイル出力を開始
- 出力先: `[TVPLogLocation]/krkr.console.log` (UTF-16 LE + BOM)
- `-forcelog=yes|clear` — 起動時にファイル出力を強制開始
- `-logerror=no|clear` — エラー時の自動フラッシュ挙動を制御

### TJS 面

- `Debug.message(...)` / `Debug.notice(...)` — INFO / WARNING 出力
- `Debug.getLastLog(n)` — リングバッファから最新 n 行を取得
- `Debug.addLoggingHandler(func)` / `Debug.removeLoggingHandler(func)`
- `Debug.logLocation` — ログ出力先ディレクトリ (プロパティ)
- `Debug.startLogToFile(clear)` — ファイル出力開始
- `Debug.logToFileOnError` / `Debug.clearLogFileOnError` — エラー時挙動

### スレッド安全性

`TVPLogDispatchLine` は main 以外のスレッド (REPL ファイルチャネル、file cache /
image load スレッド等) からも呼ばれます。LogCore 内部では:

- リングバッファ / important cache / タイムスタンプキャッシュ / ファイル出力は
  `TVPLogStateMutex` (recursive) で保護。
- **TJS logging handler の FuncCall は main thread 限定**。非 main スレッド発の
  行は内部の保留キュー (上限 1024 行、超過時は古い行から破棄) に積まれ、
  main thread がイベントポンプ (`TVPDeliverAllEvents`) / `TVPDrainREPL` 毎に
  `TVPFlushQueuedLoggingEvents()` で配送します。チャネルスレッド上で TJS VM が
  走ってメインスレッドと競合するクラッシュ (旧 -replfile 起動時) の再発防止。
  handler 未登録の間に非 main スレッドから出た行は (従来どおり) handler には
  届きません。
