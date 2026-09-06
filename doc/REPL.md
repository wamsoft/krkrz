# REPL (Read-Eval-Print Loop)

> 吉里吉里2 が本体に持っていたデバッグ窓 (監視式 / コントローラ /
> スクリプトエディタ) は **REPL コマンドとブラウザ UI として復活済み**
> (`.watch` / `.event` / Watch・Pad タブ / コントローラ)。
> 設計と決定理由は [DebugToolsRevival.md](DebugToolsRevival.md)。

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

### 起動オプション一覧

REPL 関連のオプションはここに集約しておく (詳細は各節)。 いずれも独立で、
同時指定できる。

| オプション | 既定 | 内容 |
|---|---|---|
| `-repl[=yes\|no\|new]` | 無効 | コンソール REPL。`new` は新規コンソールを強制 |
| `-replfile=<dir>` | 無効 | ファイルチャネル (**エージェント駆動の本命**) |
| `-replsocket=<name>` | 無効 | abstract unix socket チャネル (Linux 系) |
| `-replweb[=[host:]port]` | 無効 | HTTP + SSE サーバ + ブラウザ UI (既定 127.0.0.1:8899) |
| `-replwebopen=app\|tab\|no` | 条件付き自動 | ブラウザの自動オープン。**端末起動では既定で開かない** |
| `-replwebidle=<秒>\|no` | **5 秒** | ブラウザが居なくなってから終了するまで |
| `-replwebpad=<dir>` | 書込禁止 | Pad タブの [保存] を許すストレージ接頭辞 |
| `-replwatchfile=<path>\|no` | `.krkrz_watch` | 監視式リストの保存先 |
| `-replmodaltimeout=<秒>` | 30 | モーダル応答待ちのタイムアウト (0 = 無限) |

`-nostartup` (startup.tjs を実行しない) と `-loglevel=` は REPL 専用では
ないが、 エージェント駆動でよく併用する。

## コマンド・操作

プロンプトは `krkrz>`、継続行は `...`。TJS 式や文を入力して改行で評価
されます (`;` 無しの式も可)。括弧・クォートが閉じていない間は継続入力に
なります。履歴はカレントディレクトリの `.krkrz_history` に保存。

評価は「式としてパース可能なら式として実行 (結果を表示)、そうでなければ
文として実行」の二択で、パース可否は実行前にコンパイルのみで判定します。
式の実行時例外 (メンバ無し・引数不正など) はその例外メッセージがそのまま
報告されます (以前は文としての再実行にフォールバックしていたため、実行時
例外が「文法エラー」と誤報告され、副作用のある式が二重実行されることが
ありました)。

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
| `.watch` | 監視式の一覧を `id: 式 = 値` で表示 (表示前に全件評価する = 吉里吉里2 の Update ボタン相当) |
| `.watch add EXPR` | 監視式を追加して即評価。 式は空白を含んでよい |
| `.watch rm ID` / `.watch rm all` | 監視式の削除 / 全消し |
| `.watch edit ID EXPR` | 監視式の差し替え (値は «未評価» に戻る) |
| `.watch auto [ms\|on\|off]` | 自動更新の間隔を表示 / 設定。 `on` = 既定 500ms、 `0` = 毎フレーム、 下限 100ms (それ未満は切り上げ) |
| `.event [on\|off\|toggle]` | `System.eventDisabled` の表示 / 切替 (吉里吉里2 コントローラの Event ボタン相当) |

メモリ系コマンドの詳細は `doc/MemoryGuide.md`、パッドオーバレイの詳細は
`doc/PadOverlay.md`、エージェント駆動 API の詳細は後述「エージェント駆動」参照。

### 監視式 (`.watch`)

吉里吉里2 が本体に持っていたデバッグ窓「監視式」の復活
(設計 = `doc/DebugToolsRevival.md`)。 **式のリストを保持して、 まとめて評価し、
式と値を並べて見せる**だけの機能で、 コアは `common/utils/ReplWatch.{h,cpp}`。
REPL コマンドも Web API もブラウザの Watch パネルも**同じコアを共有する**ので、
どのフロントから足しても他のフロントから見える。

- **評価コンテキストは global 固定** (原典どおり)。
- **式の例外は捕まえて `(error) <メッセージ>` を «値» として並べる**。 監視式が
  1 本壊れても REPL もアプリも死なない (原典と同じ)。 コマンド自体の失敗
  (usage / 未知の id) だけがエラー扱いになる。
- 評価は**必ずメインスレッド**。 worker (console / file channel) から来た
  `.watch` は `ReplMainQueue::SubmitTask` でメインへ運んで待つ。 自動更新は
  `TVPDrainREPL()` から毎フレーム見る。
- 値の表示は深さ 2 / compact 固定 (1 行に収める用途なので `.depth` /
  `.compact` とは独立)。
- **式リストと間隔はカレントディレクトリの `.krkrz_watch` に保存**され、
  次回起動で読み戻る (REPL 履歴 `.krkrz_history` と同じ流儀)。
  `-replwatchfile=<path>` で保存先を変更、 `-replwatchfile=no` で無効。
- 自動更新で値が変わったときだけ、 `-replweb` の `watch` チャネルへ
  SSE push する (`GET /sub/watch`)。 無駄な配信を避けるため、 変化が無い
  フレームは何も出さない。

```
krkrz> .watch add System.getTickCount()
1: System.getTickCount() = 24283
krkrz> .watch auto 500
watch auto = 500 ms
krkrz> .watch
1: System.getTickCount() = 25871
```

`0` (毎フレーム) は原典の「リアルタイム」相当。 重い式を入れると描画に効くので、
常用は既定の 500ms を推奨する。

#### 保存ファイル (`.krkrz_watch`)

書式は UTF-8 の行指向テキスト。 `#` で始まる行はコメントで、
`# interval=<ms>` だけが意味を持つ。 それ以外の非空行が式 1 本。

```
# krkrz watch expressions
# interval=500
System.getTickCount()
1+2*3
```

- 書き戻すのは**式の追加 / 削除 / 編集 / 間隔変更のとき**。 評価では書かない。
- 式に改行は入らない (`Add` / `Edit` で空白へ潰している) ので行指向で足りる。
  JSON にしないのは、 読む側にパーサが要るのを避けるため。
- 書けない場所 (読取専用など) では**黙って諦める**。 開発用の付加機能なので、
  保存できないことでアプリを止めない。
- 原典 (吉里吉里2) は environ profile の `[watch]` に式一覧と間隔に加えて
  窓位置・サイズ・列幅・stayontop を持っていた。 窓まわりはブラウザ側の話
  なので持たない。

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

**先頭が `.` の行はドットコマンド**として console / web と同じ `ProcessLine` を
通り、 出力行を改行で連結したものが `result` に入る (`.watch` / `.mem` /
`.cap` 等をエージェントからそのまま叩ける)。 コマンド自体が失敗したときだけ
`ok:false` + `error` になる。 通常の TJS はこれまでどおり共有キューへ直接出す
(`result` = 評価結果 1 個の pretty print という契約を変えないため)。

```bash
krkrz64 data/ -replfile=/tmp/krkrzchan
```

典型フロー (擬似): `win.openMenu()` → `Agent.dialogs()` で状態確認 →
`Agent.click(255,80)` で遷移 → `Agent.captureScreen("cap.png")` → PNG を読んで
目視確認。

#### ⚠ ブロッキング呼出はチャネルごと止まる

チャネルは lockstep なので、**投げたコマンドが返るまで次のコマンドを読まない**。
`Window.showModal()` のように「閉じるまで戻らない」呼出を投げると、
そのコマンドの実行が終わらない = チャネルが応答待ちのまま停止し、
「閉じるためのコマンド」も送れなくなる (`modal`/`modalresp` の応答口を持つ
`System.confirm` 等とは別)。

自動テストから閉じたい場合は、**1 コマンドの中で閉じ手を仕込んでから呼ぶ**:

```tjs
(function() {
    global.__s = demoShell.scene;
    global.__t = new Timer(function() {
        if (isvalid global.__s.modalWin) global.__s.modalWin.close();
    }, "");
    global.__t.interval = 700;
    global.__t.enabled  = true;
    global.__s.openModal();          // ここでブロックする
    global.__t.enabled  = false;
    return global.__s.lastModalNote; // 閉じた後に返る
})()
```

モーダル中もタイマー・連続ハンドラ・描画は動いているのでこれで抜けられる
(コアデモ `window_multi` で実測: 閉じるまで約 1.9 秒ブロック後に復帰)。

#### 文字入力イベントは注入できない

`Agent.keyPress(VK_A)` が注入するのは**キーイベントだけ**で、
`Window.onKeyPress` (文字イベント) は発生しない (文字は OS のテキスト入力経路を
通るため)。 文字入力を伴う検証は Elements の入力欄 + `Agent.text` で行う。

## ブラウザ REPL / Web サーバ (`-replweb`)

`-replweb[=<port>|<host>:<port>]` で 127.0.0.1:8899 (既定) に軽量 HTTP+SSE
サーバが立ち、 ブラウザから REPL を操作できる (`KRKRZ_REPL_WEB` ビルド時のみ)。
端末非依存で選択/コピー/検索がブラウザネイティブに効く。 待受 URL は
`System.replWebURL` で取得できる。 `0.0.0.0:<port>` バインドで LAN 越しの
開発 PC からも接続可 (信頼できるネットワーク限定。 起動ログに警告が出る)。

サーバは起動オプション `-replweb` を付けなくても、 スクリプトから
`WebServer.start([port])` で立ち上げられる (下記 `WebServer` クラス参照)。
自前アプリの UI を載せる場合はこれを使うと、 利用者が `-replweb` を毎回指定
しなくて済む (`-replweb` を付けた場合は本体が起動済みなので二重起動しない)。
GUI (コンソール無し) 起動で `-replweb` 指定時のみ、 loopback バインドなら
起動後に自動でブラウザを開く (アプリモード優先 → 不可なら既定ブラウザ)。

### ブラウザ UI (タブ構成)

`GET /` が返す埋め込みページは **Console / Watch のタブ構成**
(`common/utils/replweb_ui.inc`。 肥大するので `ReplWebServer.cpp` から分離して
`#include` している。 ビルド構成は変更なし)。

| タブ | 中身 |
|---|---|
| Console | 従来のページ (上=ログ / 下=入力)。 `/events` の SSE + `POST /cmd` |
| Watch | 監視式の表 (式 / 値) + 追加・削除・インライン編集 + 自動更新間隔。 `/watch` + `/sub/watch` |
| Pad | スクリプトエディタ。 `POST /pad/exec` + `GET\|POST /pad/file` |

- **本体埋め込みを基本**とし、 `-replweb` だけで完結させる (プラグインもスクリプトも
  要らない)。 案件が自前 UI に差し替える道は `WebServer.serveStatic` で維持。
- Watch の表は **id をキーにその場で更新する** (`innerHTML` の組み直しにすると、
  自動更新のたびにインライン編集中のキャレットと選択が飛ぶ)。
- 式のインライン編集は原典と同じくダブルクリック → Enter 確定 / Esc 取り消し。
- 上のバーはコントローラ相当 (イベント停止 / 終了)。

### ブラウザ UI とアプリの寿命をそろえる

ブラウザを UI にした構成では、 **片方だけ残る**のが実用上いちばん困る
(ウィンドウを閉じたのに本体が残る / 本体が終わったのに «disconnected» の
ページが残る)。 両方向を閉じる。

#### 本体側 — ブラウザが居なくなったら終了 (`-replwebidle`)

| 状態 | 動作 |
|---|---|
| 起動〜**最初の SSE 購読が来るまで** | **待機** (いつまでも終了しない) |
| 購読が 1 本以上ある | 動き続ける |
| 購読が全部消えた | **`<秒>` 後に終了** (`TVPTerminateAsync(0)`) |
| 閉じる合図 (`POST /bye`) を受けた | 猶予を約 2 秒へ前倒し |

- **既定は有効 (5 秒)**。`-replwebidle=<秒>` で秒数指定、
  `-replwebidle=no` / `off` / `0` で無効。
- **武装するのは `-replweb` で起動したときだけ**。 TJS の `WebServer.start()`
  (= `StartOn()`) で立てたサーバは武装しない — アプリが自分で管理している
  サーバを、 ブラウザが閉じたからといって勝手に落とさないため。
- **一度でも購読が来てから武装する**のが肝。 ブラウザを開かないエージェント
  駆動や、 `-replweb` を単なる API 面として使う構成は購読ゼロのままなので、
  既定が有効でも影響を受けない。
- 判定と終了要求はメインスレッド (`TVPDrainREPL` → `TVPReplWeb::CheckIdleShutdown`)。
  HTTP スレッドから終了を叩くより安全。
- 複数タブは購読数で自然に扱える (1 枚閉じても残りが生きていれば畳まない)。

#### 閉じる合図 (`POST /bye`)

切断は **SSE のハートビート送信が失敗して初めて分かる**ので、 黙って閉じられると
気付くまで待たされる。 ページは `pagehide` / `beforeunload` で
`navigator.sendBeacon('/bye')` を投げ、 猶予を前倒しさせる。 届かなくても
`<秒>` 経てば畳まれるので、 **速くなるだけの経路**。

合図は **socket が閉じるより先に届く** (beacon は pagehide 中に飛び、 TCP は
その後で落ちる)。 そのため合図を受けたら 3 秒間 200ms ごとに全 SSE クライアントを
叩き起こし、 `:ping` を失敗させて死んだ socket を落とす。 探り切っても購読が
残るなら «別タブが閉じただけ» なので合図を取り下げる。 武装中はハートビート
間隔も `<秒>` まで詰める (合図が届かなかったときの検知を早めるため)。

#### ブラウザ側 — 本体が居なくなったら閉じる

| 状態 | 動作 |
|---|---|
| 本体が終了を通知 (`/sub/state` の `"exiting": true`) | **即座に** `window.close()` |
| SSE が切れて 5 秒復帰しない (クラッシュ / 強制終了) | `window.close()` |
| `window.close()` が効かない (通常タブ) | 「吉里吉里Z が終了しました」を全面表示 |

終了の通知は `TVPReplWeb::Stop()` が **`g_running` を落とす前に**流す
(落とすと SSE スレッドが送らずに抜けてしまう)。 送り切る時間として 150ms だけ
待ってから畳む。

#### ブラウザの自動オープン (`-replwebopen`)

既定は «ループバック束縛 かつ コンソール無し (= GUI 起動)» のときだけアプリ
モードで開く。 端末から起動したときに勝手に開かないのは、 端末があるなら自分で
開けるし、 CI / エージェント駆動を邪魔しないため。

| 値 | 動作 |
|---|---|
| `-replwebopen=app` (値省略も同じ) | アプリモード (Edge / Chrome の `--app`。 枠なしウィンドウ) |
| `-replwebopen=tab` / `yes` | 既定ブラウザの通常ウィンドウ |
| `-replwebopen=no` / `off` | 開かない (上の自動オープンも抑止) |

**端末から起動しつつブラウザも開きたい**ときはこれを使う。 TJS からは
`WebServer.openBrowser(url, appMode)`。

### 組み込みルート

| パス | 説明 |
|---|---|
| `GET /` | 埋め込み REPL ビューワー (上=ログ / 下=入力の単一 HTML) |
| `GET /events` | エンジンログの SSE ストリーム (直近 2000 行のバックログ付き) |
| `POST /cmd` | body の 1 行を REPL として評価 (ドットコマンド/複数行継続対応)。応答 body は継続入力中なら `"1"` |
| `GET /sub/<channel>` | 汎用 SSE チャネル購読 (`WebServer.broadcast` の配信先。バックログ無し) |
| `GET /watch` | 監視式の一覧 + 現在値 (JSON)。**評価しない**のでポーリングしても安全。`?eval=1` で評価してから返す |
| `POST /watch` | 監視式の操作 (form-urlencoded)。成功なら GET と同じ JSON を返す |
| `GET /sub/watch` | 監視式の push 先 (上の汎用 SSE を使う。自動更新で値が変わったときと、評価を伴わない変更のとき) |
| `POST /pad/exec` | body の TJS スクリプトを**まるごと 1 回**実行 (複数行可)。`{"ok","result","error"}` |
| `GET /pad/file?path=` | ストレージから読む (text/plain) |
| `POST /pad/file?path=` | ストレージへ書く。**`-replwebpad=<dir>` 配下のみ**。未指定なら 403 |
| `GET` / `POST /state` / `GET /sub/state` | コントローラ (`eventDisabled` の取得/設定、終了要求) |
| `POST /bye` | ページを閉じる合図 (`-replwebidle` の猶予を前倒し) |

**組み込みルートは `WebServer.register` / `serveStatic` より先に判定される**
(`HandleConnection` の分岐順)。 上のパスはスクリプト側から上書きできないので、
案件のエンドポイントは別の接頭辞を使うこと (`/api/` `/ui/` など)。

#### `POST /watch` のパラメータ

パラメータは **`application/x-www-form-urlencoded`** で受ける (クエリでも body
でも可、 body 優先)。 本体に JSON パーサを増やさないための選択で、 curl から
`-d 'op=add&expr=...'` で叩けて、 ブラウザからは `URLSearchParams` がそのまま
使える (計画 `doc/DebugToolsRevival.md` §4.3 の JSON body から変更)。

| `op` | 追加パラメータ | 動作 |
|---|---|---|
| `add` | `expr` | 式を追加して評価 |
| `rm` | `id` | 削除 (未知の id は 404) |
| `edit` | `id` `expr` | 差し替えて評価 (未知の id は 404) |
| `clear` | — | 全消し |
| `interval` | `ms` | 自動更新の間隔 (`0` = 毎フレーム / 負値 = オフ / 下限 100ms) |
| `eval` | — | 全件評価のみ |

引数不正は 400 + `{"error":"..."}`、 未知の id は 404、 シャットダウン中は 503。

⚠ form-urlencoded なので **`+` は空白として解釈される**。 式に `+` を含める
ときは `%2B` へエスケープすること (`curl -d 'op=add&expr=1%2B2'`)。
`--data-urlencode` を使うのが確実。

```bash
curl -s localhost:8899/watch
curl -s -X POST -d 'op=add&expr=System.getTickCount()' localhost:8899/watch
curl -s -X POST -d 'op=interval&ms=500'                localhost:8899/watch
curl -N localhost:8899/sub/watch      # 自動更新の push を受ける
```

応答 / push の JSON はどちらも同じ形:

```json
{"interval":500,"entries":[{"id":1,"expr":"System.getTickCount()","value":"32470","error":false}]}
```

`interval` は `-1` = オフ / `0` = 毎フレーム / 正値 = ms。 `error` が true の
エントリは `value` が `"(error) ..."` になる (式が壊れていても一覧は返る)。

### Pad (スクリプトエディタ)

吉里吉里2 の「スクリプトエディタ」窓の相当物。 原典
(`utils/win32/PadFormUnit.cpp`) の機能は **Execute (テキストを実行)** と
**Save** で、 編集操作 (Undo / Cut / Copy / Paste) はブラウザの `textarea` が
持っているので、 サーバに要るのはこの 2 つだけ。

- `POST /pad/exec` は `/cmd` (1 行 + ドットコマンド) と違い、 **本文をまるごと
  1 回**実行する。 複数行の関数定義やループをそのまま流せる。 式なら値が、
  文なら `(void)` が返る (共有キューの «式か文か» 判定に従う)。
- 本文はブラウザの `localStorage` に自動保存する。 Pad は「書いて実行」を
  繰り返す場所なので、 リロードで消えると使い物にならない。 ストレージへの
  書き出しは `[保存]` と役割を分けてある。

#### 書込許可 (`-replwebpad=<dir>`)

**既定は書込禁止 (403)**。 `-replwebpad=<dir>` を指定したときだけ、 その
ストレージ接頭辞の配下へ書ける。 接頭辞比較なので末尾 `/` を内部で補う
(`-replwebpad=work` が `work_other/...` に当たらないように)。

⚠ **これはセキュリティ境界ではない**。 `/cmd` や `/pad/exec` で任意の TJS が
実行できる時点でプロセスの全権限が開いている。 «UI の [保存] をうっかり押して
資材を上書きしない» ための柵として理解すること。 ネットワークへ開く
(`-replweb=0.0.0.0:...`) 場合は、 そもそも信頼できる環境でのみ使う。

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
| `WebServer.start([port])` | **サーバをスクリプトから起動** (127.0.0.1、 既定 8899)。 `-replweb` を付けなくても UI サーバを立てられる。 既に稼働中なら無視。 戻り値 = 稼働中か |
| `WebServer.startAt(host, port)` | バインド先を明示して起動 (`"0.0.0.0"` で全 IF)。 戻り値 = 稼働中か |
| `WebServer.stop()` | サーバを停止 (接続を閉じ accept スレッド終了) |
| `WebServer.openBrowser([url [, appMode=true]])` | url をブラウザで開く。 appMode 時は Edge → Chrome を `--app=<url>` (アプリモード) で試し、 不可なら既定ブラウザ (通常ウィンドウ) へフォールバック。 url 省略で稼働中サーバの URL。 戻り値 = 開けたか |
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

`WebServer.openBrowser` のブラウザ起動は、 「URL を開く」処理 (`TVPShellExecute`
= 既定ハンドラで URL/ファイルを開く。 SDL 版は `SDL_OpenURL`) と、 「プログラムを
引数付きで実行する」処理 (`TVPExecuteProgram` = デスクトップ Windows では App Paths
解決込みの Win32 `ShellExecute`。 SDL 版でも機種依存で動く) を分けて実装している。
アプリモードは `TVPExecuteProgram("msedge.exe", "--app=<url>")` (→ Chrome) で試し、
どちらも起動できなければ `TVPShellExecute(url)` で既定ブラウザにフォールバックする。

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
