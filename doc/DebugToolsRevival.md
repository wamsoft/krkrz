# 吉里吉里2 デバッグ窓の REPL / Web への復活

吉里吉里2 が本体に内蔵していたデバッグ窓 4 種 (コンソール / コントローラ /
監視式 / スクリプトエディタ) を、REPL コマンドと `-replweb` のブラウザ UI
として復活させた記録。**設計と決定理由の SSOT** で、計画時の検討もそのまま
残してある (なぜその形になったかを後から辿れるように)。

**状態: 計画分 (P1〜P5) は完了** — 監視式コア + `.watch` / `.event` +
Web API + Console / Watch / Pad のタブ UI + コントローラ + 式リストの永続化。
以後は運用しながらの手入れ。

| 復活したもの | 使い方 |
|---|---|
| コンソール | REPL 本体 (`-repl` / `-replfile` / `-replweb`) + Console タブ |
| コントローラ | ブラウザ上部のバー (イベント停止 / 終了)、REPL の `.event` |
| 監視式 | REPL の `.watch`、Watch タブ、`GET\|POST /watch` |
| スクリプトエディタ | Pad タブ、`POST /pad/exec` + `GET\|POST /pad/file` |

利用者向けの説明は `doc/REPL.md` (エンジン内部の詳細もこちら)。
umbrella 側は `doc/guide/Console.md` と `doc/topics/core/repl.md`。

## 1. 背景

| 吉里吉里2 の窓 | 吉里吉里2 の TJS API | 吉里吉里Z 本体 | 方針 |
|---|---|---|---|
| コンソール | `Debug.console.visible` | **無し** | ✅ **REPL 本体が代替済み** (`-repl` / `-replfile` / `-replweb`) |
| コントローラ | `Debug.controller.visible` | 無し | ブラウザ UI のツールバーとして復活 (中身は `System.eventDisabled` / 終了 / 他窓の表示) |
| 監視式 | **TJS API 無し** (窓とメニューのみ) | 無し | **REPL コマンド `.watch`** + ブラウザ UI の Watch パネル |
| スクリプトエディタ (Pad) | `Pad` クラス | 無し | ブラウザ UI の編集パネル (テキスト + 実行 + 保存) |

吉里吉里Z の `Debug` に登録されているのは
`message` / `notice` / `startLogToFile` / `logAsError` / `addLoggingHandler` /
`removeLoggingHandler` / `getLastLog` / `prettyPrint` と
`logLocation` / `logToFileOnError` / `clearLogFileOnError` だけで
(`common/utils/DebugIntf.cpp`)、**窓オブジェクトは持たない**。

吉里吉里2 側も窓の TJS 公開は薄く、`Debug.console` / `Debug.controller` が
`utils/win32/DebugImpl.cpp` で `visible` プロパティだけを持つクラスとして
登録されているのみ。**監視式とスクリプトエディタには TJS API が無い**
(窓のメニュー/ツールバーからしか開けない)。

互換レイヤ (`script/Krkr2Compat`) はこれらを WIN32Dialog で TJS 再実装して
いるが、Win32 GUI 依存で SDL / コンソール機ビルドでは動かない。ログの配管
(`addLoggingHandler` / `getLastLog`) は本体に残っているので、**窓だけを
プラットフォーム非依存な REPL / ブラウザ側へ作り直す**のが本計画。

## 2. 原典の仕様 (吉里吉里2 実装の実測)

### 2.1 監視式 — `environ/win32/WatchFormUnit.{h,cpp,dfm}`

- データは**式文字列のリスト**。ListView の 2 列 = `式` / `値`。
- 評価は 1 件ずつ `EvalExpression()`:
  ```cpp
  TVPExecuteExpression(expr, &result);              // 例外は eTJS で捕捉
  item->SubItems->Strings[0] = TJSVariantToReadableString(result);
  // 例外時: "(error) " + e.GetMessage()  ← 窓は死なない
  ```
- `EvalAll()` で全件更新。**更新は手動 (Update ボタン) と自動更新の 2 系統**で、
  自動更新はトグル + 間隔メニュー (リアルタイム / 0.2 / 0.5 / 1 / 3 / 5 / 9 秒)。
- 式の**追加 / 削除 / インライン編集 / ダブルクリック編集**。
- **永続化**: environ profile の `[watch]` セクションに
  `exprlist` (式の一覧) / `interval` / 窓位置・サイズ・列幅 / `stayontop`。
- ポップアップメニューから他の窓 (スクリプトエディタ / コンソール / コントローラ) を開く、
  重要ログのコピー、常に手前に表示。

### 2.2 コントローラ — `environ/win32/MainFormUnit.{h,cpp}`

吉里吉里2 の「コントローラ」はメインフォームそのもので、ツールバーは
**ScriptEditor / Console / Watch / Event / Exit** の 5 ボタン。

- `EventButtonClick` → `TVPSetSystemEventDisabledState(...)` = **`System.eventDisabled`**
- Exit → アプリ終了
- 残り 3 つは他の窓の表示トグル

### 2.3 スクリプトエディタ (Pad) — `utils/win32/PadFormUnit.{h,cpp}` / `PadImpl.cpp`

複数行テキスト窓。メニューは
**Undo / Cut / Copy / Paste / Execute / Save**、および他窓へのリンク・
重要ログのコピー・常に手前。`Execute` が本体の機能で、テキストを実行する。

## 3. 吉里吉里Z 側の受け皿 (現状)

| 部品 | 実装 | 用途 |
|---|---|---|
| `TVPExecuteExpression` / `TJSVariantToReadableString` | `common/base/FuncStubs.cpp` 経由で利用可 | **監視式の評価と表示に必要なものは既に揃っている** |
| `TVPPrettyPrint(v, depth, compact)` | REPL の結果整形 | 監視式の値表示にも流用できる (`.depth` / `.compact` と設定を共有) |
| `TVPReplMainQueue::Submit` / `SubmitTask` | `common/utils/ReplMainQueue.cpp` | **評価は必ずメインスレッド**。worker/HTTP スレッドからはこれで運ぶ |
| `tTVPReplThread::ProcessLine` | `common/utils/REPL.cpp` | ドットコマンドの追加口。console / file / web の 3 フロント共通 |
| `TVPDrainREPL()` | 毎フレーム呼ばれる | 自動更新のタイマ処理を載せる場所 |
| HTTP + SSE サーバ | `common/utils/ReplWebServer.cpp` | ルート追加口。`/` (埋め込み HTML) / `/events` / `/cmd` / `/sub/<ch>` |
| `TVPReplWeb::BroadcastChannel(ch, payload)` | 同上 (C++ から直接呼べる) | 監視式の push 配信に使う |
| `TVPReplWeb::RegisterHandler / RegisterStatic` | TJS `WebServer` クラスの実体 | スクリプト側から UI を足す道 (本計画では本体埋め込みを基本とする) |
| `System.eventDisabled` / `System.exit` / `System.terminate` | `common/base/SystemIntf.cpp` ほか | **コントローラの実機能はここに既にある** |

## 4. 設計

### 4.1 監視式コア — `common/utils/ReplWatch.{h,cpp}` (新規)

プラットフォーム非依存。UI (REPL コマンド / Web) は薄い前段に徹する。

```cpp
namespace TVPReplWatch {
    struct Entry { int id; ttstr expr; ttstr value; bool error; bool evaluated; };

    void     NoteMainThread();                // TVPCreateREPL がメインから呼ぶ
    int      Add(const ttstr& expr);          // 追加 (id を返す)
    bool     Remove(int id);                  // 削除
    bool     Edit(int id, const ttstr& expr);
    void     Clear();
    std::vector<Entry> List();                // スナップショット (thread-safe)
    size_t   Count();

    void     EvaluateAll();                   // メインスレッドで全件評価
    bool     EvaluateAllOnMain();             // worker から: キューへ運んで待つ
    void     SetInterval(int ms);             // 0 = 毎フレーム / <0 = 自動更新オフ
    int      GetInterval();
    void     Drain(tjs_uint64 now_ms);        // TVPDrainREPL から呼ぶ
    std::string ToJson();                     // P2 の GET /watch と push 用
}
```

- 評価は `TVPExecuteExpression` → 成功なら `TVPPrettyPrint`、`eTJS` は捕捉して
  `(error) <message>` を値にする (**原典と同じく窓/REPL は死なせない**)。
- 値が変化した場合のみ `TVPReplWeb::BroadcastChannel("watch", json)` で push
  (無駄な配信を避ける)。
- リストは mutex 保護。評価は必ずメインスレッド (`Drain` はメインスレッドから呼ばれる)。
- 値の表示は深さ 2 / compact 固定。 1 行に収める用途なので、 `.depth` /
  `.compact` (REPL の結果表示設定) とは共有しない。

実装で足したもの (計画からの差分):

- `evaluated` — 一度でも評価したか。 `.watch edit` 直後を «(not evaluated)»
  と表示するため。
- `EvaluateAllOnMain()` — `.watch` は worker スレッド (console / file channel)
  で処理されるので、 評価だけ `ReplMainQueue::SubmitTask` でメインへ運ぶ。
  メインから呼ばれた場合はその場で評価する。 どちらかを判定するために
  `NoteMainThread()` を `TVPCreateREPL()` (メインスレッド) から呼ぶ。
- `ToJson()` — P2 の `GET /watch` と自動更新 push で同じ形を使うので、
  コア側に置いた。
- 評価は **mutex を離してから**行う (スナップショット → 評価 → id で書き戻し)。
  式が TJS を呼ぶので、 その中からリストが触られても固まらないようにするため。


### 4.2 REPL コマンド (`REPL.cpp` の `ProcessLine` に追加)

| コマンド | 動作 |
|---|---|
| `.watch` | 一覧を `id: 式 = 値` で表示 (表示前に全件評価) |
| `.watch add <expr>` | 式を追加して即評価 |
| `.watch rm <id>` | 削除 (`.watch rm all` で全消し) |
| `.watch edit <id> <expr>` | 式の差し替え |
| `.watch auto [ms\|off]` | 自動更新の間隔表示/設定 (`0` = 毎フレーム) |
| `.event [on\|off]` | `System.eventDisabled` の表示/切替 (コントローラの Event 相当) |

`.help` にも追記する。`exit` / `quit` が既にあるので Exit ボタン相当は追加不要。

`.event` は引数なしで**表示のみ** (`.event` と打っただけでイベントが止まるのは
事故のもと)。切替は `on` / `off` / `toggle` を明示する。再有効化で
`TVPDeliverAllEvents()` が走るので、読み書きとも `ReplMainQueue::SubmitTask` で
メインスレッドへ運ぶ。

**ファイルチャネル (`-replfile`) のドットコマンド対応**: 従来 `-replfile` は
受け取った行を無条件に TJS として `ReplMainQueue::Submit` へ流していたため、
ドットコマンドが一切使えなかった (§7 の検証計画が成立しない)。
**先頭が `.` の行だけ `ProcessLine` へ通し**、出力行を改行で連結して
`result` に入れるようにした (`ReplFileChannel.cpp`)。通常の TJS は従来経路の
まま = `result` は「評価結果 1 個の pretty print」という契約を変えない。
これで console / file / web の 3 フロントすべてでドットコマンドが使える。

### 4.3 Web API (`ReplWebServer.cpp` にルート追加)

| ルート | 内容 |
|---|---|
| ✅ `GET /watch` | 監視式の一覧 + 現在値 (JSON)。**評価しない**のでポーリング安全。`?eval=1` で評価してから返す |
| ✅ `POST /watch` | `op=add/rm/edit/clear/interval/eval` (**form-urlencoded**。下記) |
| ✅ `GET /sub/watch` | 既存の汎用 SSE。自動更新の push 先 (`BroadcastChannel("watch", ...)`) |
| ✅ `POST /pad/exec` | body を**まるごと 1 回**実行 (`ReplMainQueue::Submit`)。結果/例外を JSON で返す |
| ✅ `GET /pad/file?path=` / `POST /pad/file` | 編集パッドの読み書き (Storages 経由)。**書込は `-replwebpad=<dir>` 配下のみ**、未指定なら 403 |
| ✅ `GET /state` / `POST /state` / `GET /sub/state` | コントローラ相当: `eventDisabled` の取得/設定、終了要求 (§4.5) |
| ✅ `POST /bye` | ページを閉じる合図 (§4.6) |

JSON の組み立ては本体に JSON ライブラリを増やさず、必要最小限の手書きエスケープで足りる
(監視式の値は文字列 1 個。既存 SSE も同様に手書きしている)。

**P2 実装時の変更 — リクエストは JSON body ではなく form-urlencoded にした**。
組み立て (レスポンス) は手書きで足りるが、**解析 (リクエスト) を手書きすると
JSON パーサを 1 本抱えることになる**ため。 form ならクエリ文字列のデコーダを
使い回せて、curl からは `-d 'op=add&expr=...'`、ブラウザからは
`URLSearchParams` がそのまま使える。 パラメータはクエリでも body でも取る
(body 優先)。

| `op` | 追加パラメータ | 動作 |
|---|---|---|
| `add` | `expr` | 式を追加して評価 |
| `rm` | `id` | 削除 (未知の id は 404) |
| `edit` | `id` `expr` | 差し替えて評価 (未知の id は 404) |
| `clear` | — | 全消し |
| `interval` | `ms` | 自動更新の間隔 |
| `eval` | — | 全件評価のみ |

成功なら常に GET と同じ状態 JSON を返す (P3 のパネルが 1 往復でモデルを
差し替えられるように)。引数不正は 400 + `{"error":"..."}`。

**`GET /watch` は既定で評価しない**。`.watch` (一覧) が評価を伴うのとは意図的に
違えてある — GET が任意の TJS を走らせるのは驚きが大きく、ポーリングしても安全で
あってほしいため。原典の Update ボタン相当は `?eval=1` か `op=eval`。

**評価を伴わない変更 (削除 / 全消し / 間隔変更) も push する**
(`TVPReplWatch::BroadcastState()`)。`EvaluateAll()` の「値が変わったら push」だけ
だと、全消しは `entries` が空で評価自体が走らないため、ブラウザが消える前の
一覧を表示したままになる。

### 4.4 ブラウザ UI

現在の `/` は「上=ログ / 下=入力」の単一 HTML (`kHtmlPage`)。ここを
**タブ構成**に拡張する:

```
[ Console ] [ Watch ] [ Pad ]        ← タブ
--------------------------------------------------
ヘッダのツールバー (コントローラ相当):
  [イベント停止] [キャプチャ] [ダイアログ一覧] [終了]
--------------------------------------------------
Console: 既存 (/events の SSE + /cmd)
Watch  : 表 (式 / 値) + 追加削除 + 自動更新間隔セレクタ (/watch + /sub/watch)
Pad    : テキストエリア + [実行] [保存] (/pad/exec + /pad/file)
```

- **本体埋め込みを基本**とし、`-replweb` だけで完結させる (プラグイン/スクリプト不要)。
- 既存の `WebServer.serveStatic` はそのまま残るので、案件側が自前 UI に差し替える道も維持する。
- 埋め込み HTML が肥大するので、`kHtmlPage` は 1 ファイル 1 定数のままにせず
  `common/utils/replweb_ui.inc` 等へ分離する (ビルドは変更なし)。
- ⚠ **MSVC は 1 個の文字列リテラルが 16380 バイトまで** (C2026)。タブを足すと
  すぐ超えるので、`replweb_ui.inc` の raw string は複数に割ってある
  (隣接連結で 1 本になる)。タブを増やしたら割り直すこと。

**P3 実装メモ**:

- タブは Console / Watch の 2 枚で入れた。Pad は P4、ツールバーは P5 なので
  タブ枠だけ用意して中身は後から足す (`.tab[data-tab=...]` + `<section class=panel>`
  を 1 組増やすだけ)。
- Watch の表は **id をキーにその場で更新する** (`innerHTML` を組み直すと、
  自動更新のたびにインライン編集中のキャレットと選択が飛ぶ)。行の追加/削除だけ
  DOM を触り、値セルは textContent の差し替えで済ませている。
- 自動更新間隔のセレクタはサーバから push された値に追従させるが、
  **セレクタにフォーカスがある間は書き換えない** (操作中に値が飛ぶのを防ぐ)。
- 式のインライン編集はダブルクリック → Enter 確定 / Esc 取り消し (原典と同じ)。
  取り消しは `GET /watch` で引き直してサーバの値へ戻す。

**P4 実装メモ (Pad)**:

- `textarea` 1 枚 + `[実行 (Ctrl+Enter)]` + パス欄 + `[読込]` `[保存]`。原典の
  Undo / Cut / Copy / Paste は `textarea` が持っているので足さない。
- 本文は **`localStorage` に自動保存**する。Pad は「書いて実行」を繰り返す
  場所なので、リロードやアプリ再起動で消えると使い物にならない。ストレージへの
  書き出しは `[保存]` (本体側で `-replwebpad` が要る) と役割を分ける。
- 結果はパネル下部に出す (成功は緑 / 例外は赤)。`Debug.message` 等のログは
  従来どおり Console タブへ流れる。

### 4.5 コントローラ (P3 で先行実装)

計画では P5 だったが、 タブを入れた時点で上のバーが空いたので先に入れた。
原典 (`environ/win32/MainFormUnit.cpp`) のツールバーは
**ScriptEditor / Console / Watch / Event / Exit** の 5 つ。 前 3 つはこの UI では
タブなので、 バーに残るのは **Event と Exit** だけ。

| ルート | 内容 |
|---|---|
| `GET /state` | `{"eventDisabled": bool}` |
| `POST /state` | `op=event value=on\|off\|toggle` / `op=exit` |
| `GET /sub/state` | 状態の push |

状態は毎フレーム `PublishStateIfChanged()` で変化を見て push するので、
**ゲーム自身が `System.eventDisabled` を変えてもブラウザの表示が追従する**。
`op=exit` は応答を返してから `TVPTerminateAsync(0)` する (ブラウザ側の fetch が
失敗しないように)。

### 4.6 ブラウザ UI とアプリの寿命をそろえる

計画には無かったが、 ブラウザを UI にする構成では**片方だけ残る**のが実用上
いちばん困るので P3 と同時に入れた。 両方向を閉じる。

#### 本体側 — ブラウザが居なくなったら終了 (`-replwebidle`)

| 状態 | 動作 |
|---|---|
| 起動〜**最初の SSE 購読が来るまで** | 待機 (いつまでも終了しない) |
| 購読が 1 本以上ある | 動き続ける |
| 購読が全部消えた | `<秒>` 後に `TVPTerminateAsync(0)` |
| 閉じる合図 (`POST /bye`) を受けた | 猶予を約 2 秒へ前倒し |

- **既定は有効 (5 秒)**。`-replwebidle=no` / `off` / `0` で無効。
- **一度でも購読が来てから武装する**のが肝。 購読を張らないエージェント駆動や
  API 面としての利用は、 既定が有効でも落ちない。
- 判定と終了要求はメインスレッド (`TVPDrainREPL` → `CheckIdleShutdown`)。

#### 閉じる合図 (`POST /bye`) — elements_console の「番人」から取り込み

`elements_console` の `scripts/uitool/server.py` が同じ問題を先に解いていた
(クラス `_Life`)。 あちらは **2 秒ごとの `/api/heartbeat` + 閉じ際の
`sendBeacon("/api/bye")`** で deadline を延ばす / 前倒しする方式。

こちらは生存信号を **SSE 購読そのもの**にしているので専用ハートビートは要らない
(張りっぱなしなので fetch より確実) が、 **切断の検知が遅い**という弱点がある。
そこを補うのが向こうから借りた `bye` ビーコンで、 ページは
`pagehide` / `beforeunload` で `navigator.sendBeacon('/bye')` を投げる。

合図は **socket が閉じるより先に届く** (beacon は pagehide 中に飛び、 TCP は
その後で落ちる)。 1 回叩き起こすだけでは «まだ生きている» と判定されたので、
**合図から 3 秒間 200ms ごとに全 SSE クライアントを叩き起こし**、 `:ping` を
失敗させて死んだ socket を落とす。 探り切っても購読が残るなら «別タブが閉じた
だけ» なので合図を取り下げる。 武装中はハートビート間隔も `<秒>` まで詰める。

意図的に取り込まなかったもの:

- **STARTUP_GRACE (60 秒でブラウザが来なければ終了)** — uitool は「ブラウザを
  開くのが前提のツール」なので正しいが、 `-replweb` は API 面としても使うので
  «一度も来なければ落ちない» を選んだ。
- **親 stdin の EOF で終了** — 吉里吉里は GUI アプリで stdin を持たないことが
  多いので不適。

#### ブラウザ側 — 本体が居なくなったら閉じる

| 状態 | 動作 |
|---|---|
| 本体が終了を通知 (`/sub/state` の `"exiting": true`) | 即座に `window.close()` |
| SSE が切れて 5 秒復帰しない (クラッシュ / 強制終了) | `window.close()` |
| `window.close()` が効かない (通常タブ) | 「吉里吉里Z が終了しました」を全面表示 |

終了の通知は `TVPReplWeb::Stop()` が **`g_running` を落とす前に**流す (落とすと
SSE スレッドが送らずに抜けてしまう)。 送り切る時間として 150ms 待ってから畳む。

#### ブラウザの自動オープン (`-replwebopen`)

既定は «ループバック束縛 かつ コンソール無し (= GUI 起動)» のときだけ app モード。
端末から起動したときに勝手に開かないのは、 端末があるなら自分で開けるし、
CI / エージェント駆動を邪魔しないため。 `-replwebopen=app|tab|no` で明示指定。

### 4.7 式リストの永続化 (P5)

保存先は **カレントディレクトリの `.krkrz_watch`** (REPL 履歴 `.krkrz_history` と
同じ流儀)。 `user://` は SDL ビルド限定で WINVER から使えないので見送った。
`-replwatchfile=<path>` で差し替え、 `=no` で無効。

書式は UTF-8 の行指向テキスト:

```
# krkrz watch expressions
# interval=500
System.getTickCount()
1+2*3
```

- `#` 始まりはコメントで、 `# interval=<ms>` だけが意味を持つ。 それ以外の
  非空行が式 1 本。 **JSON にしないのは、 読む側にパーサが要るのを避けるため**
  (書く側は手書きエスケープで足りるが、 読む側はそうはいかない)。
- 式に改行は入らない — `Add` / `Edit` が改行を空白へ潰す (`NormalizeExpr`)。
  これで行指向が壊れない。
- 書き戻すのは**式の追加 / 削除 / 編集 / 間隔変更のとき**。 評価では書かない。
  読込中は `g_loading` で書き戻しを止める (1 行ごとに保存し直すのは無駄なうえ、
  途中で落ちるとファイルを削り取ってしまう)。
- 書けない場所 (読取専用など) では**黙って諦める**。 開発用の付加機能なので、
  保存できないことでアプリを止めない。
- 読込は `TVPCreateREPL()` から `InitPersistence()` を呼ぶ (メインスレッド)。

原典 (吉里吉里2) は environ profile の `[watch]` に式一覧と間隔に加えて
窓位置・サイズ・列幅・stayontop を持っていた。 窓まわりはブラウザ側の話なので
持たない。

## 5. フェーズ分け

| # | 内容 | 完了条件 |
|---|---|---|
| ✅ P1 | `ReplWatch` コア + `.watch` / `.event` コマンド | `-replfile` から add/list/auto を流して値が更新される (§7 で実測済み) |
| ✅ P2 | `GET/POST /watch` + `/sub/watch` への push | curl で JSON 取得と SSE 受信ができる (§7 で実測済み) |
| ✅ P3 | ブラウザ UI のタブ化 + Watch パネル | ブラウザで式の追加/削除/自動更新が回る (§7 で実測済み) |
| ✅ P4 | Pad パネル (`/pad/exec` + `/pad/file`) | ブラウザでスクリプトを書いて実行・保存できる (§7 で実測済み) |
| ✅ P5 | コントローラ相当のツールバー + 永続化 | ツールバーは P3 で先行実装 (§4.5)。式リストは `.krkrz_watch` へ保存し、**再起動しても残る** (§4.7) |

P1 だけでも「監視式を REPL コマンドとして復活」という要件は満たせるので、
P1 を先に完結させてから P2 以降へ進んだ。P2 まででブラウザ UI を書くための
サーバ側は揃っているので、P3 は HTML/JS だけの作業になる。

## 6. 決定事項 / 残る未決

P1 着手時 (2026-09-06) に決めたもの:

1. **式リストの永続化** — P1 では「しない」で通し、**P5 で a) を採用**した:
   カレントディレクトリの `.krkrz_watch` (REPL 履歴 `.krkrz_history` と同じ流儀)。
   b) の `user://` は SDL ビルド限定で WINVER から使えないため見送り。
   `-replwatchfile=<path>` で差し替え、`=no` で無効。詳細 = §4.7。
2. **評価コンテキストは global 固定** (原典どおり)。`incontextof` 指定は
   足さない。必要なら式の中で `incontextof` を書けば済む。
3. **自動更新は既定 500ms / 下限 100ms / `0` = 毎フレームは許可**。
   `.watch auto on` が 500ms、1〜99 は 100 へ切り上げ、負値でオフ。
   毎フレームは原典の「リアルタイム」相当なので残すが、重い式では描画に
   効くので常用は勧めない。
4. **UI 文言は日本語** (既存の埋め込み HTML を踏襲) — P3 以降で適用。

P4 着手時 (2026-09-06) に決めたもの:

5. **`/pad/file` の書込許可範囲 = 既定は書込禁止**。`-replwebpad=<dir>` を
   指定したときだけ、そのストレージ接頭辞の配下へ書ける (403 が既定)。
   接頭辞比較なので末尾 `/` を内部で補い、`work` が `work_other/...` に
   当たらないようにする。
   **これはセキュリティ境界ではない** — `/cmd` や `/pad/exec` で任意の TJS が
   実行できる時点でプロセスの全権限が開いている。「UI の [保存] をうっかり
   押して資材を上書きしない」ための柵、と位置づける。

**計画分の未決はすべて解消した**。

## 7. 検証

### P1 実測 (2026-09-06、`x64-windows` = SDL3 / Release、`-replfile` チャネル)

| 確認項目 | 結果 |
|---|---|
| `.watch add System.getTickCount()` → `.watch` の繰り返し | 値が毎回変わる (24283 → 24816 → 25083) |
| 空リストの `.watch` | `(no watch expressions)` |
| `.watch edit` / `.watch rm ID` / `.watch rm all` | いずれも意図どおり |
| `.watch auto` の表示 / `on` / `50` / `0` / `off` | `off` → `500 ms` → `100 ms` (下限へ切り上げ) → `every frame` → `off` |
| 自動更新が実際に走るか (副作用カウンタ式で計測) | `auto off` = 1 秒で 0 回、`auto 100` = 1 秒で 13〜14 回、`auto 0` = 1 秒で 76 回 (≒ fps)、`auto off` に戻すと停止 |
| 例外を投げる式 (`nosuch.member`) | `(error) メンバ "nosuch" が見つかりません` を**値として**表示。以後の `.watch` も TJS 評価も正常 |
| `.event` / `.event on` / `.event off` | 表示のみ / `System.eventDisabled == 1` を TJS 側でも確認 / 復帰 |
| `.event maybe` (不正引数) | usage を返すだけで状態は変わらない |

計測に使った副作用式:
`.watch add (global.wcount = global.wcount + 1)` → `global.wcount` を別コマンドで読む。
「一覧表示が評価を伴う」ため、これが「自動更新だけで評価されたか」を分離できる唯一の形。

### P2 実測 (2026-09-06、`x64-windows` = SDL3 / Release、curl のみ)

| 確認項目 | 結果 |
|---|---|
| `GET /watch` (空) | `{"interval":-1,"entries":[]}` |
| `POST op=add` ×2 | 追加した式が値つきで返る |
| `GET /watch` を続けて 2 回 | 値が動かない (評価しない設計どおり) |
| `GET /watch?eval=1` | `System.getTickCount()` の値が動く |
| `POST op=edit` / `op=rm` / `op=clear` / `op=interval` | いずれも意図どおり。`ms=50` は 100 へ切り上げ |
| 例外を投げる式 | `"value":"(error) メンバ \"nosuch\" が見つかりません","error":true` として一覧に並ぶ |
| エラー系 | 未知 id = 404 / `op=add` に expr 無し = 400 / 未知 op = 400 / `DELETE` = 405 |
| `GET /sub/watch` の push | add → 1 本、`interval=500` → 1 本、以後 500ms ごとに値が動くたび 1 本、`clear` → 空の 1 本 |
| **値が変わらないときは push しない** | 定数式 `40+2` + `auto 200ms` で 3 秒間、フレームは 2 本のみ (add と interval の分だけ) |
| フロント間の一貫性 | `-replfile` の `.watch add` が SSE 購読者にも push される |

### P3 実測 (2026-09-06、Edge を CDP で駆動)

`--remote-debugging-port` で開いた Edge に繋いで DOM を直接叩いた
(手順 = memory `reference_cdp_edge_devtools_driving`)。

| 確認項目 | 結果 |
|---|---|
| タブ | Console / Watch の切替で `hidden` が入れ替わる |
| 式の追加 (入力 + ボタン) | 行が増え、値が入る。「監視式はまだありません」が消える |
| 自動更新 500ms | `System.getTickCount()` の値が 2 秒で動き、定数式 `40+2` は動かない |
| セレクタの追従 | サーバ側 (`.watch auto 1000`) の変更が select に反映される |
| 例外を投げる式 | `(error) メンバ "nosuch" が見つかりません` が赤 (`c-val error`) で並ぶ |
| インライン編集 | dblclick → contenteditable、書き換えて Enter で式と値が入れ替わる |
| 削除 | × ボタンでその行だけ消える |
| クロスフロント | `-replfile` の `.watch add` がブラウザへ SSE で届く (リロード不要) |
| Console タブ | 裏に回してもログを受け続け、`status` は connected のまま |

### コントローラ / 寿命の実測 (2026-09-06)

**コントローラ (Edge を CDP で駆動)**

| 確認項目 | 結果 |
|---|---|
| 初期表示 | `イベント: 有効` (class `subtle on`) |
| ブラウザから切替 | `イベント: 停止中` になり、`GET /state` も `{"eventDisabled":true}` |
| **ゲーム側 (TJS) から変更** | `System.eventDisabled = true/false` の両方向でブラウザ表示が追従 (`/sub/state` の push) |
| curl での `op=event` on/off/toggle | 意図どおり。`System.eventDisabled` を TJS 側でも確認 |
| エラー系 | 未知 op = 400 / `DELETE` = 405 |

**寿命 (`-replwebidle` / `/bye` / ブラウザ側の自動クローズ)**

| 確認項目 | 結果 |
|---|---|
| 既定 (オプション無し) | ログに `idle shutdown armed (5 s ...)`。**未接続のまま 8 秒放置しても終了しない** |
| `-replwebidle=no` | ログに `idle shutdown disabled`。武装しない |
| ブラウザで開いて離脱 (`-replwebidle=30`) | **数秒で終了** = `bye` ビーコンが効いている (合図が無ければ 30 秒待つはず) |
| ブラウザの `[終了]` ボタン | 本体が終了し、**ページも即座に閉じた** (`exiting` の push) |
| 本体を強制終了 (合図なし) | ページが 5 秒後に自分で閉じた |
| `-replwebopen=app` (端末から起動) | ログに `open browser (app mode) -> ... [ok]`、ウィンドウが開く |
| 既定 (端末から起動・オプション無し) | ログに `browser auto-open skipped`、開かない |

WINVER (`x64-windows-win`) でもページ配信 / `/watch` / `/state` / アイドル終了を確認。

### P4 実測 (2026-09-06、curl + Edge を CDP で駆動)

| 確認項目 | 結果 |
|---|---|
| `POST /pad/exec` 単純式 | `1+2*3` → `{"ok":true,"result":"7"}` |
| 同 複数行 (var / for / 文) | `ok:true`、値は `(void)` (文なので。共有キューの判定どおり) |
| 同 例外 | `{"ok":false,"error":"Error: メンバ \"nosuch\" が見つかりません"}` |
| 同 空 body | 400 |
| `GET /pad/file?path=` | 中身が text/plain で返る。無いパスは 404、`../` は 400 |
| `POST /pad/file` (`-replwebpad` 無し) | 403 `saving is disabled ...` |
| 同 (`-replwebpad=work`、許可外) | 403 `outside the allowed directory (work/)` |
| 同 (紛らわしい `work_other/a.tjs`) | **403** (末尾 `/` を補っているので前方一致で誤許可しない) |
| 同 (許可内 `work/out.tjs`) | 200、実ファイルに書けていることを確認 |
| ブラウザ: Ctrl+Enter / 実行ボタン | `=> (void)` / `=> 42` がパネル下部に出る |
| ブラウザ: 読込 → 編集 → 保存 → 読み戻し | 往復して内容が一致 |
| ブラウザ: 許可外へ保存 | ステータスに赤でサーバのメッセージが出る |
| ブラウザ: 下書き | `localStorage` に残る |

### P5 実測 (2026-09-06)

| 確認項目 | 結果 |
|---|---|
| 式 2 本 + `interval=500` を入れて終了 | `.krkrz_watch` に式と間隔が書かれる |
| 再起動 | `restored 2 expression(s)` のログ。`/watch` に式・間隔とも戻る |
| `-replwatchfile=no` | 読まない (空で起動)。既定ファイルも書き換えない |
| `-replwatchfile=mywatch.txt` | 既定ファイルを読まず、指定先へ書く |
| `op=clear` | ファイルからも式が消える (ヘッダだけ残る) |

### まだやっていない検証

- **負荷**: 重い式 (大きな配列の展開) を登録した状態で `DrawStats` を見て
  drain 時間の増加を確認 (`doc/DrawStats.md`)。自動更新を毎フレームにする
  運用が出てきたら測る。

## 8. 参照

- P1 の実装: `common/utils/ReplWatch.{h,cpp}` (コア) /
  `common/utils/REPL.cpp` (`.watch` / `.event` / `TVPDrainREPL` のフック) /
  `common/utils/ReplFileChannel.cpp` (ドットコマンドの取り込み) /
  `sources.cmake`
- P2 の実装: `common/utils/ReplWebServer.cpp` (`HandleWatch` + ルート追加) /
  `common/utils/ReplWatch.{h,cpp}` (`BroadcastState`)
- P5 の実装: `common/utils/ReplWatch.{h,cpp}` (`InitPersistence` / `Save` /
  `SaveLocked` / `NormalizeExpr`) / `common/utils/REPL.cpp` (起動時の読込)
- P4 の実装: `common/utils/ReplWebServer.cpp` (`HandlePadExec` /
  `HandlePadFile` / `WriteStorageOnMain` / `-replwebpad`) /
  `common/utils/replweb_ui.inc` (Pad タブ)
- P3 の実装: `common/utils/replweb_ui.inc` (埋め込み UI 本体) /
  `common/utils/ReplWebServer.cpp` (`#include` へ差し替え + `/state` + `/bye` +
  `-replwebidle` / `-replwebopen`) / `common/utils/REPL.cpp`
  (`CheckIdleShutdown` / `PublishStateIfChanged` のフック) / `sources.cmake`
- 寿命まわりの参考にした先行実装: `elements_console` の
  `scripts/uitool/server.py` クラス `_Life` (「番人」)
- 吉里吉里Z: `common/utils/REPL.cpp` / `ReplWebServer.cpp` / `ReplMainQueue.cpp` /
  `ReplWebIntf.cpp`、`common/utils/DebugIntf.cpp`、`doc/REPL.md`
- 吉里吉里2 (原典): `environ/win32/WatchFormUnit.*` (監視式) /
  `environ/win32/MainFormUnit.*` (コントローラ) /
  `utils/win32/PadFormUnit.*` + `PadImpl.cpp` (スクリプトエディタ) /
  `utils/win32/ConsoleFormUnit.*` + `DebugImpl.cpp` (コンソールと窓の TJS 公開)
