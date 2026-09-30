# Window の状態 (最大化 / 最小化 / 復帰 / 元の矩形)

status: **実装済み (2026-09-26)**
対象: WINVER (`win32/`) と SDL・generic (`generic/`)
関連: [WindowGeometry.md](WindowGeometry.md)

`windowEx` プラグイン (Win32 専用) が持っていた**ウィンドウ状態**の機能を本体へ移した。
プラグインは SDL / CS 機には無いので、同じことを**本体に全プラットフォーム分**置く。

---

## 1. TJS API

| 名前 | 種別 | 意味 |
|---|---|---|
| `Window.maximized` | プロパティ (読取専用) | 最大化しているか |
| `Window.minimized` | プロパティ (読取専用) | 最小化 (アイコン化) しているか |
| `Window.maximize()` | メソッド | 最大化する |
| `Window.minimize()` | メソッド | 最小化する |
| `Window.showRestore()` | メソッド | 最大化 / 最小化から**元のサイズへ戻す** |
| `Window.getNormalRect()` | メソッド | 最大化 / 最小化して**いないとき**の外形矩形。取れなければ `void` |
| `Window.getWindowRect()` | メソッド | 外形 (装飾込み) の**画面座標**矩形 |
| `Window.getClientRect()` | メソッド | クライアント (描画領域) の**画面座標**矩形 |
| `Window.setClientRect(rect)` | メソッド | クライアントが指定の画面座標矩形になるようにウィンドウを動かす。欠けた要素は現在値 |
| `System.getMonitorInfo([nearest[, …]])` | メソッド | モニタ情報。下記 |
| `System.getDisplayMonitors([x,y,w,h])` | メソッド | 全モニタ (または矩形と重なるモニタ) の配列 |
| `System.getSystemMetrics(名前)` | メソッド | Win32 の `GetSystemMetrics`。知らない名前は `void` |

矩形はすべて `%[ x, y, w, h ]` の辞書 (windowEx プラグインと同じ形)。

### モニタ情報の形

```tjs
%[ name:"\\.\DISPLAY1", primary:1,
   monitor:%[ x:0, y:0, w:3840, h:2160 ],   // モニタ全体
   work:   %[ x:0, y:0, w:3840, h:2064 ] ]  // タスクバーを除いた作業領域
```

`getDisplayMonitors(x,y,w,h)` で範囲を指定したときは、その範囲との重なりが `intersect` に入る。

`getMonitorInfo` は引数の数で対象が変わる (windowEx と同じ)。

| 引数 | 対象 |
|---|---|
| なし | プライマリモニタ |
| `(nearest, window)` | そのウィンドウのあるモニタ |
| `(nearest, x, y)` | その点のあるモニタ |
| `(nearest, x, y, w, h)` | その矩形と重なるモニタ |

`nearest` が真なら「いちばん近いモニタ」、偽なら重ならないとき `void`。

状態の変更はメソッドで行う。`maximized = true` のような代入はできない
(最大化と復帰は「どちらへ戻すか」が要るので、対称な setter にならない)。

### `getNormalRect()` が要る理由

最大化中でも「**元に戻したときの位置とサイズ**」を知りたい場面がある
(セーブして次回起動時に復元する、フルスクリーンから戻す、など)。
`left` / `top` / `width` / `height` は**いまの**状態を返すので、最大化中は使えない。

```tjs
// 最大化していても「元のサイズ」が取れる
var r = win.getNormalRect();   // %[ x:100, y:100, w:640, h:480 ]
win.maximize();
var r2 = win.getNormalRect();  // 同じ %[ x:100, y:100, w:640, h:480 ]
```

## 2. 実装

| 層 | ファイル |
|---|---|
| TJS 公開 | `common/visual/WindowIntf.cpp` |
| `tTJSNI_Window` (委譲) | `win32/visual/WindowImpl.{h,cpp}` / `generic/visual/WindowImpl.{h,cpp}` |
| WINVER の実体 | `win32/environ/TVPWindow.{h,cpp}` (`tTVPWindow`) |
| SDL / generic の実体 | `generic/environ/WindowForm.h` (`TTVPWindowForm`、スタブ) |

### WINVER

`ShowWindow(SW_MAXIMIZE / SW_MINIMIZE / SW_RESTORE)` と `IsZoomed` / `IsIconic`。
`getNormalRect` は `GetWindowPlacement()` の `rcNormalPosition`。

⚠ **`rcNormalPosition` はワークエリア座標**なので画面座標へ直す必要がある。
タスクバーが上や左にあるとその分ずれる。`MonitorFromWindow` + `GetMonitorInfo` で
`rcWork` と `rcMonitor` の差を足している。

### SDL / generic (2026-09-26 に実装)

デスクトップの SDL3 は本物のウィンドウを持つので、`generic` のスタブを
`SDL3WindowForm` (`sdl3/environ/form.cpp`) で埋めた。

| | SDL3 での実体 |
|---|---|
| `maximized` / `minimized` | `SDL_GetWindowFlags` の `SDL_WINDOW_MAXIMIZED` / `MINIMIZED` |
| `maximize()` / `minimize()` / `showRestore()` | `SDL_MaximizeWindow` / `SDL_MinimizeWindow` / `SDL_RestoreWindow` |
| `getClientRect()` | `SDL_GetWindowPosition` + `SDL_GetWindowSize` (SDL の座標はクライアント基準) |
| `getWindowRect()` | 上に `SDL_GetWindowBordersSize` の厚みを足す |
| `setClientRect()` | `SDL_SetWindowPosition` + `SDL_SetWindowSize` |
| `getNormalRect()` | ⚠ **SDL は覚えていない**ので自前で追う。移動 / リサイズ / 復帰のたびに、通常状態なら控える |
| `disableResize` | `SDL_SetWindowResizable`。borderStyle と AND を取る (`ApplyResizable`) |
| `disableMove` | ⚠ **SDL に口が無い**。値は保持して返すだけ。枠なしなら WM がそもそも動かさない |
| `System.getMonitorInfo` / `getDisplayMonitors` | `SDL_GetDisplays` / `GetDisplayBounds` / `GetDisplayUsableBounds` / `GetDisplayName`。`tTVPApplication` の仮想関数 (`GetMonitorCount` / `GetMonitorInfoAt` / `FindMonitorForRect`) 越しに `generic/base/SystemImpl.cpp` が TJS へ出す |
| `System.getSystemMetrics` | **無い**。Win32 のメトリクス名そのものなので SDL には移さない |

⚠ モニタの `index` は `SDL_GetDisplays` の**その瞬間の並び**でしかない。抜き差しで変わるので保存しないこと。

CS 機 (常にフルスクリーン) は `tTVPApplication` の既定のまま = モニタ 0 台、
ウィンドウ状態はスタブなので、従来どおり何も起きない。

## 3. ⚠ 名前の罠

`tTVPWindow` のメソッド名を **`IsMaximized` / `IsMinimized` にしてはいけない**。
`windowsx.h` が**関数形式マクロ**を定義していて衝突する。

```c
#define IsMinimized(hwnd)  IsIconic(hwnd)
#define IsMaximized(hwnd)  IsZoomed(hwnd)
```

引数ゼロの `IsMaximized()` でも**空引数として展開される**ので、
`windowsx.h` を include している翻訳単位だけ名前が `IsZoomed` に化け、
**リンクエラー (未解決の外部シンボル) になる**。`win32/environ/TVPWindow.cpp` は
`windowsx.h` を include しているので実際に踏んだ。`GetMaximized` / `GetMinimized` にしてある。

## 4. 確認

```
PROBE あり: maximized:Integer minimized:Integer maximize:Object minimize:Object
            showRestore:Object getNormalRect:Object
PROBE 初期 maximized=0 minimized=0
PROBE getNormalRect(通常)     = 100,100 640x480
PROBE 最大化後 maximized=1
PROBE getNormalRect(最大化中) = 100,100 640x480   ← 元の矩形が残る
PROBE 復帰後 maximized=0 w=640 h=480

PROBE getWindowRect = 120,140 640x480
PROBE getClientRect = 133,198 614x409            ← 画面座標。装飾のぶん内側
PROBE setClientRect -> 1 後 client=200,220 400x300
PROBE 一部だけ指定 (%[w:320]) -> 1 後 client=200,220 320x300   ← 欠けた要素は現在値
PROBE getSystemMetrics CYCAPTION=45 CXSIZEFRAME=5 CMONITORS=1 未知キー=(void)
PROBE getMonitorInfo() name=\.\DISPLAY1 primary=1
      monitor=0,0 3840x2160 work=0,0 3840x2064   ← タスクバー 96px ぶん
PROBE getMonitorInfo(true, win) / (true, x,y) / (true, rect) … いずれも DISPLAY1
PROBE getDisplayMonitors() 件数=1
PROBE getDisplayMonitors(0,0,50,50) intersect=0,0 50x50
```

## 5. 拡張イベント (registerExEvent)

`Window.registerExEvent()` を呼ぶと、ウィンドウ枠の操作に伴うイベントが飛ぶようになる。
**呼ぶまでは飛ばない** (windowEx と同じ)。SDL / CS 版は何も起きない。

| イベント | いつ | 引数 |
|---|---|---|
| `onMove(x, y)` | 移動した (`WM_MOVE`) | 新しい位置 |
| `onMoving(rect)` | 移動中 (`WM_MOVING`) | 矩形。**書き換えると位置を拘束できる** |
| `onResizing(rect, edge)` | サイズ変更中 (`WM_SIZING`) | 矩形 + どの辺を掴んでいるか。**書き換え可** |
| `onMoveSizeBegin()` / `onMoveSizeEnd()` | 枠のドラッグ開始 / 終了 | |
| `onDPIChanged(dpiX, dpiY)` | DPI が変わった | |
| `onDisplayChanged()` | 画面解像度が変わった | |
| `onMinimize()` / `onMaximize()` | 最小化 / 最大化した | |
| `onMaximizeQuery()` | 最大化しようとしている | |

### SDL3 で飛ぶもの / 飛ばないもの (2026-09-26)

| イベント | SDL3 |
|---|---|
| `onMove(x,y)` | ✅ `SDL_EVENT_WINDOW_MOVED` |
| `onMinimize()` / `onMaximize()` | ✅ `SDL_EVENT_WINDOW_MINIMIZED` / `MAXIMIZED` |
| `onDPIChanged(x,y)` | ✅ `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`。SDL は倍率なので `96 * scale` に直して渡す |
| `onDisplayChanged()` | ✅ `WINDOW_DISPLAY_CHANGED` / `DISPLAY_ADDED` / `REMOVED` / `DESKTOP_MODE_CHANGED` |
| `onDeviceChanged(arrival)` | ✅ `SDL_EVENT_JOYSTICK_ADDED` / `REMOVED` / `GAMEPAD_ADDED` / `REMOVED` |
| `onMoving(rect)` / `onResizing(rect,edge)` | ❌ **SDL に無い**。ドラッグ中に同期で問い合わせる仕組みが無く、結果 (`RESIZED` / `MOVED`) しか来ない |
| `onMoveSizeBegin()` / `onMoveSizeEnd()` | ❌ 同上 |
| `onMaximizeQuery()` | ❌ 同上 (最大化の可否を聞かれない) |
| `onPaste()` | ❌ SDL に「貼り付け指示」のイベントが無い (`CLIPBOARD_UPDATE` は別物) |

⚠ パッドの抜き差しとディスプレイの増減は **window に紐付かない**ので、
`SDL3Application::AppEvent` で全 form へ配っている (キーボード / マウスの抜き差しと同じ扱い)。

⚠ `onMoving` / `onResizing` は**同期 (`TVP_EPT_IMMEDIATE`) で投げる**。
ハンドラが書き換えた矩形を Windows へ返す必要があるため、非同期にはできない。
書き換えられていたら `TRUE` を返して Windows に「拘束した」と伝える。

### 最大化を止める (onMaximizeQuery)

⚠ **`TVPPostEvent` は戻り値を受け取れない**ので、windowEx の「ハンドラが真を返したら
既定処理を抑制」はそのままでは移せない。**`onCloseQuery` と同じ作り**にした。
ハンドラは止めたいときだけ**親を呼び返す**。

```tjs
function onMaximizeQuery() {
	// 最大化のかわりに擬似フルスクリーンへ、という例
	goPseudoFullScreen();
	super.onMaximizeQuery(false);   // ← これで最大化そのものは止まる
}
```

呼ばなければ既定どおり最大化する (毎回 `true` で始まる)。
`Window.onMaximizeQuery(canMaximize = true)` はネイティブメソッドで、
本体側の `WM_SYSCOMMAND` / `SC_MAXIMIZE` は同期でイベントを投げたあとこの返事を読み、
`false` なら `0` を返して既定処理を握り潰す。

`Window.maximize()` は `ShowWindow(SW_MAXIMIZE)` を直接呼ぶので**この問い合わせを通らない**
(プログラムからの最大化は意図的なものとみなす)。windowEx も同じ。

## 6. システムキーのログ

`WM_SYSKEYDOWN` (ALT / F10 系) が実際に届いているかは外から見えないので、
`win32/environ/TVPWindow.cpp` で **info レベルのログ**に残すようにした。

```
(info) WM_SYSKEYDOWN vk=121 shift=0
```

ネイティブメニューを持つと Windows がメニュー起動に食ってしまい、アプリに来なく
なることがある。通常キーは数が多いので記録しない。

実測 (2026-09-26): `Window.postMessage(0x0104 /*WM_SYSKEYDOWN*/, VK_F10, 0)` で
ログが出て、TJS の `onKeyDown` まで届き、ゲーム側のポップアップメニューが開くことを確認した。
**`Agent.keyPress` は form の `OnKeyDown` 直叩きで Win32 経路を通らない**ので、
この確認には使えない (OS 合成キーか `postMessage` を使う)。

## 7. Win32 メッセージ直叩きの置き換え (2026-09-26)

windowEx の `setMessageHook`「Win32 のメッセージ番号を TJS に見せて直接拾わせる」を
やめるために、足りなかったものを本体のイベント / API として足した。

| 足したもの | 何のため | 形 |
|---|---|---|
| `Window.registerDeviceChange()` → `onDeviceChanged(arrival)` | 入力デバイスの抜き差し (プラグインで列挙しているパッドの貼り直し) | 登録制。`WM_DEVICECHANGE` の ARRIVAL / REMOVECOMPLETE / DEVNODES_CHANGED |
| `WM_CANCELMODE` → キャプチャ解放 | 横取りされて `mouseUp` が来ず掴みっぱなしになるのを防ぐ | **スクリプトから見えない**。本体が自分で解放する |
| `onPaste()` | 外のランチャ等から `WM_PASTE` で貼り付けを指示される | 無条件に投げる |
| `System.registerHotKey` を **Win32 でも動かした** | フォーカスや `System.eventDisabled` に関係なく効く脱出口 | 既存 API。SDL3 しかポンプしていなかった |
| `Window.disableMove` / `disableResize` | 枠なし表示で掴んで動かされないように | プロパティ。`SC_MOVE`/`SC_SIZE` と `WM_NCLBUTTONDOWN` を止める |
| `System.breathe()` | 長い処理中にメッセージだけ回す (ツール類が使う) | `TVPBreathe()` を TJS へ出しただけ |

⚠ **`System.registerHotKey` は WINVER では登録しても何も起きなかった** (SDL3 の
`AppEvent` からしか `TVPProcessHotKey` を呼んでいなかった)。
`TTVPWindowForm::Proc` のキー分岐の先頭で呼ぶようにした。
消費されたキーは Windows へも通常 dispatch へも渡さない。

## 8. 残り (windowEx にあって本体に無いもの)

移していない。呼び出し側は全部 `typeof` ガード付きなので、無いと**黙って機能が落ちる**。

- **Win32 固有の見た目** … `setWindowIcon` / `resetWindowIcon` / `setIconicPreview` /
  `setWindowCornerPreference` / `setOverlayBitmap`
- **IME** … `resetImeContext` (EditLayer の IME 制御)
- **小物** … `findWindowEx` / `classLongPtr` / `loadCursor` / `setCursorPos` /
  `getCursorPos`

✅ `expandEnvString` / `getAboutString` は **本体へ入れた** (2026-09-28、
`common/base/SystemIntf.cpp`)。 あわせて `readEnvValue` / `writeEnvValue` /
`urlencode` / `urldecode` も本体に入り、WINVER / generic の両方で使える。

⚠ **ウィンドウアイコン** (`setWindowIcon` 相当) は本体には無いままだが、
`DpiIcon` クラス (PackinOneWin32) が DPI に合わせた大きさで設定できる。
engine 側は `typeof global.DpiIcon` を見て使い分けている。

`setDpiAwareness` は**要らない**。exe のマニフェストが `dpiAware=True/PM` /
`PerMonitorV2` を宣言しているので、起動時に呼び直す必要が無い。
