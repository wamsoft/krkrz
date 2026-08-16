# モーダルウィンドウ (`Window.showModal`)

`Window.showModal()` は「そのウィンドウを表示し、閉じられるまで他のウィンドウを
操作させずにブロックする」API。 WINVER (win32) では従来から実装されていたが、
SDL / generic ビルドでは長らく未実装で `TVPModalWindowIsNotSupported` を投げていた。
本ドキュメントは両者の実装方針と、実装上の落とし穴をまとめる。

## 全体像

| | WINVER (`win32/`) | SDL / generic (`generic/` + `sdl3/`) |
|---|---|---|
| ループ | `tTVPWindow::ShowModal` が `Application->HandleMessage()` を回す | `SDL3WindowForm::ShowWindowAsModal` が `SDL_PollEvent` + `AppEvent/AppIterate/Dispatch` を回す |
| 他ウィンドウの抑止 | `Application->DisableWindows()` (`EnableWindow(FALSE)`) | `SDL_SetWindowParent` + `SDL_SetWindowModal` (OS レベル) + エンジン側の入力フィルタ |
| 終了条件 | `modal_result_ != 0` | 同じ (`generic` の `in_mode_` / `modal_result_`) |
| 閉じる経路 | `tTVPWindow::Close` が `in_mode_` を見て `modal_result_` を立てる | `TTVPWindowForm::Close` / `OnCloseQueryCalled` が同様に立てる |

状態 (`in_mode_` / `modal_result_`) と close 経路の扱いは `generic/environ/WindowForm.{h,cpp}`
に共通で置き、ネストループの実装だけを `SDL3WindowForm::ShowWindowAsModal` が
override する。 ループを回せないプラットフォーム (LIB ビルド等) は基底実装のまま
`TVPModalWindowIsNotSupported` を投げる。

## SDL 実装の要点と落とし穴

### 1. ネストループは自前で組む

SDL3 のメインループはコールバック方式 (`SDL_AppEvent` / `SDL_AppIterate`) でネスト
できないため、モーダル中は `SDL_PumpEvents` + `SDL_PollEvent` でイベントを取り出し、
`app->AppEvent()` / `AppIterate()` / `SendPadEvent()` / `RequestUpdate()` / `Dispatch()`
を自分で呼ぶ。 Elements の `SDLElementsModalRunner::PumpModalLoop` と同じ方式。
`KRKRZ_USE_REPL` 時は `TVPDrainREPL()` も回し、モーダル中も REPL / Agent で
操作・観測できるようにする。 wasm では `SDL_Delay` の代わりに JSPI の
`krkrz_jspi_wait_frame()` で次フレームまで suspend する (ブラウザのイベントループを
止めないため)。

### 2. 入力の排他は二段構え

`SDL_SetWindowParent` + `SDL_SetWindowModal` が効く環境 (Windows / X11 / Wayland) では
OS 側が親ウィンドウを無効化してくれる。 効かない環境と、既にキューに積まれていた
イベントのために、`SDL3Application::AppEvent` でも
`TTVPWindowForm::GetModalWindowForm()` を見てモーダル以外のウィンドウ宛の
ユーザ入力 (キー/マウス/タッチ/ドロップ/閉じる要求) を捨てる。 リサイズや再描画
などのシステム系は通す。

### 3. ⚠ `SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE` でアプリごと終了する

SDL は「閉じる要求が来たとき、**親を持たない可視ウィンドウ**が 1 枚以下なら
`SDL_EVENT_QUIT` を送る」(`src/events/SDL_windowevents.c`)。 モーダル化のために
`SDL_SetWindowParent` を呼ぶとモーダルウィンドウはこの数から外れるので、
**モーダルの × を押しただけでアプリが終了してしまう**。 モーダル中だけ
`SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE` を `"0"` にして退避・復元する。

### 4. ウィンドウを複数作れない環境

モバイル / コンソール等では 2 枚目の `SDL_CreateWindow` 自体が失敗する。
その場合 `mWindow` が null になるので `TVPModalWindowIsNotSupported` を投げる。
呼び出し側 (ゲームスクリプト) でそもそもモーダルを出さない作りにするのが前提。

## 複数ウィンドウ (SDL) の落とし穴

モーダルに限らず「2 枚目以降の `Window`」を開くと踏む問題。 `window_multi`
デモ (`data/window_multi/`) 作成時に判明して修正した。

### a. ウィンドウ属性が全部 no-op だった

`generic/environ/WindowForm.h` の位置 / サイズ / `borderStyle` / `stayOnTop` /
`fullScreen` / `setZoom` は「モバイル・コンソール = 全画面 1 枚」を想定した
空実装のままで、SDL デスクトップでも `left`/`top`/`width`/`height` が 0、
`fullScreen` が常に true を返していた。 `SDL3WindowForm` でこれらを override
して実装した (`sdl3/environ/form.cpp`)。

- 外側サイズ ⇔ クライアントサイズの変換は `SDL_GetWindowBordersSize` の枠幅で行う
  (吉里吉里の `Window.width` は装飾込み、SDL の window size はクライアント)。
- `borderStyle` は SDL に対応概念が無いので
  `SDL_SetWindowBordered` (枠) + `SDL_SetWindowResizable` (リサイズ可否) の
  組み合わせへ写像し、値そのものは保持して返す。
- `setZoom` は generic 側 (`TTVPWindowForm::SetZoom`) に実装した。 「レイヤ
  サイズ×倍率」をウィンドウの内側サイズにし、実際の拡縮は viewport の
  フィット計算 (`CalcDestRect`) に任せる。 **WINVER は DestRect にしか倍率を
  使わないため windowed では見た目が変わらない** (差異は `TODO.md` に記録)。

### b. ⚠ サブウィンドウを閉じるとメイン画面が更新されなくなる

GL 描画デバイス (SDL のデスクトップ既定は `SDLOGLDrawDevice`) では、2 枚目の
ウィンドウも 1 枚目の GL コンテキストを共有する。 `DestroyNativeWindow` が
そのコンテキストを無条件に破棄していたため、サブウィンドウを閉じただけで
メイン画面が真っ白になっていた。

- コンテキストは**最後の参照ウィンドウが消えるときだけ**破棄する。
- `SDL_DestroyWindow` は破棄対象がカレントだった場合にコンテキストのカレントを
  外す。 **破棄後に生存ウィンドウへ `SDL_GL_MakeCurrent` し直す**こと。
  張り直さないと直後のテクスチャ更新が「カレント無し」で黙って捨てられ、
  ダーティ矩形だけ消費されて画面が古いまま止まる (今度は白ではなく
  「一部が更新されない」形で出るので気付きにくい)。
- ウィンドウ毎に別コンテキストを作る案は不可。 レイヤ更新時のテクスチャ
  アップロードは「そのときカレントの」コンテキストへ行くため、もう一方の
  ウィンドウの画面が更新されなくなる。

### c. Elements overlay がサブウィンドウへ移設されて消える

`tTVPElementsDialogManager::PaintOverlay` には「提示デバイスが切り替わったら
既存ダイアログを現行デバイスへ移設する」処理がある (GL デモで `drawDevice` を
差し替えてもパネルが出るようにするためのもの)。 これは**同じウィンドウ内での
デバイス差し替え**を想定したもので、ウィンドウが 2 枚あると毎フレーム両方が
`PaintOverlay` を呼ぶため overlay がウィンドウ間を往復し、サブウィンドウを
閉じた瞬間に `UnregisterDialogHost` でパネルごと teardown されていた。

→ 移設と `active_device` の更新は **メインウィンドウ (`TVPMainWindow`) の
device が提示したときだけ**行う。 host 未指定時の解決も、`active_device` が
無ければメインウィンドウの device を優先する。 overlay ダイアログをサブ
ウィンドウに出すことは現状サポートしない。

## 併せて修正した既存の欠落: `onClick` が SDL で発火しない

`Layer.onClick` / `Window.onClick` は、ウィンドウ側が「左ボタンの離し」で
クリックイベントを投函することで発生する。 これは win32 の `tTVPWindow::Proc`
(`WM_LBUTTONUP` → `OnMouseClick` → `tTVPOnClickInputEvent`) にしか無く、
**generic (SDL) ビルドでは `onClick` が一度も発火していなかった** (KAG の
`ButtonLayer` などクリック駆動の UI が全滅する)。

`TTVPWindowForm::OnMouseUp` に win32 と同じ意味論のクリック生成を追加した:

- 左ボタンの離しで、**押した位置** (`LastMouseDownX/Y`) を使って発火
- ダブルクリック時は発火しない (`AM_MOUSE_DBLCLK` → `OnMouseDoubleClick` で抑止)
- 発火順序も win32 に合わせ、click → mouseup の順

SDL 側は `SDL_EVENT_MOUSE_BUTTON_DOWN` の `clicks >= 2` を見て `AM_MOUSE_DBLCLK` を
先に送る (win32 の `WM_*BUTTONDBLCLK` → `OnMouseDoubleClick` + `OnMouseDown` と同じ順)。

## ⚠ モーダル中にタイマーが止まる問題 (修正済み)

`tTVPTimerThread::HandleWake()` (メインスレッド) は、以前

- `TVPTimerCS` を `Fire()` の間ずっと握り、
- `PendingEventsAvailable` (「wake 投函済み」ラッチ) を最後に false へ戻す

という作りだった。 `Fire()` の先ではタイマーハンドラが走る
(`TVPTimer` は直接、TJS の `Timer` はイベント配送経由) ので、そこから
`showModal` のネストループへ入ると **HandleWake が戻らない**。 その結果

- タイマースレッドが `TVPTimerCS` 待ちで停止する
- ラッチが true のままなので wake も二度と投函されない

となり、**モーダル表示中は TJS の `Timer` も `tTVPSystemControl` の 50ms 監視タイマー
(= イベント配送の駆動源) も完全に止まっていた**。 実案件では
`onCloseQuery` → `askYesNo` → `showModal` がまさにこの経路
(`SystemWatchTimerTimer` → `TVPDeliverAllEvents` → … → `ShowModal`) を通る。

対処 (`common/utils/TimerThread.cpp`):

- `Pending` をロック下でスナップショット (`swap`) し、**`PendingEventsAvailable` を
  Fire の前に下ろす**。 ネストループ内でも新しい wake を受け取れる。
- `Fire()` はロックの外で呼ぶ。 ペンディング数は `TakePendingCount()` で
  ロック下に取り出してゼロにしてから渡すので、Fire 中に `Trigger` が来ても
  「PendingCount == 0 → pending へ再登録」の経路に乗りティックを落とさない。
- 作業用配列をメンバ (`ProcWork`) からローカルへ移し、HandleWake の再入で
  壊れないようにした。

### 併発していた別件: wake の投函失敗でタイマーが永久停止

WINVER の `NativeEventQueueImplement::PostEvent` は `PostMessage` の戻り値を捨てて
いた。 `PostMessage` はスレッドのメッセージキューが上限 (既定 10000) に達すると
失敗するため、**「ラッチを立てたのに wake は届かない」= 以後タイマーが永久に
動かない**状態になり得た (メッセージを大量に投函する実験で再現)。
`PostEvent` を `bool` 返しにし、`tTVPTimerThread::Execute` は**投函に成功したときだけ**
ラッチを立てるようにした。 generic 側は `SendAppEvent` が失敗時にリトライキューへ
積むので従来どおり。

## Agent (自動テスト) からの操作

`TVPAgentInject*` は generic / WINVER とも、モーダル表示中はモーダルウィンドウを
注入先にする (実入力と同じくモーダル中はそれしか操作できないのが正)。

- generic: `TTVPWindowForm::GetModalWindowForm()`
- WINVER: `TVPGetModalWindowForm()` (`TVPModalWindowList` の最後尾)

また WINVER の Agent 注入は WndProc を通らないため、クリックイベントが生成されず
`Layer.onClick` が発火しない (= ボタンが反応しない) 問題があった。
`TVPAgentInjectMouseButton` の up 側で、実入力 (`WM_LBUTTONUP`) と同じ順序で
`OnMouseClick` → `OnMouseUp` を呼ぶようにした。 generic は `OnMouseUp` 自体が
クリックを生成するので追加不要。

これで `-replfile` からの `Agent.click` でモーダルダイアログを両ビルドとも検証できる。
