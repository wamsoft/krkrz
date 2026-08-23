# Window ジオメトリ仕様 (サイズ / 位置 / ズーム / ビューポート)

status: **完了 (方針確定 2026-08-16 / P1〜P5 実装 2026-08-17)**
対象: WINVER (`win32/`) と SDL・generic (`sdl3/`, `generic/`)
関連: [ModalWindow.md](ModalWindow.md) / [D3D11Migration.md](D3D11Migration.md) /
[ScreenTransfer.md](ScreenTransfer.md)

`Window` のサイズ関係 API が WINVER と SDL/generic で概念からずれていたので、
両者の基準を決めて揃えるための仕様書。**何が正か**と**現状との差分 (=やること)**
を SSOT としてここに置く。

---

## 1. 用語と基準面

| 用語 | 意味 |
|---|---|
| **inner** (クライアント / 描画領域) | ウィンドウ装飾を除いた、実際に絵が出る領域 |
| **outer** (外形) | 装飾込みのウィンドウ矩形 |
| **ゲーム画面** | `primaryLayer` のサイズ × `zoom` |
| **DestRect** | inner の中の、ゲーム画面を描く矩形 |

### 基準面は inner

**サイズ仕様の基準面は inner とする。** 理由:

- **outer は Windows でしか定義できない量**である。
  - Wayland はクライアントサイド装飾なので `SDL_GetWindowBordersSize` が失敗し、
    枠 0 = `outer == inner` に縮退する。
  - モバイル / コンソールには装飾自体が無い。
  - Windows ですら `GetWindowRect` は DWM の**不可視リサイズ枠**を含むので、
    「見た目の窓」と一致しない (実測で片側 8px 程度大きい)。
- inner は全プラットフォームで定義でき、ゲーム画面が直接対応する量である。

**例外: 常にフルスクリーンの環境**。Android / iOS / PS4 / PS5 はウィンドウを
`SDL_WINDOW_FULLSCREEN` + 1920x1080 指定で作るが、SDL が返す実サイズは
ディスプレイ解像度 (PS5 なら 3840x2160) になる。ここで基準面まで実フレーム
バッファ側に寄せると、1920x1080 で作った UI が実画面の中央に原寸で出てしまう
(Elements の overlay は基準面より大きければ縮小するが**拡大はしない**ため)。
そこでこれらの環境では **`GetSurfaceSize` / `GetInnerWidth` / `GetInnerHeight`
は生成時に要求した論理サイズを返す** (`SDL3WindowForm::mFixedSurfaceWidth/Height`)。
実サイズへの引き伸ばしは従来どおり `SDL_SetRenderLogicalPresentation` が行う。
NX はこの分岐に入らず、従来どおり実ウィンドウサイズに追従する。

したがって「**元の WIN に合わせる**」は
**「WIN の *API 意味論* に合わせる。ただし *基準面* は inner に取り直す」**
と読み替える。表示するだけの値なら outer でも許容できるが、
`setMinSize` のような**制約**を outer に置くと移植先で実効値が変わって必ず壊れる。

---

## 2. API の 3 分類

| 分類 | 定義 | 方針 |
|---|---|---|
| **A. 全 OS で定義できる** | `innerWidth/Height`・`setInnerSize`・min/max・zoom・`fullScreen`・`displayDensity` | **ここを仕様の正とする。WIN 側も合わせる** |
| **B. Windows でしか定義できない** | `width`/`height`/`setSize` (outer)、`borderStyle`、`stayOnTop`、`setMaskRegion` | WIN 挙動を維持。「枠を持たない環境では inner と同値 / no-op」と明記する |
| **C. 旧 WIN に無く SDL で追加した** | `viewport*` 一式 | 全バリアントへ昇格。既定値で旧 WIN 互換を担保する |

---

## 3. 責務の分離: zoom と viewport

サイズの話には独立した 2 軸がある。**この 2 つを混ぜない。**

| 軸 | 問い | 担当 |
|---|---|---|
| **軸A** | ゲーム画面サイズから **inner をどう決めるか** | `zoom` (+ スクリプトの `setInnerSize`) |
| **軸B** | inner とゲーム画面が **食い違ったとき、どう埋めるか** | `viewport` |

3 者を並べると、viewport は旧仕様と衝突していないことが分かる。

| | 軸A | 軸B |
|---|---|---|
| 旧 WIN (kirikiri2/Z) | **zoom が決める** (`inner == layer×zoom`) | 食い違わない前提。実質 `none` + 左上 |
| 現 WINVER | **誰も決めない** (窓は放置) | `contain` + 左上 (DPI 追従のため後付け) |
| 現 SDL | **zoom が決める** (`SetInnerSize(layer×zoom)`) | `viewport` (`contain` + 中央) |

**SDL は旧 WIN の軸A をそのまま踏襲したうえで、軸B を明示 API にしただけ**であり、
viewport は旧仕様の上位互換。ずれているのは軸A を落とした現 WINVER の側。

`setZoom` が WINVER で効かなく見えるのもこれが原因で、
`w = LayerWidth × zoom` を fit に食わせると分子分母で zoom が約分されて消える。

### 旧来の契約は「スクリプトが不変条件を維持する」だった

KAG3 の `YesNoDialog.tjs:51-59` が根拠:

```tjs
setZoom(kag.innerWidth, kag.scWidth);
...
setInnerSize(w * zoomNumer / zoomDenom, h * zoomNumer / zoomDenom);
```

`setZoom` の直後にスクリプト自身が `setInnerSize(layer×zoom)` を呼んでいる。
つまり旧契約は「`setZoom` は倍率を覚えるだけ。`inner == layer×zoom` は
スクリプトが維持する」。**この不変条件が守られている限り、fit が `contain` でも
`none` でも scale=1 になり結果は 1:1 で一致する。**

本仕様では**不変条件をエンジン側で保証する**ことにし、`setZoom` は両バリアントとも
`SetInnerSize(layer×zoom)` を行う (移植時の事故を減らすため)。KAG3 は同じ値を
再設定するだけなので無害。

---

## 4. 確定した既定値

| 項目 | 決定 | 理由 |
|---|---|---|
| **既定 fit** | `contain` | 現行両ビルドの共通挙動。不変条件が守られていれば 1:1 と一致するので旧互換に影響しない。差が出るのは「ユーザが窓を変形/最大化」「DPI 変更」「モバイルで surface 固定」= 旧 WIN では未定義だった領域のみ |
| **既定 align** | **中央** (`alignX=alignY=0.5`) | SDL 現行に合わせる。**前提**: WINVER の入力座標変換を DestRect オフセット対応にすること (§6 P1) |
| **DPI ポリシー** | **inner の物理ピクセルサイズを維持** (OS のスケール追従はしない) | ゲームはピクセル単位で作られており、モニタを跨いだだけで絵の大きさが変わらないほうが都合が良い。UI アプリの慣習 (OS 提案矩形へ追従) には従わない |

厳密な旧 WIN 挙動 (窓を広げてもゲームは原寸・左上) が要るタイトルは
`window.viewportFit = "none"` / `viewportAlignX = 0` を明示する。

### DPI ポリシーの意味

- 異 DPI モニタへ移動しても **inner の物理ピクセルサイズは変わらない**。
  したがって `innerWidth` / `innerHeight` は不変で、`onResize` も飛ばない。
- 変わるのは**枠の太さ**だけ。新しい DPI 用の枠を計算し直すので
  `width` / `height` (outer) は変化しうる。
- `primaryLayer` も DestRect も不変なので、**ゲーム画面の見え方は一切変わらない**。
- 実 DPI が要るスクリプトは `Window.displayDensity` で取得する
  (これは移動先の実 DPI を返す)。
- トレードオフ: 100% モニタで 320x240 に見えていた窓を 200% モニタへ移すと、
  物理 320x240 のまま = 見かけ半分の大きさになる。これは**意図した挙動**。

#### 実装メモ

- **WINVER** ✅: `WM_DPICHANGED` は以前 OS 提案矩形をそのまま採用していたので
  **追従してしまっていた** (実測 320→157)。提案矩形の**サイズは使わず**、現在の
  client 物理サイズを維持したまま `AdjustWindowRectExForDpi(newDpi)` で新しい
  外形を求め、位置だけ提案矩形の原点へ移す形にした。新 DPI は `GetDpiForWindow`
  がまだ旧値を返しうるので `wParam` の HIWORD を使う。
- **SDL**: ユーザがドラッグでモニタを跨いだ場合は SDL3 の Windows バックエンドが
  既にこの挙動になっている
  (`SDL_windowsevents.c` の `WM_DPICHANGED`: *"Calculate the new frame w/h such
  that the client area size is maintained"*)。
  ただし **SDL 自身の `SetWindowPos` に由来する DPI 変更 (`expected_resize`) では
  何もしない**ため、プログラムからウィンドウを別 DPI モニタへ移すと
  外形が据え置かれ client が枠差ぶんずれる (実測 320→330)。
  → ✅ krkrz 側に `TVPSDLSetWindowPositionKeepingSize()` を新設し、プログラム移動
  (`SetPosition`/`SetLeft`/`SetTop`、および `-display` の起動時配置
  `TVPMoveWindowToStartupDisplay`) はすべてこれを通す。移動前後でサイズを比べ、
  変わっていたら元の値へ戻す。

---

## 5. API 別 確定仕様

| API | 分類 | 確定仕様 | 直す側 |
|---|---|---|---|
| `innerWidth` / `innerHeight` | A | inner = 描画領域。**毎回実値を問い合わせる** | ✅ P3 (SDL の `SDL_GetWindowSize` 直読み化) |
| `setInnerSize` | A | inner を直接指定。per-monitor DPI で正しく枠を逆算 | ✅ P3 (`AdjustWindowRectExForDpi`) |
| `setMinSize` / `setMaxSize` / `min*` / `max*` | A | **inner 基準へ統一** | ✅ P3 (WINVER を inner 基準へ / SDL の getter が 0 固定だったのも修正) |
| `width` / `height` / `setSize` | B | outer 維持。枠を持たない環境では inner と同値になると明記 | 現状維持 |
| `borderStyle` 変更時の保存量 | A の帰結 | **inner 維持** (枠だけ増減する) | ✅ P3 (WINVER) |
| `displayDensity` | A | 実 DPI を返す | ✅ P3 (SDL の `return 96;` を `SDL_GetWindowDisplayScale` 由来へ) |
| `setZoom` / `zoomNumer` / `zoomDenom` | A | 軸A。`inner = layer×zoom` をエンジンが保証 | ✅ P2 (WINVER) |
| `viewportFit` / `viewportZoom` / `viewportAlign*` / `viewportOffset*` / `setViewport` | C | 全バリアントで公開。既定 `contain` + 中央 | ✅ P1 (WINVER に追加) |
| `viewportBgColor` / `setViewportWallpaper` / `clearViewportWallpaper` | C | 全バリアントで公開。反映されるのは `viewportBackgroundHost` を公開する描画デバイスのみ | ✅ P5 (`iTVPViewportBackgroundHost` + `BasicDrawDevice` の D3D11 実装) |
| `aspectLock` | C | ウィンドウ (inner) の縦横比を固定。軸A とは独立 (下記) | ✅ SDL3 / 汎用のみ。**WINVER は no-op** |
| `fullScreen` | A | ウィンドウのあるディスプレイで全画面 | 既に一致 |
| `left` / `top` / `setPos` | B 寄り | outer 原点。現状のまま | 現状維持 |
| `layerLeft` / `layerTop` / `showScrollBars` / `innerSunken` | — | `USE_OBSOLETE_FUNCTIONS` 内で**両バリアントとも未登録**。復活させない | — |

### 追加する API

- **`Window.frameWidth` / `frameHeight`** (read only) — `outer − inner`。
  枠を持たない環境では 0。outer 前提で書かれた既存コードを inner 基準へ
  機械的に移植するための橋渡し。 ✅ P3 で追加。

### 画面比率の固定 (`Window.aspectLock`)

**ゲーム画面 (primaryLayer) の比率と、画面全体の比率を分離したい**場合がある。
例えば画面全体を 16:9 に保ったまま、その中へ 640x400 (8:5) のゲーム画面を
整数倍 (ドットバイドット) または収まるまで拡大して置く構成。UI (Elements) を
16:9 の基本サイズ (1920x1080 / 1280x720) で作っていると、ウィンドウが 16:9 で
ないと UI とゲーム画面の枠がずれてしまう。

```
window.aspectLock = "16:9";   // "" を入れると解除
```

- 値は `"W:H"` 形式の文字列。`"W/H"` も読める (数字以外は区切り扱い)。
  解除は空文字列。getter は未設定時に空文字列を返す。
- **軸A (`zoom`) とも軸B (`viewport`) とも独立**した第 3 の拘束で、
  「inner の**形**を決める」もの。ゲーム画面をその枠の中へどう置くかは
  従来どおり `viewport` (fit / zoom / align / offset) が行う。
- 有効時の効果は 2 つ:
  1. **リサイズが比率へ拘束される** — SDL3 は `SDL_SetWindowAspectRatio`
     (min=max) を掛けるので、ユーザのドラッグ操作中も比率が保たれる。
     ウィンドウ生成直後にも適用される。
  2. **`setZoom` が高さを比率から決める** — 従来は `inner = layer×zoom` で
     毎回レイヤ比率 (8:5 等) へ戻ってしまっていた。
- 設定した瞬間、現在の inner も比率へ合わせられる (**幅基準**で高さを再計算)。
- 実装は `TTVPWindowForm` が比率を保持し、環境側の `ApplyAspectLock()` が
  実際の拘束を掛ける。**WINVER は API だけ揃えた no-op** (`SetAspectLock` が
  何もせず getter が 0 を返す) なので、`aspectLock` を読み戻すと空文字列になる。
  対象は SDL3 デスクトップ。

---

## 6. 現状との差分 = やること

| 段 | 内容 | 挙動変化 |
|---|---|---|
| **P1** ✅ | WINVER の DestRect 算出を `TVPCalcViewportDestRect` (`common/visual/ViewportConfig.h`、ヘッダオンリーで既に共通配置) へ差し替え。`WindowIntf.cpp` の `#ifdef __GENERIC__` を外して viewport の**配置 API** を全バリアント公開 (余白塗りは Generic 限定のまま)。**併せて WINVER の入力座標変換を DestRect オフセット対応にする** (`TransformToPrimaryLayerManager` は DestRect 原点基準の座標を期待している) | 既定を `contain` + `align(0,0)` にしたので**等価変換で挙動不変**。align を中央へ倒すのは P2 |
| **P2** ✅ | WINVER `SetZoom` を `SetInnerSize(layer×zoom)` 方式へ (= 旧 WIN / SDL と同じ)。既定 align を中央へ (両バリアントとも `tTVPViewportConfig` の既定そのまま) | `setZoom` を使うタイトルのみ (同梱では KAG3 `YesNoDialog` の 1 箇所、同値再設定なので無害)。`set_logical == false` (フルスクリーン遷移でシステムが倍率を流し込む経路) ではウィンドウに触らない |
| **P3** ✅ | 基準面の統一: SDL の `innerWidth/Height` を `SDL_GetWindowSize` 直読みへ / WINVER の min/max を inner 基準へ / WINVER の `borderStyle` 変更を inner 維持へ / `SDL3Application::GetDensity()` を実 DPI へ / `tTVPWindow::SetClientSize` を `AdjustWindowRectExForDpi` + `GetDpiForWindow` へ / `frameWidth`・`frameHeight` 追加 / **WINVER の最小ウィンドウサイズ** (下記) | 局所的 (下記 §7 の実測どおり同梱スクリプトへの影響はほぼ無い) |
| **P5** ✅ | 余白塗り (`viewportBgColor` / 壁紙) を全バリアントへ。`iTVPViewportBackgroundHost` を新設して実行時検出方式にし (下記)、`BasicDrawDevice` (D3D11) に「余白背景色でクリア + 壁紙クアッドを先に描く」実装を追加。`OGLDrawDevice` の既存実装も全バリアントへ開放 | WINVER で余白が塗れるようになった (従来は黒固定)。`iTVPDrawDevice` の vtable は不変 |
| **P4** ✅ | DPI ポリシーの統一 (inner の物理サイズ維持): WINVER の `WM_DPICHANGED` を「client 物理サイズ維持 + 位置だけ提案矩形へ」に変更 (新 DPI は `wParam` の HIWORD を使う) / SDL は `TVPSDLSetWindowPositionKeepingSize()` を新設し、プログラム移動 (`SetLeft`/`SetTop`/`SetPosition`/`-display` 起動配置) の前後で client サイズを退避・再適用 | 両方。WINVER は挙動が変わった (旧: OS 追従) |

P1〜P4 は 2026-08-17 に全て実装済み。 200%→100%→200% の往復で inner が
320x240 のまま保たれ、枠だけ 26x71 ⇔ 16x39 と変わることを両バリアントで実測確認した。

### 移行リスクの実測 (同梱スクリプト全体を grep)

- `setMinSize` / `setMaxSize` / `minWidth` 系 … **使用 0 件**
  (KAG3 / KAG3_Ham / Krkr2Compat / 全デモ)。inner 基準へ変えるのは実質無料。
- `borderStyle` … KAG3 `MainWindow` の `bsSingle`/`bsDialog`、
  `YesNoDialog` の `bsDialog` のみ。**`bsNone` への切替は無し**。
- `window.setSize` … **0 件**。`setInnerSize` は使用あり
  (KAG3 `MainWindow.tjs:338` ほか)。元々スクリプトは inner でしか考えていない。
- `setZoom` … KAG3 / KAG3_Ham の `YesNoDialog` のみ (§3 参照)。

---

## 7. 実測ログ (2026-08-16、3 画面構成)

環境: プライマリ 3840x2160 @200%、`\\.\DISPLAY2`/`3` 1920x1080 @100%。
`-nostartup` で `new Window()` を作って計測。

### 同一 DPI (プライマリのみ)

P1〜P3 前 (ずれていた状態) と P3 後 (揃えた状態) を並べる。

| 操作 | P3 前 WINVER | P3 前 SDL | **P3 後 (両方一致)** |
|---|---|---|---|
| `new Window()` 直後 | w=258 h=81 iw=232 ih=10 | w=58 h=103 **iw=0 ih=0** | WINVER 258x81/232x10、SDL 58x103/**32x32** (生成時サイズの違いのみ) |
| `setInnerSize(320,240)` | w=346 h=311 iw=320 ih=240 | 同左 | w=346 h=311 iw=320 ih=240 |
| `setSize(400,300)` | w=400 h=300 iw=374 ih=229 | 同左 | w=400 h=300 iw=374 ih=229 |
| `width = 500` | w=500 h=300 iw=474 ih=229 | 同左 | w=500 h=300 iw=474 ih=229 |
| `primaryLayer.setSize(640,480)` | 変化なし | 変化なし | 変化なし |
| `setMinSize(400,350)` 直後 | **w=400 h=350** iw=374 ih=279 (outer 基準) | w=426 h=421 iw=400 ih=350 (inner 基準) | **w=426 h=421 iw=400 ih=350** |
| → `setInnerSize(200,150)` | w=400 h=350 iw=374 ih=279 | w=426 h=421 **iw=200 ih=150** ← 実値と乖離 | **w=426 h=421 iw=400 ih=350** |
| `borderStyle = bsNone` | **w=346 h=311** iw=346 ih=311 (outer 維持) | w=320 h=240 iw=320 ih=240 (inner 維持) | **w=320 h=240 iw=320 ih=240** |
| `fullScreen = true` | 3840x2160 | 3840x2160 | 3840x2160 |
| `displayDensity` | 192 | **96** (固定値) | **192** |
| `frameWidth` x `frameHeight` | (無し) | (無し) | **26x71** |

P3 後は `new Window()` 直後 (生成サイズが 10x10 と 32x32 で違う) を除き、
**全項目が両バリアントで一致**する。

### 異 DPI モニタへの移動 (200% → 100%、プログラムからの `setPos`)

| | P3 前 WINVER | P3 前 SDL | P3 後 | P4 適用後の目標 |
|---|---|---|---|---|
| 200% 上で `setInnerSize(320,240)` | iw=320 ih=240 density=192 | iw=320 ih=240 density=96 | 両方 iw=320 ih=240 density=192 | 同左 |
| 100% モニタへ移動 | **iw=157 ih=117** (OS 追従してしまう) | iw=330 ih=272 (外形据え置き) | ✅ P4 で両方 **iw=320 ih=240 outer=336x279 frame=16x39 dpi=96** | 達成 |
| 100% モニタ上で `setInnerSize(320,240)` | **iw=330 ih=272** ← 一致しない | iw=320 ih=240 | **両方 iw=320 ih=240 frame=16x39 density=96** | 同左 |

P3 前に WINVER が一致しなかったのは `tTVPWindow::SetClientSize` が
`AdjustWindowRectEx` を使っていたため。PerMonitorV2 ではこれは
**システム DPI** で枠を計算するので、システム DPI と異なるモニタ上では
外形が誤算される (346 − 枠(96dpi)16 = 330)。フルスクリーン復帰も同じ経路を
通るのでサイズがずれていた。 → P3 で `AdjustWindowRectExForDpi` +
`GetDpiForWindow` に差し替えて解消 (`tTVPWindow::AdjustWindowRectForWindow`)。

---

### 残る差: WINVER のキャプション付きウィンドウには Windows 由来の最小幅がある

min/max を**明示的に指定したとき**は両バリアント完全に一致する (実測: 400/500 とも
inner 基準で同値)。差が出るのは**未指定 (0 = 制限なし) のとき**だけ:

| `setInnerSize(60,40)` (min/max 未指定) | 内側 | 外形 |
|---|---|---|
| WINVER (`bsSizeable`) | **232x40** | 258x111 |
| WINVER (`bsNone`) | 60x40 | 60x40 |
| SDL (`bsSizeable`) | 60x40 | 86x111 |

P3 で「未指定なら OS 既定の最小 (`SM_CXMINTRACK`) を残さない」ようにし、
`WM_GETMINMAXINFO` で `ptMinTrackSize` を 1 にしたが、**それでも 232 で止まる**。
検証したこと:

- `ptMinTrackSize` を 1 にしても、`WM_GETMINMAXINFO` を握らず OS 既定に委ねても、
  結果は同じ 232 → **`ptMinTrackSize` では下げられない Windows 側の下限**
- `borderStyle = bsNone` (キャプション無し) にすると 60x40 まで縮む
  → 下限は**キャプションを持つウィンドウに対する Windows の制約**
- min を明示指定すればその値が `SetWindowPos` にも効く (両バリアント一致)
- ウィンドウスタイルは両バリアント同一 (`0x16CF0000`)。SDL がなぜ回避できるかは未解明

実害が出るのは「内側幅が ~116 論理px 未満」のキャプション付きウィンドウだけなので、
**現状は既知の差として残す**。必要になったら SDL の回避方法を調べる。

### P5 の設計: 余白塗りは別インターフェース + 実行時検出

余白塗りの 3 メソッドは **`iTVPDrawDevice` には載せていない**。仮想関数を足すと
既存プラグインが実装した描画デバイスの vtable と食い違って壊れるため、
動画の `videoPresenterHost` / Elements の `dialogRendererHost` と同じ方式にした:

1. `common/visual/ViewportBackground.h` に **`iTVPViewportBackgroundHost`**
   (背景色 / 壁紙 / 壁紙クリアの 3 メソッド) を定義する。
2. `tTVPDrawDevice` がこれを実装し、設定値を保持する
   (`tTVPDrawDevice` は tp_stub 非公開なのでプラグイン ABI に影響しない)。
3. **余白を実際に描けるデバイスだけ**が TJS の読み取り専用プロパティ
   `viewportBackgroundHost` でポインタを公開する
   (`BasicDrawDevice` / `OGLDrawDevice` / `SDLDrawDevice` / `SDLOGLDrawDevice`)。
4. `tTJSNI_Window::UpdateContent` が `TVPQueryViewportBackgroundHost()` で
   問い合わせ、**非 0 のときだけ**余白設定を push する。非対応デバイス
   (`NullDrawDevice` / プラグイン製のカスタムデバイス等) では何も起きず、
   余白はそのデバイスの既定の塗りつぶしのままになる。

結果として **`iTVPDrawDevice` の vtable は P5 前と完全に同一**で、
プラグインの再ビルドは不要。`tp_stub.h` には
`iTVPViewportBackgroundHost` が追加されるだけ (プラグインが余白塗り対応の
デバイスを書きたいときはこれを実装して同名プロパティを生やせばよい)。

## 8. 未決 / フォローアップ


- **macOS の point / pixel**: `SDL3WindowForm::GetSurfaceSize` は
  `SDL_GetWindowSize` (= macOS では point) を使っている。Retina では
  描画解像度が半分になる可能性がある。`SDL_GetWindowSizeInPixels` との
  使い分けを別途整理する。**未検証 (macOS 実機確認が前提)**。
  umbrella の [TODO.md](../../../TODO.md) 「将来課題」に登録済 (優先度 低)。
- ~~**`Window.width` の非推奨化**~~ ✅ リファレンス (`width` / `height` /
  `setSize`) に「内側基準の API を推奨」の注記を追加済み。仕様上は残す。
- ~~**`viewportFit = "none"` の案内**~~ ✅ `viewportFit` / `setViewport` の
  リファレンスと [ビューポート](../../doc/topics/core/viewport.md) の
  トピックページに、旧来 (吉里吉里2 / 吉里吉里Z) と同じ表示にする指定
  (`setViewport("none", 1.0, 0, 0)`) を明記済み。
