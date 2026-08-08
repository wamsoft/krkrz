# REPL (Read-Eval-Print Loop)

`KRKRZ_REPL=ON` でビルドされたバイナリに、コマンドライン引数 `-repl`
(または `-repl=yes`) を付けて起動すると、対話型 TJS シェルが有効になります。
Win / Mac / Linux 対応。行編集は [icline](https://github.com/deths74r/icline)
(isocline フォーク) を利用し、FetchContent で取得されます。

## 起動

```bash
# SDL 版 (console subsystem なのでそのまま動きます)
krkrz -repl data/

# Win32 版 (windowed subsystem なので親コンソールに接続します)
krkrz64 -repl data/
```

`-repl` が無ければ REPL は起動しません (TTY 自動判定は無し)。
`-repl=no` / `-repl=off` / `-repl=false` / `-repl=0` で明示的に無効化も可能。

WIN (windowed subsystem) 版では REPL 起動時に `AttachConsole` で親プロセス
のコンソールを捕まえ、それも無ければ `AllocConsole` で新規確保します。

## コマンド・操作

プロンプトは `krkrz>`、継続行は `...`。TJS 式や文を入力して改行で評価
されます (`;` 無しの式も可)。括弧・クォートが閉じていない間は継続入力に
なります。履歴はカレントディレクトリの `.krkrz_history` に保存。

REPL 特殊コマンド:

| コマンド | 説明 |
|---|---|
| `exit` / `quit` / `Ctrl+D` | REPL を抜けてアプリを `TVPTerminateAsync(0)` で終了 |
| `.help` | ヘルプ表示 |
| `.clear` | 継続入力のバッファをクリア |
| `.depth [N]` | 結果表示の展開深さを表示または設定 |
| `.compact [on\|off]` | 結果表示のコンパクトモード切替 |
| `.mem` | File/Bitmap allocator + GlobalAlloc (Krkrz/SDL) + Process memory + システムアロケータの 1 行サマリ |
| `.memdump` | 詳細メモリ統計をログへダンプ (`TVPHeapDump` = per-allocator + GlobalAlloc + Process memory + システム空き + WINVER の HeapWalk) |
| `.memoverlay [on\|off]` | 画面オーバレイを切替 (SDL3 build) |
| `.mempeakclear` | File/Bitmap allocator + GlobalAlloc collector の peak を current_used に揃える |
| `.sysalloc` | システムアロケータ情報 (空き / 確保可能 / RSS) を 1 行表示 (コンソール機等のプラットフォーム固有値含む) |
| `.padoverlay [on\|off]` | 画面左上にゲームパッド 16 ボタン + 6 軸アナログ値をオーバレイ表示 (SDL3 build)。CLI `-padoverlay=1` でも可 |
| `.filecache` | StorageCache (file 層) の全エントリをログへダンプ |
| `.imagecache` | TVPGraphicCache (decode 層) の全エントリをログへダンプ |
| `.cap [path]` | overlay 込みの実画面を PNG 保存 (`Agent.captureScreen`、省略時 `agent_cap.png`) |
| `.dlg` | アクティブな Elements ダイアログ一覧 (`Agent.dialogs`) |
| `.dlgclose` | 全 Elements ダイアログを強制クローズ (`Agent.closeAllDialogs`) |
| `.click X Y` | (X,Y) にマウスクリックを注入 (`Agent.click`) |

メモリ系コマンドの詳細は `doc/MemoryGuide.md`、パッドオーバレイの詳細は
`doc/PadOverlay.md`、エージェント駆動 API の詳細は後述「エージェント駆動」参照。

## エージェント駆動 (Agent API + ファイルチャネル)

エージェント / 自動テストから krkrz を「外から」操作するための機構 (SDL3 /
WINVER 両対応)。 入力イベント注入・画面キャプチャ・Elements ダイアログ制御を、
REPL (対話) または `-replfile` ファイルチャネル (非対話) のどちらからでも使える。
`KRKRZ_USE_ELEMENTS` + `KRKRZ_USE_REPL` が有効なときに `Agent` クラスが登録される。

### `Agent` TJS クラス

`System` 同様にインスタンス不要でクラスメソッドを呼ぶ。 入力注入は実入力と
同じウィンドウ入力ハンドラ経路 (SDL3 = `SendMouseMessage/SendMessage`、
WINVER = `OnMouse*/OnKey*`) を `AgentInput` seam 経由で通すので、 ゲームにも
Elements ダイアログにも届く (DrawDevice / Window の dialog intercept を経由)。

| メソッド | 説明 |
|---|---|
| `Agent.mouseMove(x, y [, shift])` | マウス移動 (論理座標) |
| `Agent.mouseDown / mouseUp(x, y [, button [, shift]])` | ボタン押下 / 解放 (button: 0=左 1=右 2=中) |
| `Agent.click(x, y [, button [, shift]])` | move + down + up |
| `Agent.wheel(delta, x, y [, shift])` | ホイール (delta は 120 単位) |
| `Agent.keyDown / keyUp / keyPress(vk [, shift])` | キー (vk は `VK_*` 数値) |
| `Agent.text(str)` | アクティブダイアログへ UTF-8 テキスト入力 (input_box 等) |
| `Agent.dialogs()` | アクティブダイアログ記述の配列 `%[index, modal, active, screen, focused, x, y, w, h]` |
| `Agent.dialogTree(index)` | ダイアログ内の id 付き widget 一覧 `%[id, type, value]` (UI ツリー dump)。 どの widget が居るか・現在値を観測でき、 id 指定操作と組で使う |
| `Agent.closeDialog() / closeAllDialogs()` | 最前面 / 全ダイアログを閉じる |
| `Agent.dialogClick(index, id)` | 指定ダイアログの widget を **id で起動** (座標不要)。 focus を即時適用してから Enter (button=click / checkbox=toggle)。 内部は `overlay_session::activate_by_id` |
| `Agent.dialogFocus(index, id)` | 指定 widget へフォーカス移動 |
| `Agent.captureScreen(path [, x, y, w, h])` | overlay 込みの実画面を次フレームで PNG 保存 (即 return、 戻り値 = path) |
| `Agent.lastCapture()` | 直近キャプチャの結果 `%[path, width, height, ok]` |

画面キャプチャは、 アクティブな DrawDevice (既定 `SDLOGLDrawDevice` = GL、 または
`SDLDrawDevice` = SDL_Renderer) の present 直前に backbuffer を読み戻して保存する。
`captureScreen` は内部で `RequestUpdate` を呼ぶのでアイドル時でも 1 フレーム後に
ファイルが出来る。

### `-replfile=<dir>` ファイルチャネル

console (CONIN$) を介さずにエージェントが REPL を駆動するための、 ファイル
ベースのコマンドチャネル。 `-repl` と独立に起動でき (両方同時も可)、 メイン
スレッド実行は共有キュー (`ReplMainQueue`) で console REPL と共用される。

このチャネルと、その応答口を使う file-based modal (`System.confirm` /
`inputString` / ファイル選択を REPL 経由で応答する `modal`/`modalresp`
サブプロトコル) は、 ローカルファイルシステムを使うデスクトップ向け機能で、
CMake の `KRKRZ_REPL_FILE` (既定は `KRKRZ_REPL` に追従) でゲートされる。
端末 (標準入出力) を持たない一部プラットフォーム向けの web-only ビルド
(`KRKRZ_REPL_WEB` のみ) ではリンク外となり、 その場合の modal 呼び出し元は
native / 既定へフォールバックする。 web REPL 用の modal はブラウザ側 + web
インターフェース側の別実装が必要 (未実装)。

プロトコル (`<dir>` 配下、 lockstep):

1. エージェント: コマンド (UTF-8 TJS) を `cmd.tmp` に書き、 `cmd` に rename。
2. チャネル: `cmd` を検出→読取→削除→メイン実行→結果 JSON を `resp.tmp` に
   書き `resp` に rename。
3. エージェント: `resp` の出現を待ち、 読取→削除。 次コマンドへ。

結果 JSON: `{ "ok": bool, "result": "<pretty-printed>", "error": "<msg>" }`。
未読の `resp` が残る間は次コマンドを処理しない (取りこぼし防止)。

```bash
krkrz64 data/ -replfile=/tmp/krkrzchan
```

典型フロー (擬似): `win.openMenu()` → `Agent.dialogs()` で状態確認 →
`Agent.click(255,80)` で遷移 → `Agent.captureScreen("cap.png")` → PNG を読んで
目視確認。

## ブラウザ REPL / Web サーバ (`-replweb`)

`-replweb[=<port>|<host>:<port>]` で 127.0.0.1:8899 (既定) に軽量 HTTP+SSE
サーバが立ち、 ブラウザから REPL を操作できる (`KRKRZ_REPL_WEB` ビルド時のみ)。
端末非依存で選択/コピー/検索がブラウザネイティブに効く。 待受 URL は
`System.replWebURL` で取得できる。 `0.0.0.0:<port>` バインドで LAN 越しの
開発 PC からも接続可 (信頼できるネットワーク限定。 起動ログに警告が出る)。

### 組み込みルート

| パス | 説明 |
|---|---|
| `GET /` | 埋め込み REPL ビューワー (上=ログ / 下=入力の単一 HTML) |
| `GET /events` | エンジンログの SSE ストリーム (直近 2000 行のバックログ付き) |
| `POST /cmd` | body の 1 行を REPL として評価 (ドットコマンド/複数行継続対応)。応答 body は継続入力中なら `"1"` |
| `GET /sub/<channel>` | 汎用 SSE チャネル購読 (`WebServer.broadcast` の配信先。バックログ無し) |

### `WebServer` TJS クラス (拡張登録口)

スクリプト / プラグインがこのサーバへ機能を追加公開するためのクラス
(`System` 同様インスタンス不要)。 サーバ未起動でも登録は保持され、 起動時から
有効になる。 **ハンドラは常にメインスレッドで呼ばれる** (`ReplMainQueue` の
タスクとして dispatch) ため、 TJS / エンジン API を自由に触ってよい。

| メンバ | 説明 |
|---|---|
| `WebServer.register(prefix, handler)` | パスプレフィックス最長一致で動的ハンドラを登録 (同一 prefix は上書き) |
| `WebServer.unregister(prefix)` | ハンドラ解除。 あれば 1 |
| `WebServer.serveStatic(prefix, storageDir)` | prefix 以下の GET を `storageDir + 相対パス` のストレージから配信 (`..` は 403)。 例: `("/ui/", "ui/")` |
| `WebServer.unserveStatic(prefix)` | 静的マウント解除 |
| `WebServer.broadcast(channel, text)` | `/sub/<channel>` の購読者へ text を配信 (改行可、 SSE 複数 data 行に整形) |
| `WebServer.active` | サーバ稼働中か |
| `WebServer.url` | 待受 URL (未稼働なら空文字列) |

ハンドラ呼び出し規約:

```tjs
WebServer.register("/api/foo/", function(req) {
    // req = %[ method, path, query, body(文字列), bytes(octet: body 非空時のみ) ]
    return %[ status : 200, mime : "application/json", body : "{}" ];
});
```

戻り値の解釈: 文字列 = 200 `application/json` / octet = 200
`application/octet-stream` / 整数 = そのステータスで空ボディ / void = 204 /
辞書 = `%[status, mime, body(文字列 or octet)]`。 ハンドラ内の TJS 例外は
500 (本文 = メッセージ) になり `WebServer: handler error:` としてログに出る。

プラグインからは TJS グローバル経由で登録するのが定石 (ネイティブメソッドの
クロージャを渡す。 C++ エクスポート追加は不要):

```tjs
// プラグイン側 POST_REGIST から TVPExecuteExpression で評価する例
if (typeof global.WebServer != "undefined") {
    var api = new MyPluginApi();          // ncbind で登録したクラス
    WebServer.register("/api/mine/", api.handle);
}
```

実装: `common/utils/ReplWebServer.cpp` (サーバ本体) /
`common/utils/ReplWebIntf.cpp` (TJS クラス)。 動的ハンドラと静的配信の
ストレージ読込はどちらも `ReplMainQueue::SubmitTask` でメインスレッドに運んで
実行される (タスクは 1 フレームあたり予算付きで複数件 drain)。 TJS クロージャは
メインスレッド専用 map に隔離され、 HTTP スレッドは prefix 文字列しか触らない。
利用例: krkr_threepp プラグインのブラウザ編集 UI (`/ui/` + `/app/` +
`/api/three/`)。

## 毎フレーム系イベントの連続例外ガード

REPL 駆動中は例外で終了しないため、onDraw / Timer のような繰り返し発火する
ハンドラが例外を投げ続けると同じ例外が延々とログに流れる。これを止めるため、
発火元ごとに連続例外カウンタを持ち、上限に達したら発火元を自動停止する
(`common/base/EventIntf.h` の `tTVPRepeatedExceptionGuard`)。

- 上限: `-eventexceptionlimit=N` (既定 10、`0` で無効 = 従来挙動)
- **Timer**: 連続 N 回で `enabled=false` + キュー破棄 + WARNING。
  `enabled = true` の再設定でガードがリセットされ再開できる
- **OGLDrawDevice.onDraw**: 連続 N 回で発火停止 + WARNING
  (描画サイクル自体は継続し画面はクリアされ続ける)。
  `drawDevice.resumeOnDraw()` で再開
- **continuous handler**: 従来から例外 1 回で自動除去 (変更なし)。
  黙って消えていたのを WARNING ログを出すように変更

なお `TVPPostEvent(TVP_EPT_IMMEDIATE)` は例外を内部で表示して飲み込むため、
発火側では `TVPScriptExceptionShownCount` (`ScriptMgnIntf.h`) の前後差分で
ハンドラの失敗を検知している。同様の周期イベントにガードを足す場合も
この方式を使うこと。

## 結果表示

評価結果は `TVPPrettyPrint(variant, depth, compact)` (`DebugIntf.h` 公開、
TJS では `Debug.prettyPrint(v, depth=2, compact=false)`) で整形されます:

- `void` → `(void)`、null object → `(null)`
- 数値・文字列・octet → `TJSVariantToExpressionString` 相当
- Array → `[ e1, e2, ... ]` (compact は `[e1, e2, ...]`)
- Dictionary → `%[ "k" => v, ... ]`
- Function / Class / Property → `(function)` / `(class)` / `(property)`
- その他 object → `(object: 0x...)`
- 深さ到達 → `[...]` / `%[...]`
- 循環参照 → `(recursion)`

ログ出力は REPL 実行中、icline の bbcode 機能でレベル別に色付けされて
プロンプト行の上に割り込み表示されます (VERBOSE=gray, DEBUG=cyan,
INFO=デフォルト, WARNING=yellow, ERROR=red, CRITICAL=bold red)。

## スレッド構造

REPL ワーカースレッドが `ic_readline` で入力をブロッキング取得し、
完成した式を共有実行キュー `ReplMainQueue` (CV 付き request/response スロット)
に積み、メインスレッドを起床させます。メインスレッドは毎 frame
`TVPDrainREPL()` → `TVPReplMainQueue::Drain()` を呼び出してリクエストを
1 件ずつ取り出し `TVPExecuteExpression` を実行、結果を response スロットに
詰めて CV で worker を起こします。

`-replfile` ファイルチャネルスレッド (`tTVPReplFileChannel`) も同じ
`ReplMainQueue` に提出するため、 console REPL と file channel が共存しても
メインスレッド実行は 1 件ずつ直列化されます (提出は submit mutex で排他)。
`TVPCreateREPL()` が `-repl` / `-replfile` / `-replweb` の有無を見て
console / file channel / web viewer を独立に起動します。 これらの有効化は
CMake の `KRKRZ_REPL` (console) / `KRKRZ_REPL_FILE` (file channel + file-based
modal) / `KRKRZ_REPL_WEB` (browser viewer) で独立に制御でき、 共有基盤
(`ReplMainQueue` / エントリポイント等) は派生フラグ `KRKRZ_REPL_CORE`
(いずれか一つでも有効なら ON) で一括ゲートされます。 file channel と
file-based modal はデスクトップ向けで、 web-only ビルドではリンク外です。

この構造により:

- TJS エンジンのスレッドアフィニティ (メインスレッドのみ) を守りつつ
  ワーカー側で行編集が動く
- 初期起動スクリプト (AM_STARTUP_SCRIPT) が長時間走っていても、完了次第
  REPL リクエストが確実にピックアップされる (`NativeEventQueue` の共有
  `command_que_` を介さないため、他のイベントと競合しない)
- Win32 では worker が `PostThreadMessage(WM_NULL)` でメインスレッドの
  `WaitMessage` を起こす。SDL3 では `SDL_AppIterate` が連続呼び出しされる
  ので起床機構は不要
