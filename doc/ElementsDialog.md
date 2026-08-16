# Elements ベース汎用ダイアログ機構

吉里吉里Z 上で [Elements](https://github.com/wamsoft/elements) (ThorVG ベースの C++ GUI ライブラリ) を埋め込み、 JSON 定義のモーダル/非モーダルダイアログを動作させるための機構の現状記録。 設計の経緯ではなく、 実装済み構造の参照ドキュメント。

## 全体像

3 経路で同じ JSON 定義のダイアログを動かせる:

| TJS API | 呼出例 | 経路 |
|---|---|---|
| `Dialog.showJson(json)` / `showFile(path)` | dialogTest (VK_D) | 非モーダル (オーバーレイ) — `Dialog.onAction` が逐次発火、 `close()` で終了 |
| `Dialog.showModalJson(json, title, w, h)` | modalTest (VK_E) | 独立 SDL_Window + ブロッキング (`OS WINDOW_MODAL`) |
| `Dialog.showModalJson(json)` | modalOverlayTest (VK_O) | overlay + ブロッキング (ゲーム画面の上に nested ループ) |
| `Dialog.showFlow(manifest)` / `showFlowScreens(dict, entry)` | — | overlay + ブロッキング 複数画面フロー (navigator)。 `onScreen` / `onScreenLeave` 発火 |
| `Dialog.startFlow(manifest)` / `startFlowScreens(dict, entry)` | startup.tjs ジャンルメニュー | overlay + **非ブロッキング** 常駐フロー。 出しっぱなしで背景動作と併存。 `dlg.active` / `close()` で制御 |
| `Dialog.showDict(dict)` / `showModalDict(dict [, title, w, h])` | — | 上記 showJson / showModalJson の **TJS Dictionary 版** (後述「TJS Dictionary レイアウト」) |

独立 window 経路 (E) は、 SDL ビルドでは krkrz 非依存ライブラリ [`external/elements/external/elements_modal/`](../external/elements/external/elements_modal/README.md) の `run_modal` (SDL_Window/Renderer 内蔵) をそのまま呼び出す。 overlay 経路 (D / O) は krkrz の DrawDevice にぶら下がる `tTVPElementsDialogManager` がライブラリの `overlay_session` を駆動する。

**WINVER (x64-windows-win)**: 独立ウィンドウ modal (`showModalJson(json, title, w, h)`) も
**専用 Win32 ウィンドウ + `overlay_session` + GDI `StretchDIBits` で自前実装** する
(`common/visual/elements/WinElementsModalRunner.cpp` の `TVPRunElementsModalWindow`)。
SDL の `run_modal` に相当する処理を、 独立 `WNDCLASS` (`TVPElementsModalWindow`) を登録して
親ゲームウィンドウを `EnableWindow(FALSE)` で無効化 → `overlay_session::start` →
nested Win32 pump (`PeekMessage`/`TranslateMessage`/`DispatchMessage`、 入力は
`win32_input.h` の VK→cycfi 変換で `on_mouse_*`/`on_key_*`/`on_text_input` へ、 描画は
`render_to_buffer`→`StretchDIBits`) → `session.finished()` で結果取得 → 親を復帰、 という流れ。
per-monitor DPI (`GetDpiForWindow`) に追従して物理密度でレンダする。 overlay-modal / flow の
nested ループはゲームウィンドウ上の overlay を Win32 メッセージ pump で回し、 描画は
`VSyncTimingThread` が `Show()` を毎フレーム駆動する
(`win32/visual/D3D11DialogRenderer.cpp`)。 非モーダル show* / 独立ウィンドウ modal /
overlay-modal / navigator フロー / テキスト入力 (WM_CHAR / サロゲート) はすべて WINVER で動作する。

## DrawDevice との接続

```
   Window(SDL/Win32) ──イベント──> tTVPDrawDevice 入力ハンドラ
                                         │
                                         ▼
                              ┌──────────────────────────┐
                              │ if (DialogMgr.IsActive) │
                              │   DialogMgr.Forward      │
                              │   return;                │
                              └────┬─────────────────────┘
                                   │ (非アクティブ時は LayerManager 経路)
                                   ▼
                              LayerManager::NotifyMouseDown

   DrawDevice::Show() 末尾:
                              ┌──────────────────────────┐
                              │ DialogMgr.PaintOverlay(this) │ ← 各 DrawDevice 派生 1 行
                              └────┬─────────────────────┘
                                   │
                                   ▼
                       ┌─ iTVPDialogRenderer 実装 ─┐
                       │  SDL3:  SDL_UpdateTexture │
                       │  OGL:   krkrz GLTexture   │
                       │  Win:   D3D11 DYNAMIC tex │
                       └────────────────────────────┘
```

入力インターセプトと PaintOverlay の挿入位置は 1 箇所ずつで完結 (`tTVPDrawDevice` 基底 + 派生 1 行追加)。

## 中核クラス

### `tTVPElementsDialogManager` (`common/visual/elements/`)

- シングルトンだが **複数インスタンス同時表示に対応** (R6)。 内部は `overlay_session`
  を束ねた **z-order 付きインスタンスリスト** (`std::vector<Instance>`、 先頭=最背面 /
  末尾=最前面)。 各 `Instance` は session / フロー状態 / handler / host device / 配置 /
  描画レイヤ (renderer テクスチャキー) / **`modal` フラグ** を個別に持つ。
- `ShowFromJsonString(json, handler, hostDevice, modal=true)` /
  `StartFlowFromManifest(..., modal=false)` 等でインスタンスを **追加** する
  (既存を弾かない)。 同一 `handler` が既にアクティブな場合のみ false。
- `modal` フラグ: `true` = 下のインスタンス / ゲームへ入力を通さず全入力を独占。
  `false` = 非モーダル (ヒットしない入力は下 / ゲームへ素通し、 複数並存の常駐 UI 向け)。
  `showJson` / `showModal*` / `showFlow`(ブロッキング) は `modal=true`、
  `startFlow` / `startFlowScreens` は `modal=false`。
- 状態 query: `IsModalActive()` = 何か 1 つでもアクティブ (入力インターセプトのゲート)。
  `HasModalInstance()` = `modal=true` が 1 つでもアクティブ (ウィンドウクローズ抑止用)。
  `IsHandlerActive(handler)` = 指定 handler のインスタンスがアクティブか。
- `Close()` = 最前面を閉じる。 `Close(handler)` = 指定 handler のインスタンスを閉じる
  (他人を巻き込まない)。 `ForceClose()` = 全インスタンス即破棄。
- `ActiveHandler()` = 最前面アクティブの `iTVPDialogEventHandler*`。
- `TakeLastModalResult(handler, action, values)` で、 その handler のインスタンスが
  auto-finish した結果を取得 (`close_on_click` / ブロッキング pump 用、 handler ごと)。
- `RegisterDialogHost(device, host)` / `UnregisterDialogHost(device)` で DrawDevice ごとの
  描画アダプタ提供口 (`iTVPDialogRendererHost`) を登録する (renderer は DrawDevice が所有し、
  manager は host 経由で解決)。 unregister はその device のインスタンスを teardown する。
  詳細は後述「`iTVPDialogRenderer` と提供口」。

### `iTVPDialogEventHandler`

```cpp
class iTVPDialogEventHandler {
public:
    virtual ~iTVPDialogEventHandler() = default;
    // state widget の値変化 (payload に値)、 button click (payload = void) で発火
    virtual void OnAction(const ttstr& id, const tTJSVariant& payload) = 0;
    // フロー画面遷移通知 (既定 no-op)
    virtual void OnScreenEnter(const ttstr& name) {}
    virtual void OnScreenLeave(const ttstr& name, const ttstr& action) {}
    // teardown 完了通知 (既定 no-op)。 action は close_on_click / Esc で閉じた
    // button id (Close() / ForceClose 等の外部要因は空)。 show 失敗では来ない。
    virtual void OnClosed(const ttstr& action) {}
};
```

TJS の `Dialog` クラスはこれを実装し、 TJS の `onAction` / `onScreen` /
`onScreenLeave` / `onClose` を `TVPPostEvent` 経由で発火する。

### 変数 store への書込 (`SetVar` / `Dialog.setVar`)

elements_modal の VariableStore (`vars` / `text_var`) へホスト側から書き込む
経路。 `tTVPElementsDialogManager::SetVar(handler, name, value)` が
`overlay_session::set_var` を呼び、 同名を `"text_var"` で subscribe している
label が次フレームで自動更新される。 TJS からは `dlg.setVar(name, value)`。
ソフトウェアキーボードの入力文字列表示のような「ホスト状態 → label」の
動的反映に使う (Phase 7d 値 API の最小先行版)。

`text_var` 以外の変数連動フィールドも同じ setVar 経路で駆動できる (詳細は
elements_modal README「変数 store」節): `text_list` / `rect_list` +
`index_var` (10 進 index 文字列で表示エントリ切替)、 `value_var`
(slider / gauge、 `"0.75"` 形式)、 `at_var` (canvas 子の配置 rect、
`"x,y"` / `"x,y,w,h"`)。 picker 系の `index_var` は**双方向** (選択変更で変数へ
書く + setVar で picker 表示が quiet 追従) で、 text_list / rect_list と同名に
すると選択連動 UI が JSON だけで組める。 さらに `enabled_var` (picker 選択肢の
有効/無効 mask、 `'0'`/`'1'` 文字列)、 `selected_var`+`selected_value`
(atlas_choice / radio_button のラジオグループ変数、 双方向) も setVar 駆動。

### モーダルへの初期変数注入 (`showModalFile(path, %[vars])`)

overlay モーダル (`showModalJson` / `showModalFile` / `showModalDict` の引数
1 個形式) は表示中呼出側 TJS がブロックするため、 setVar での初期値注入が
できない。 第 2 引数に **Dictionary** を渡すと build 直後・pump 前に
変数 store へ流し込まれ、 `index_var` / `enabled_var` / `selected_var` 等の
subscribe 済 widget が反映する (静的 JSON への動的初期値注入):

```tjs
var r = dlg.showModalFile("ui/launcher.jsonc",
    %[ "machine" => "2", "machine_mask" => "10101011" ]);
```

モーダル中も onAction は同期で届くので、 picker の現在 index 等は onAction で
追跡し、 close 後 (戻り値の action) と組み合わせて確定処理を行う。

### フォント / pad アイコンのセットアップ (static)

- `Dialog.registerFont(family, path[, weight[, slant[, stretch]]])` /
  `Dialog.registerFontDir(dir)` — storage パス (XP3 内可) からフォント登録。
- `Dialog.defaultFontFamily = "Open Sans, Roboto, Noto Sans JP, ..."` —
  theme 全スロットの family 列を明示。 **明示設定後は EnsureRuntimeInitialized
  の自動並び (登録済み family から生成) に上書きされない**。 自動並びは
  Latin → CJK → Emoji の順 (Emoji 系が primary になると英数の字間が崩れるため
  必ず末尾)。
- **アイコンフォント `elements_basic.ttf`** (fontello 生成、 elements 同梱) —
  check_box の ✓ / selection_menu の ▼ 等のアイコングリフ (`draw_icon`) 用。
  `resource/` に同梱し起動時に自動登録される。 theme.icon_font の参照名
  "elements_basic" と一致させるため名前加工せず登録し、 本文フォントの
  fallback 連結 (自動並び) には混ぜない。 **これが無いとチェックマーク等の
  アイコンだけ描画されない** (枠は出るが ✓ が出ない) ので注意。
- `Dialog.setPadIconBase(dir)` — pad_icon (Kenney input prompts) のベース
  ディレクトリ (storage パス、 配下に xbox/ps/switch/keyboard + vector/*.svg)。
  未設定だと pad_icon は灰色プレースホルダになる。
- `Dialog.setPadTheme(name)` — "xbox"/"ps"/"switch"/"keyboard"/"none"。
  画面 JSON の top-level `pad_theme` があればそちらが優先。

### 実行時画像の注入 (`registerImage` / `image` ウィジェット)

セーブサムネイルのように**実行時に変わる画像**を Elements ウィジェットへ渡す
仕組み。 静的な atlas とは別に、 名前→画像バイトの実行時ストアを持つ。

- `Dialog.registerImage(name, path)` — storage パスのファイルを読み `name` で
  登録。 jsonc の `image` ウィジェットからは `"image": "mem://<name>"` で参照。
  戻り値 = 成否。 `Dialog.unregisterImage(name)` / `Dialog.clearImages()`。
- jsonc: `{ "type": "image", "image": "mem://save0", "at": [x,y,w,h] }` で
  bounds にアスペクト維持 fit 描画 (elements_modal README 参照)。
- pixmap は画面 build 時に一度読むので、 **再登録 → 画面を開き直す**と更新。
  登録前に build すると空表示なので、 画面 push の前に registerImage する。
- ⚠ **Elements の画像デコーダ (ThorVG/stb) は krkrz の BMP を読めない**
  (stb が "bad offset" で拒否)。 セーブサムネイルは BMP 保存 (saveThumbnail)
  なので、 **krkrz Layer に loadImages → saveLayerImage で PNG 化 → その PNG を
  registerImage** する。 PNG/JPEG/WEBP は ThorVG が直接デコードする。

### フォーカスリング (static、 `Dialog.focusRing`)

フォーカス中の要素に elements が描く汎用の枠 (青い角丸)。 既定 `true`。

```tjs
Dialog.focusRing = false;    // アプリ全体で消す
```

**画面単位ではなくアプリ全体設定** (グローバルテーマの
`focus_ring_enabled` = `elements_modal::set_focus_ring_enabled`)。
button / slider / dial / thumbwheel の枠がまとめて消える。 状態別の絵
(通常 / オーバー / 押し下げ / 無効) を素材として持つ画像 UI では、 枠が絵に
重なって邪魔になるので切る。 **フォーカス自体は生きている**ので、
キー/パッドのナビゲーションと `hilite` frame への切替は従来どおり動く。

> クラス内から触るときは `global.Dialog.focusRing`。 `Dialog` を継承した
> クラスのメソッド内で素の `Dialog` と書くと親クラス参照になり、 static
> プロパティへの代入が「メンバが見つかりません」になる。

### 描画密度 (static、 `Dialog.renderScale`)

overlay の ThorVG ラスタライズ密度を切り替える。 表示中の画面にも次フレーム
から反映されるので、 品質/負荷の比較にも使える。

- `0` (既定) = **auto**: 最終 present サイズで直接ラスタライズする。 authored
  サイズが surface より大きい画面 (1920x1080 authored → 1280x720 surface 等)
  は縮小率ぶん小さい buffer で描くため、 CPU ラスタ / テクスチャ転送コストが
  最小になる。
- `>0` = authored 論理サイズ × この倍率で描き、 present 時に GPU 拡縮する
  (`1.0` = 原寸レンダ→拡縮表示、 `2.0` = 旧 supersampling 相当)。

なお oversized present (縮小表示) 中のマウス座標は manager が縮小率と
センタリングの逆変換をかけて dialog 論理座標へ戻すため、 どのモードでも
マウス操作は authored 座標系の hit-test に正しく届く。

ゲーム側セットアップ例:

```tjs
ElementsDialog.registerFontDir("ui/resources/fonts");
ElementsDialog.setPadIconBase("ui/resources/kenny_input_prompts");
ElementsDialog.setPadTheme("xbox");
ElementsDialog.defaultFontFamily =
    "Open Sans, Roboto, Noto Sans JP, Noto Sans TC, Noto Sans SC, Noto Emoji";
```

### 再ラスタライズ抑止 (static、 `Dialog.renderCache` / `Dialog.renderCount`)

overlay は従来、 アクティブな全パネルを**毎フレーム** ThorVG (CPU) で再ラスタ
ライズしてテクスチャへ再アップロードしていた (720p で約 92 万 px/パネル)。
`renderCache = true` (既定) では「update (状態更新) と rasterize (描画) の分離」
により、 変化の無いフレームはラスタライズ + 全クリア + アップロードを丸ごと
省略し、 レンダラ (SDL / OGL / D3D11) が layer 単位で保持している前回テクスチャ
を同じ位置に提示するだけになる。 アイドル中の CPU 負荷が大幅に下がる
(Switch 等の非力な CPU 向け)。

- ダーティ (再描画) になる契機: 入力イベントの転送 (マウス移動含む) /
  focus・hover の変化 / パーツ演出 (`animate`) の再生中 / `setVar` ・言語切替の
  実変化 (同値書込は無視) / view 内部の refresh 要求 (テキスト欄キャレットの
  点滅等) / registerImage による mem:// 画像差替 / 画面遷移エフェクト混色中 /
  ウィンドウリサイズ・デバイス切替・`renderScale` 変更 (描画条件の不一致)。
- 状態更新 (変数 poll / パーツ演出 tick / 退場演出の完了検出 / 遅延 focus 適用 /
  キャレット点滅タイマ) は描画をスキップするフレームでも毎フレーム実行される
  (`overlay_session::update()`)。 挙動は従来と変わらず、 描画だけが省略される。
- `Dialog.renderCache = false` で従来どおり毎フレーム再描画 (負荷 A/B 比較・
  問題切り分け用のランタイム逃げ道)。
- `Dialog.renderCount` (読取専用) は実際にラスタライズした累計回数。 アイドル時
  に増えていなければキャッシュが効いている (検証・負荷比較用カウンタ)。

### 部分再描画 (static、 `Dialog.partialRedraw`)

`renderCache` はパネル単位の「全か無か」で、 変化のあるフレームは常に全面を
再ラスタライズしていた。 `partialRedraw = true` (既定) では、 **ダーティが矩形
で特定できる変化はその矩形だけを描き直す**。

- 矩形化される契機:
  - **テキスト欄キャレットの点滅** (view の `refresh(rect)` 経由)
  - **`setVar` / `vars_on_focus` による要素更新** — 変数の subscriber に
    「見た目が変わる要素」を持たせ、 その要素の矩形をダーティにする。
    複数要素が変わる購読 (slider+gauge) や位置が変わるもの (`at_var`) は
    特定できないので全面へフォールバック
  - **focus / hover の変化** — 変化した新旧の id を `id_map` から引いて
    両方の矩形をダーティにする (片方だけだと枠や hilite が消え残る)
  - **パーツ演出 (`animate`) の tick** — 変換 proxy (`xform_base`) が描画の
    たびに subject の未変換矩形を `xform_state` へ控えるので、 ホストは
    それと変換量から実描画矩形を**算術だけで**求められる
    (`transformed_bounds()`)。 tick の前後で求めて両方を積む (動いた元の
    場所も塗り直す必要があるため)。 **要素ツリーを走査しないのが要点** —
    走査方式も試したが +1.9ms/frame でラスタの節約を食い潰した
- 全面のままの契機: 入力転送 (マウス移動含む)、 言語切替、
  `invalidate()`、 画面遷移エフェクト混色中。
  範囲を特定できない契機は**呼び出し側で明示的に全面ダーティにする**設計
  (「不明なら全面」を既定にして正しさを担保)。
- ⚠ 要素は自分の bounds を**はみ出して描く**ことがある (レイアウトを再計算
  しないまま label の text が伸びた場合など)。 そのためダーティ矩形は
  bounds ではなく「bounds ∪ 自然サイズ (`limits().min`)」で求め、 さらに
  変数変化では**変更前と変更後の 2 回**矩形を積む (縮んだときに伸びていた
  頃の描画が消え残らないように)。
- 処理の流れ: elements 側が矩形を device px で蓄積 →
  `overlay_session::render_to_buffer_partial()` が矩形を外側 1px 膨らませて
  **その矩形だけクリア** → **`view::draw_bounds` で矩形外の要素をカリング**
  (composite / layer の子カリングが `view_bounds` を見る = shape 生成ごと省く)
  → **ThorVG `Canvas::viewport` でラスタ範囲も矩形に限定** → core 側が
  `iTVPDialogRenderer::ReleaseBufferRect` で**その矩形だけテクスチャへ転送**。
- `ReleaseBufferRect` は SDL (`SDL_UpdateTexture` の rect 版) と OGL
  (`GLTexture::UpdateTexture` の部分矩形) で実装済み。 未実装レンダラ
  (D3D11 = WINVER) は基底の既定実装が全面 `ReleaseBuffer` へフォールバック
  するので正しさは保たれる。
- 前提として **`renderCache` 有効時のみ機能する** (staging に前回フレームが
  残っていることが条件)。 buffer サイズ変化 / 遷移エフェクト混色中 /
  ダーティ矩形が面積の 3/4 以上を占める場合は全面描画へ自動フォールバック。
- `Dialog.partialRedraw = false` で常に全面再描画 (A/B 比較・切り分け用)。
  実際に部分描画できた回数は `renderStats.partials`。

実測 (Windows SDL、 ラスタ 1 回あたりの時間):

| シナリオ | 全面 | 部分 | |
|---|---|---|---|
| キャレット点滅 | 3505us | 1700us | -52% |
| HUD カウンタ (毎フレーム setVar) | 1580us | 998us | -37% |
| アニメ小 (無限 yoyo) | 2320us | 1489us | -36% |
| アニメ広域 (大パネル) | 3089us | 2188us | -29% |
| 複合 3 パネル | 1544us | 1041us | -33% |

効いているのは主に **`view::draw_bounds` による子要素カリング** (矩形外の
要素は shape 生成ごと省かれる) で、 ThorVG viewport によるラスタ範囲限定
だけでは -21% に留まった (コストの大半は shape 生成側にある)。 全面再描画
した絵とのピクセル差は無し (文字列の伸縮・hover 状態とも完全一致。
キャレットのみ AA 境界の数 px・輝度差 2)。

実測 (NX 実機 EDEV / Release、 `-benchauto`。 ラスタ 1 回あたり / CPU 占有):

| シナリオ | 導入前 | 導入後 | |
|---|---|---|---|
| HUD カウンタ (毎フレーム setVar) | 8747us / 54.5% | **5949us / 37.3%** | -32% |
| アニメ小 (無限 yoyo) | 12688us / 78.2% | **9108us / 56.4%** | -28% |
| アニメ広域 (大パネル) | 16317us / 93.9% / **54.5fps** | **11904us / 74.5% / 60.0fps** | -27% (**フレーム落ち解消**) |
| 複合 3 パネル | 8748us / 54.9% | **5935us / 37.9%** | -32% |
| 静的パネル | ラスタ 0 回 / 0.6% | 変化なし | (既にゼロコスト) |

演出の矩形化は「毎フレーム要素ツリーを走査して bounds を得る」方式を一度
試して取り下げている (+1.9ms/frame でラスタの節約を上回った上、 変換後の
実描画範囲を再現しきれず残像が出た)。 現行は **proxy が描画時に既に
持っている `ctx.bounds` を控える**方式で、 走査ゼロ・変換の適用順も
proxy と同一なので原理的に残像が出ない。 有限アニメ (移動+拡縮+回転) を
完走させた後の画素比較で、 全面再描画と**完全一致**を確認済み。

NX 実機 (EDEV / Release) では、 矩形を特定できないシナリオ
(counter / anim / multi) の数値が導入前と一致 = **回帰なし**を確認済み
(部分再描画が発動しないので当然だが、 カリング追加による副作用がない
ことの確認になる)。 NX でのキャレット部分再描画の効果測定は下記の理由で
自動化できないため、 実画面 (テキスト入力を持つゲーム画面) での確認となる。

⚠ **テキスト欄に focus が入るとソフトウェアキーボードが開く** (Switch /
Android 等。 ホストが focus に追従して出す)。 **表示しただけで focus が
入る `initial_focus` 指定は、 そうしたプラットフォームでは避けること**。
`elements_bench` の caret パネルも initial_focus を持たず、 クリックして
初めて点滅する。 自動計測の caret A/B は `-benchcaret` を明示指定した
ときだけ実行される (既定の `-benchauto` はテキスト欄に focus しない)。

### overlay 描画コストの内訳 (2026-08-13 実測、 Windows SDL)

部分再描画で「描く範囲」は絞り切ったので、 残りは範囲内を描くコストになる。
その内訳を測ると **ほぼ全てテキスト**だった:

| 内容 | ラスタ 1 回 |
|---|---|
| ラベル 20 個 | 12.3ms |
| box 20 個 (同数・同面積) | 0.23ms |

グリフ数に比例する (4 文字×20=8.7ms / 16 文字×20=15.4ms / 48 文字×20=33.7ms)
ので、 内訳は概ね **28us/グリフ + 320us/テキスト 1 本**。 これは ThorVG SW の
**グリフ輪郭ラスタライズを毎フレーム行っている**ぶんで、 整形結果 (shaping)
を使い回しても消えない。 実際に以下を試して効果が無いことを確認済み:

- 整形済み `tvg::Text` を (文字列/フォント/サイズ/spacing/locale) でキャッシュ
  して `Paint::ref()` で保持 → 変化なし
- 色/変換を「前回と同じなら設定しない」ようにして再テッセレーションを抑止
  → 変化なし

効いたのは**テキスト描画のバッチ化**のみ (テキスト 1 本ごとに ThorVG の
add/update/draw/sync/remove を回していたのをやめ、 1 フレーム 1 サイクルに
まとめた: ラベル 20 個で 15.2ms → 12.3ms)。

### テキスト run のビットマップキャッシュ (2026-08-13 導入)

上記を受けて、 **同じ内容のテキストは 1 度だけラスタライズして、 以後は
その ARGB ビットマップを貼り直す**ようにした (elements 側 `text_backend_tvg`)。
即時モードの canvas のままテキストだけ実質保持モードになる。

- キャッシュ鍵 = 文字列 / フォント / locale / サイズ / 字間 / 色 / 拡大率。
  上限 4M px (≒16MB) を超えたら古いものから捨てる
- **初めて見る文字列はキャッシュに載せず従来の輪郭描画で描く**。 毎フレーム
  変わるテキスト (HUD カウンタ等) はどうせ必ずミスするので、 載せると
  オフスクリーン描画のぶん却って遅くなるため。 2 回目に現れて初めて載せる
- 回転/スキュー中・グラデーション塗りは対象外 (従来経路のまま)
- 環境変数 `ELEMENTS_TEXTCACHE_OFF=1` で従来経路に固定できる (A/B 用)

効果 (ラスタ 1 回あたり、 Windows SDL):

| シナリオ | 導入前 | 導入後 |
|---|---|---|
| ラベル 20 個 (毎フレーム全面) | 12.3ms | **9.5ms** |
| 全面再描画 (partialRedraw=OFF) の各シナリオ | — | **-25〜35%** |
| 部分再描画併用時 (既定) | — | -0〜20% (元々描く量が少ない) |

バッチ化と合わせると 15.2ms → 9.5ms で **-38%**。

**NX 実機 (EDEV / Release / -benchauto、 ラスタ 1 回あたり)**:

| シナリオ | 導入前 | 導入後 |
|---|---|---|
| idle を毎フレーム全面ラスタ (renderCache OFF) | 29.6ms (32fps) | **19.3ms (48fps)** |
| anim 広域 | 11.9ms | **9.7ms** |
| anim 小 | 9.1ms | **8.1ms** |
| counter / 複合 | 5.9ms | 5.9〜6.0ms (横ばい) |

counter / 複合が横ばいなのは**設計どおり**で、 毎フレーム内容が変わる
テキストは (2 回目に現れないので) キャッシュに載らない。 効くのは
「同じ文言を描き続ける」 通常の UI で、 そこが最大 -35% になる。

輪郭描画との画素差は 「別バッファに描いてから合成する」 ことによる丸め差で、
拡大比較しても品質は同等 (完全一致ではない)。

⚠ 実装上の落とし穴 (elements 側を触るとき):
- `tvg::Text` の原点は**行の上端**でありベースラインではない
- canvas に add した paint は remove で所有権ごと手放されるので、 同じ
  Picture を毎フレーム貼り回せない。 キャッシュはテンプレートとして持ち、
  描画には `duplicate()` を渡す (pixmap 描画と同じ作り)
- 貼付位置は整数画素へ吸着させる (端数のままだと ThorVG が再サンプルする)

さらに削るなら **シーンの保持 (retained 化)** — 毎フレーム全 Shape を作り直す
即時モードをやめ、 変化した paint だけ更新する。 ThorVG の damage 機構
(`TVG_PARTIAL`) もこれとセットで初めて使える。

### 描画パイプラインの区間計測 (static、 `Dialog.renderStats` / `renderStatsReset()`)

overlay 描画の負荷内訳を実測するための累積カウンタ (`ElementsDialogManager` の
PaintOverlay / RenderInstance に計測点を常設。 steady_clock 数回/フレームで
オーバーヘッドは無視できる)。 `Dialog.renderStats` (読取専用) が辞書を返す:

| キー | 意味 |
|---|---|
| `frames` | PaintOverlay 呼出回数 (= 提示フレーム数。 複数 DrawDevice 時はデバイス毎に 1) |
| `updates` | `overlay_session::update()` 実行回数 (インスタンス毎・毎フレーム) |
| `rasters` | `render_to_buffer` 実行回数 (renderCount と同じ契機) |
| `partials` | うち部分再描画 (ダーティ矩形限定) だった回数 |
| `cachedPresents` | renderCache による提示のみ (ラスタ省略) の回数 |
| `presents` | PresentOverlay 呼出回数 |
| `totalUs` | PaintOverlay 全体の所要時間 (microsecond、 以下同) |
| `updateUs` | update (変数 poll / 演出 tick / dirty 判定) |
| `rasterUs` | render_to_buffer (ThorVG CPU ラスタ + 全クリア) |
| `acquireUs` | AcquireBuffer (staging 確保 / テクスチャ lock 待ち) |
| `uploadUs` | ReleaseBuffer (テクスチャ転送) |
| `presentUs` | PresentOverlay (提示) |

すべて累積値なので、 **2 回読んで差分を取り、 経過実時間との比**で
「Elements が 1 フレーム/1 秒あたりに消費した時間・割合」を出す。
`Dialog.renderStatsReset()` で 0 クリア (計測区間の開始)。

計測用ベンチ画面 = **`data/elements_bench/`** (コアデモ)。 更新パターン別の
シナリオ (静的 / キャレット点滅 / 毎フレーム setVar / アニメ小 / アニメ広域 /
複合) を数字キーで切り替え、 renderCache の A/B (R キー) と負荷内訳を
500ms ごとに表示する。 renderCache 効果の実測、 部分再描画 (ダーティ矩形化)
導入時の before/after 確認に使う。

実測知見 (NX 実機 EDEV / Release, 2026-08-13, -benchauto スイープ):
- **renderCache の効果は決定的**: idle パネル 1 枚でも cache OFF だと全面
  再ラスタ 29.6ms/回で fps 32 まで低下 (share 96.5%)。ON なら share 0.6% /
  60fps (アイドルはゼロコスト化)。旧挙動 (毎フレーム再ラスタ) の重さの
  定量確認でもある。
- **NX の支配項は raster (ThorVG CPU)**: upload は 0.1〜0.8ms/frame と軽い
  (Windows のスパイクとは逆傾向)。raster は counter 小パネル 8.9ms /
  anim 小 12.8ms / anim 広域 16.4ms (fps 54 に低下) / 複合 cache OFF 17fps。
- 毎フレーム更新系 (HUD カウンタ・アニメ) は renderCache が効かず 55〜94%
  を消費 → **部分再描画 (ダーティ矩形) はラスタ矩形限定を本命に据える**
  (発動条件成立)。

実測知見 (Windows SDL, 2026-08-12, -benchauto スイープ):
- raster (ThorVG) は 1 回 1.6〜5.2ms でパネル内容/サイズに応じて安定
  (毎フレーム再ラスタのシナリオでは定常の支配項)。
- upload が 1 回 ~15ms に張り付く現象があった → **原因判明・解消済み**
  (次節)。当時 「SDL レンダラのテクスチャ同期起因」 と推定していたが誤りで、
  実際は既定 DrawDevice (`sdlogl`) の PBO 経路だった。

### テクスチャ転送経路 (2026-08-13 に見直し・解消)

overlay のアップロードが 1 回 13〜14ms かかり、CPU も 1 コア分を食っていた。
DrawDevice ごとに経路が違うので、まず**どの経路が動いているか**を確認する
(SDL ビルドの既定は `-drawdevice=sdlogl` = OpenGL 直接)。

| DrawDevice | レンダラ | 転送 |
|---|---|---|
| `sdlogl` / `ogl` (SDL 既定) | `OGLDialogRenderer` | `glTexSubImage2D` |
| `sdl` | `SDLDialogRenderer` | `SDL_UpdateTexture` |
| WINVER (`BasicDrawDevice`) | `D3D11DialogRenderer` | `UpdateSubresource` |

**① OpenGL 経路の PBO をやめた**。 `GLTexture::UpdateTexture` は PBO を
map して書き、`glTexSubImage2D` で吸い上げる形だが、**ANGLE (GLES→D3D11) では
PBO からの転送で内部的にバッファを読み戻すため、毎フレーム書き換える overlay
では同期待ちになる** (しかもビジーウェイト)。 overlay の元データは既に CPU 側の
staging にあり PBO に書き写す意味も無いので、`GLTexture::UpdateTextureDirect`
(部分矩形は `GL_UNPACK_ROW_LENGTH` で読み飛ばす = コピー 0 回) を新設して
直接転送に切り替えた。 環境変数 `KRKRZ_DLGTEX=pbo` で旧経路に戻せる。

| 計測 (5 秒、counter / anim 広域) | PBO (旧) | 直接 (新) |
|---|---|---|
| upload 1 回 | 13.7〜14.5ms | **9〜56us** |
| プロセス CPU 時間 | 5.7〜6.3 秒 | **1.1〜1.8 秒** |

CPU **-70〜83%**。 fps は元々フレーム上限に張り付いていて変わらないが、
1 コア分を無駄に焼いていたのが無くなる。

**② WINVER (D3D11) をアップロードと present に分けた**。 旧実装は
`ReleaseBuffer` が no-op で、`PresentOverlay` が毎回 DYNAMIC テクスチャへ
`Map(WRITE_DISCARD)` + 全面 memcpy していた。 **renderCache でラスタを省略
した「変化なし」フレームでも全面転送が走っていた**ことになる。
`D3D11_USAGE_DEFAULT` + `UpdateSubresource(box)` に変え、`ReleaseBuffer` /
`ReleaseBufferRect` で転送、present は `RenderSRV` で描くだけにした。

| 計測 (静止パネル / renderCache ヒット中) | 旧 | 新 |
|---|---|---|
| 420x360: present / 1 フレーム総計 | 75us / 113us | **5us / 39us** |
| 1240x680: present / 1 フレーム総計 | 323us / 365us | **5us / 39us** |

静止 UI の常時コストが **-65〜89%**、かつ**パネルサイズ非依存**になった。
`Map(WRITE_DISCARD)` はリソース全体を捨てる契約なので部分更新ができない
(だから DEFAULT + `UpdateSubresource` にした) 一方、全面転送だけは
`WRITE_DISCARD` の方が速いため、**大パネルを毎フレーム全面ラスタする**
ケースのみ 267us → 401us と僅かに不利になる。 部分再描画が効く通常の
使い方では圧倒的に新実装が有利。

`-drawdevice=sdl` の `SDL_UpdateTexture` 経路は元から 65〜214us で問題なし。

**③ ゲーム本画面は転送サイズで経路を選ぶようにした** (2026-08-13)。

一度 「本画面は PBO と直接転送で実測差が無い」 と結論したが **それは誤り**で、
**フレーム上限を外した状態 (137fps) で測っていたため待ちがフレーム全体に
分散して経路差が消えていた**。 通常どおりフレーム上限のある状態で測り直すと
はっきり差が出る。

NX 実機実測 (`data/upload_bench`、 60fps 上限下、 転送が実時間に占める割合):

| 更新の形 | PBO | 直接転送 | **既定 (自動判定)** |
|---|---|---|---|
| dense (全画面 1 矩形 = 3.5MB) | 11.9% | 16.9% | **9.2%** |
| sparse (小矩形 40 個 = 各 9KB) | **29.7%** | 5.9% | **5.8%** |

**形によって速い経路が逆転する**。 固定コストを逆算すると
**PBO = 121us/回 + 2.5GB/s、 直接転送 = 16us/回 + 1.25GB/s** で、
交差点は約 **256KB**。 そこで `GLTexture::UsePBOForUpload(bytes)` が
**転送バイト数で選ぶ**ようにした (256KB 未満は直接、 以上は PBO)。

sparse = **立ち絵 + UI が散在して個別更新される実案件の形**。 ここが
旧既定 (常に PBO) では 29.7% を食っていたのが 5.8% になる (**-80%**)。
dense も 11.9% → 9.2% で悪化しない。

**ただし ANGLE (Windows の GLES→D3D11) はサイズに関係なく PBO が致命的に
遅い**ため (全画面 1 枚で 10〜13ms、 実時間の 60〜80%)、 `glGetString(GL_RENDERER)`
に "ANGLE" を含む場合は常に直接転送にしている。

overlay 側も同じ判定に統一した (部分再描画の矩形は小さいのでほぼ直接転送)。
NX の `elements_bench` で回帰が無いことを確認済み (全シナリオ 60fps 維持)。

参考: 同じ 3.5MB の全面転送で **OpenGL (ANGLE) 1.1ms 対 D3D11
`UpdateSubresource` 0.29ms 対 `SDL_UpdateTexture` 0.32ms**。 ANGLE の GLES
テクスチャ転送はネイティブ D3D11 の 3〜4 倍遅い。

⚠ **Windows の計測値は試行ごとに数倍振れる** (デスクトップコンポジタ等との
競合と思われる)。 経路の判断は **NX 実機の値で行うこと** (NX は誤差 1% 以内で
再現する)。

### 転送コストの計測画面 (`data/upload_bench`)

上記を測るための計測画面。 更新の形 (dense / sparse) × 転送経路
(PBO / 直接) を切り替えて `System.renderStats` の値を表示する。

- 操作: `1` = dense / `2` = sparse / `P` = 経路切替 / `C` = 計測リセット
- 自動: `krkrz64.exe <data> -uploadauto` で全組合せを無操作計測し
  `@upload ...` 行をログへ出して終了 (NX は `nxctl upload-inst`)
- 経路の実行時切替は **`System.texUploadUsePBO`** (void = 既定 = サイズによる
  自動判定 / true = PBO 強制 / false = 直接転送 強制)。 環境変数を渡せない
  実機用。 環境変数 `KRKRZ_GLTEXUP=pbo|direct` も同じ意味
- 自動計測は 3 経路 (既定 / PBO / 直接) × 2 形 = 6 組を回すので、
  **自動判定が形ごとに速い方を選べているか**がそのまま確認できる

⚠ **フレーム上限のある状態で測ること**。 無制限 fps で測ると待ちが分散して
経路差が消える (実際にそれで一度誤った結論を出した)。

### 画面転送コストの計測口 (`System.renderStats` / `System.renderStatsReset()`)

上の調査で使った計測を常設したもの。 **ビルドオプション不要で常に有効**
(1 フレームに数回のカウンタ加算のみ)。 描画デバイスに依らず、
OpenGL / SDLDrawDevice / WINVER の 3 経路すべてで同じキーが埋まる。

| キー | 意味 |
|---|---|
| `texUploadUs` | 転送呼び出しの累計時間 (us) |
| `texUploads` | 転送回数 (ダーティ矩形単位) |
| `texUploadBytes` | 転送した累計バイト数 |
| `frames` | 転送フェーズの実行回数 (≒ 画面更新フレーム数) |

累積値なので 2 回読んで差分を取る。 overlay 側の内訳は `Dialog.renderStats`。

⚠ **fps だけ見ても転送の詰まりは分からない**。 フレーム上限に張り付いていると
転送が 14ms かかっていても fps は変わらない (今回の PBO 問題がまさにそれで、
1 コアを無駄に焼いていた)。 `renderStats` の差分と**プロセスの CPU 時間**
(`Get-Process ... TotalProcessorTime`) を併せて見ること。

### SDL 拡張プラグイン向け C ABI サービス (`tp_dialog_service.h`)

静的リンクプラグイン (tp_stub ベース) から overlay ダイアログ機構を使うための
C インタフェース。 tp_stub は本体と別系統の TJS 型定義を持つため本体ヘッダを
include できない — そこで UTF-8 `char*` + 関数ポインタ + opaque handle に
落とした ABI を `common/visual/elements/tp_dialog_service.h` に切ってある。

```c
const TVPSDLDialogAPI_v1* api = TVPGetSDLDialogAPI(TVP_SDL_DIALOG_API_VERSION);
// api->show_overlay_json(json, modal, grab_focus, on_action, on_close, user)
// api->close(handle) / api->is_active(handle) / api->set_var(handle, name, value)
```

実装は `DialogPluginService.cpp` (KRKRZ_USE_ELEMENTS ゲート)。 handle は
`on_close` が返るまで有効で、 死んだ handle は生存レジストリで弾かれる。
将来の拡張 (SDL_Renderer への描画 hook 等) は version を上げた別 struct を
同じ entry point から返す。 利用例: krkrz_nx の `plugins/softkey`
(Elements ベース英数字ソフトウェアキーボード)。

### `iTVPDialogRenderer` (DrawDevice 適合) と提供口 `iTVPDialogRendererHost`

全 DrawDevice で実装済み (WINVER も対応):

| 実装 | 場所 | 方式 |
|---|---|---|
| `tTVPSDLDialogRenderer` | `sdl3/visual/SDLDialogRenderer.cpp` | `SDL_Texture (STREAMING, ARGB8888)` + `SDL_UpdateTexture` + `SDL_RenderTexture` |
| `tTVPOGLDialogRenderer` | `common/visual/opengl/OGLDialogRenderer.cpp` | krkrz `GLTexture` (PBO 経由 UpdateTexture) + `GLTextureDrawer` straight-alpha 合成。SDL/WIN 両 OGL DrawDevice 共用 |
| `tTVPD3D11DialogRenderer` | `win32/visual/D3D11DialogRenderer.cpp` | **WINVER (BasicDrawDevice/D3D11)**。CPU staging → DYNAMIC tex (`B8G8R8A8`) → クアッド α 合成。overlay 動画と同じ `tTVPVideoPresenterD3D` ブリッタを layer ごとに流用。Elements の overlay バッファ `0xAARRGGBB` はメモリ上 BGRA で `DXGI_FORMAT_B8G8R8A8_UNORM` と一致するため swizzle 不要 |

`AcquireBuffer(layer, w, h)` / `ReleaseBuffer(layer)` / `PresentOverlay(layer, x, y, w, h)` /
`ReleaseLayer(layer)` + `GetSurfaceSize` / `GetDestRect` の 6 関数。 **`layer` は overlay
インスタンスを一意に識別する不透明キー** (manager は `Instance` ポインタを渡す)。 複数の
非モーダル UI を同一フレーム内で重ねて present できるよう、 テクスチャ + ステージングは
layer ごとに `std::map` で保持する。 `SDL_RenderTexture` 等はテクスチャを **参照キューイング**
するため、 単一テクスチャを使い回すと先に present したレイヤ内容が壊れる — layer ごとに
別テクスチャが必須。 `PaintOverlay` は z-order 奥→手前の順に layer ごとに present し、
インスタンス close 時に `ReleaseLayer` する。

**提供口の汎用化 (`iTVPDialogRendererHost`)**: renderer は **DrawDevice が所有** し、
`iTVPDialogRendererHost::GetDialogRenderer()` 経由で貸し出す。 manager は具象 renderer 型を
知らず host 経由で解決する (overlay 動画の `iTVPVideoPresenterHost` と同じ設計)。 各 DrawDevice
は ctor / `InitContext` で `RegisterDialogHost(this, this)`、 dtor / `DoneContext` で
`UnregisterDialogHost(this)`。 さらに TJS 読取専用プロパティ **`dialogRendererHost`** で自身の
host ポインタを `tjs_int64` 公開する (`videoPresenterHost` と同規約)。 `iTVPDialogRenderer` /
`iTVPDialogRendererHost` と登録 free 関数 `TVPRegisterDialogHost` / `TVPUnregisterDialogHost` は
**tp_stub 公開済み** (`DialogRenderer.h` の抽出マーカー)。 IF は完全に PF 非依存の POD なので
D3D11 等の型を tp_stub に持ち込まない。 → **プラグイン / 差し替え DrawDevice も、
`iTVPDialogRendererHost` を実装 + renderer を所有 + `TVPRegisterDialogHost` で登録すれば
overlay ダイアログ描画に参加できる**。

## 入力ルーティング (krkrz overlay)

DrawDevice / Window の入力ハンドラは `TVP_DIALOG_INTERCEPT` マクロで、 何か 1 つでも
インスタンスがアクティブ (`IsModalActive()`) なら入力を `Forward*` へ流す。 **`Forward*` は
「消費したか (bool)」を返し、 消費した場合だけマクロが `return` してゲーム入力処理を止める。**
これにより非モーダル UI で、 ダイアログに当たらない入力をゲームへ素通しできる。

配送の優先順位は一列に整理されている:

```
1. モーダルインスタンス     … 全消費 (最優先。 下にもゲームにも通さない)
2. ホストホットキー         … registerHotKey 登録キーはダイアログへ渡さず
                              通常経路 (Window.onKeyDown / onMouseDown) へ直行
3. フォーカスパネル         … キー/パッドを送り、 未処理分のみ素通し
4. ゲーム / レイヤ          … 未消費の落ち先
```

複数インスタンス時の配送ルール (最前面 = z-order 末尾から走査):

| 入力 | ルール |
|---|---|
| マウス down/up/wheel | 最前面から走査。 `modal` インスタンスに当たればそこで消費 (下へ通さない)。 非モーダルは描画矩形 (`last_rect`) にヒットしたら消費。 どれにも当たらなければ非消費 → ゲームへ素通し |
| マウス move | 同様にヒット先へ hover を送る。 以前カーソルが居て今は外れたインスタンスには `on_mouse_leave` |
| キー / パッド / テキスト | **キーボードフォーカス保持インスタンス**へ送る (下記)。 マウスのヒットテストとは別概念 |

### キーボードフォーカス (focus 保持 + grabFocus + handled 素通し)

キーボード/パッドは「どこへ送るか」を z-order とは別に決める必要があるため、 専用の
ルールを持つ:

- **フォーカス = フォーカスを持つ最前面アクティブインスタンス** (`TopmostKeyboardFocus`)。
  候補は `modal` または `wants_focus` のもの。 後から開いた focus-grab ダイアログが
  自然にフォーカスを持ち、 閉じると直前の focus-grab へ戻る (スタック不要)。 誰も
  持たなければキーは**ゲームへ素通し** (非消費)。
- **modal**: 常にフォーカスを強制取得し、 未処理キーも含め**全消費** (下にもゲームにも
  通さない)。
- **非モーダル**: フォーカス保持時のみキーを受け、 **`overlay_session` が実際に処理した
  キーだけ消費** (handled pass-through)。 ダイアログが使わないキー (ゲームのホットキー等)
  はゲームへ通る。 `overlay_session::on_key_down/up/on_pad_button` は handled を返す
  (elements submodule、 `view::key()` の戻り値)。
- **`grabFocus` 引数** (`ShowFromJson*` / `StartFlow*` / TJS `startFlow`/`startFlowScreens`、
  既定 `true`): `false` で「フォーカスを取らない常駐 HUD」になり、 キーを一切奪わない
  (操作はマウス、 またはフォーカスを持つ別ダイアログ経由)。 `modal=true` は常に強制取得。
- **`modal` 引数** (TJS `showJson`/`showFile`/`showDict` の第 3 引数、 省略時は
  後方互換で `grabFocus` に追従): `showJson(json, true, false)` =
  **「非モーダル + フォーカスあり」の中間状態**。 キー/パッドはダイアログへ届き
  (パッドで slider / picker を操作できる)、 未処理分はホストへ素通しする。
  シェル操作に必須のキーはホストホットキー (下記) と組み合わせて確保する。
  用途 3 態: モーダルダイアログ (`showJson(json)`) / 操作パネル
  (`showJson(json, true, false)`) / 表示専用 HUD (`showJson(json, false)`)。
- **テキスト入力フォールバック** (2026-08-10): `grabFocus=false` のパネルでも、
  クリック等で **input_box 等のテキスト入力ウィジェットが focus されている間**
  (`focus_consumes_text()`) は、 そのインスタンスへキー/テキスト (`Agent.text` /
  IME 含む) が届く。 未処理キーは従来どおりゲームへ素通しなので、 テキスト欄から
  focus が外れれば (別ウィジェットのクリック等) ゲームのホットキーも復帰する。
  「クリックでキャレットが出るのに文字が届かない」ギャップの解消 = 見えている
  キャレットと入力の行き先が常に一致する。

例: 左上メニュー (`grabFocus=true`) + 右下 HUD パネル (`grabFocus=false`) を同時表示。
メニューにフォーカスがあるが、 メニューが使わない `P` キーは passthrough でゲームに届き、
ゲーム側ホットキーが生きる (`data/startup.tjs` の openMenu / togglePanel)。

`modal=true` なインスタンスが最前面なら、 矩形外のクリックも含めて全入力を独占するので、
従来のモーダルダイアログと同じ「下を触れない」挙動になる。 非モーダルの常駐メニュー +
背景のゲーム動作を併存させたいときは `startFlow` (= `modal=false`) を使う。

### ホストホットキー (Dialog.registerHotKey)

「ダイアログにフォーカスを渡しつつ、 特定のキーだけは必ずホストが取る」ための
バイパス機構。 登録したキーは `Forward*` の先頭で判定され、 **ダイアログへ渡らず
非消費 (false) で返る** = そのまま通常のゲーム入力経路 (`Window.onKeyDown` /
`onMouseDown` 等) へ流れる。 専用イベントは無い (バイパス方式)。

```tjs
Dialog.registerHotKey(key, shift = 0, duringTextInput = false);
Dialog.unregisterHotKey(key, shift = 0);
Dialog.clearHotKeys();
```

- `key` は VK コード。 **キー / パッドボタン (VK_PAD*) / マウスボタン
  (VK_LBUTTON / VK_RBUTTON / VK_MBUTTON / VK_XBUTTON1 / VK_XBUTTON2) を同じ
  空間で受ける**。 右クリック=戻る等のマウスホットキーもここで確保できる
  (全画面透過の非モーダル overlay が右クリックを常に拾ってしまう問題の解)。
- `shift` は `ssShift | ssAlt | ssCtrl` の組合せ。 down は完全一致、 **up は
  key のみ一致**で対でバイパスする (押下中の修飾キー変化で up がパネルへ
  漏れない)。
- `duringTextInput=false` (既定) は **テキスト入力ウィジェット focus 中
  (`focus_consumes_text()`) は抑止** = ESC/BS 等を入力欄から奪わない。
  「入力中も必ず効かせたい」キーだけ `true` で登録する。
- **モーダル表示中は無効** (モーダル確認ダイアログの Esc=cancel 等を奪わない)。
- テーブルはプロセス共有 (Window 単位ではない)。 印字キーの登録は非推奨
  (`onKeyPress` の文字イベントまでは抑止しない)。

実例が demolib (`data/demolib/demo_common.tjs` の DemoShell): パネルを
`showJson(json, true, false)` (フォーカスあり非モーダル) で出し、 シェル操作に
必須の ESC / PgUp / PgDn / パッド B / LB / RB をホットキー登録する。 パッドの
十字 / A はホットキーにしない = フォーカスパネルのウィジェット操作に流れ、
パネルが無いシーンでは素通しでシェルに届く。

`tTVPElementsDialogManager::ForwardKeyDown` / `ForwardKeyUp` は VK code を 2 種に振り分ける (`RouteVk`):

| 種別 | VK の例 | 配送先 |
|---|---|---|
| `key` | VK_RETURN / VK_TAB / VK_ESC / 方向キー / 英数字 | `overlay_session::on_key_down(cycfi::elements::key_code, mods)` |
| `pad_button` | VK_PAD1〜VK_PAD12 / VK_PADLEFT 等 / VK_PAD_L_LEFT 等 | `overlay_session::on_pad_button(cycfi::elements::pad_button, down)` |

overlay_session の入力 API は **host 非依存の cycfi 中立型** (`mouse_button::what` / `key_code` /
`pad_button` / `pad_axis` + `mod_*`) を受ける (SDL / Win32 のネイティブ enum は経由しない)。
manager は krkrz ネイティブ入力 (Windows VK / `tTVPMouseButton` / `TVP_SS_*`) を `RouteVk` /
`MouseButtonToElements` / `FlagsToElementsMods` で cycfi 型へ直接マップする。 VK_PAD↔pad_button の
対応は [Gamepad.md](Gamepad.md) を参照。 stick の 8 方向量子化 (VK_PAD_L_*) は DPAD 扱いに統合。

**テキスト入力**: `ForwardKeyPress(tjs_char)` が UTF-16 code unit を UTF-8 化して
`overlay_session::on_text_input` へ渡す (input_box 等の文字入力)。 SDL は
`SDL_EVENT_TEXT_INPUT` → `ForwardText`(完全 UTF-8)、 **WINVER は `WM_CHAR` →
`OnKeyPress` → `ForwardKeyPress`** 経路。 BMP 外 (絵文字 / 拡張漢字) は WM_CHAR が
high/low サロゲート 2 回に分けて配信するので、 `ForwardKeyPress` が high を保持して low と
合成し 1 コードポイント (最大 4 byte UTF-8) にする。

Elements 側はこれを受けて [keyboard / arrow / gamepad ナビゲーション](https://github.com/wamsoft/elements/blob/develop/docs/keyboard-navigation.md) で動く。 pad→key 合成のデフォルトは A=Enter / B=Esc / X=Shift+Tab / Y=Tab / D-Pad=矢印だが、 overlay ではその手前で **named-action バインド** (A→accept / B→cancel / LB,RB→page 等、 後述「named-action バインド」) が優先して発火する。

## テキスト入力とソフトキーボード (物理キーボードが無い環境)

テキスト欄 (`input_box`) に focus が入っている間だけ text 入力を有効にするのは
`ElementsDialogManager::Impl::UpdateFocusDrivenTextInput()` (PaintOverlay 末尾から
毎フレーム)。 分岐は 3 通り:

| 環境 | 挙動 |
| --- | --- |
| デスクトップ (WINVER / Windows SDL) | 何もしない。 text 入力は常時有効 (form 生成時に `SDL_StartTextInput`) |
| 物理キーボードあり (`SDL_HasKeyboard()`) | `SDL_StartTextInput` のみ。 OS のソフトキーボードは SDL の auto 判定で出ない |
| 物理キーボード無し | **内蔵仮想キーボード**を overlay で表示 (OS のキーボードは出さない) |

**内蔵仮想キーボード**は英数 4 段 + SPACE / BS / DONE の Elements ダイアログで、
`BuildVirtualKeyboardJson()` がレイアウトを組む。 押鍵は貯めずに**その場で入力先の
`overlay_session::on_text_input()` へ流し込む** (BS だけ `on_key_down(backspace)`)
ので、 下の入力欄がリアルタイムに更新される。 DONE / B で閉じ、 閉じた直後は
同じ欄で出し直さない (`vk_dismissed_for` ラッチ。 focus が一度外れると解除)。

- **前提**: `SDL_HasKeyboard()` が実態を返すこと。 各 video ドライバが
  `SDL_AddKeyboard` / `SDL_RemoveKeyboard` を呼んでいる必要がある (NX / PS5 は対応済み)。
- **PS5 の IME**: `SceImeDialog` は `ShowScreenKeyboard` ではなく `StartTextInput`
  フック内にあるため SDL 標準の `AutoShowingScreenKeyboard()` ポリシーが効かない。
  SDL3-playstation 側で物理キーボード接続時は開かないようガードしてある。
- **切替**: TJS の `Dialog.virtualKeyboard` プロパティで実行時に変更できる。
  `"auto"` (既定 = 物理キーボードが無いときだけ) / `"always"` (常に出す。 テスト用。
  デスクトップでも出る) / `"never"` (出さず OS 側に任せる。 表示中なら閉じる)。
  初期値は環境変数 `KRKRZ_FORCE_VIRTUAL_KEYBOARD=1` なら `"always"`。
- **既知の制限**: 大文字英数字のみ (v1)。 画面中央に出るため入力欄を覆うことがある。
  `focus_by_id` (Agent.dialogFocus) だけでは input_box が編集状態にならず
  `focus_consumes_text()` が false のままなので、 検証時は実クリックで focus させる。

## elements_modal ライブラリ

詳細は [`external/elements/external/elements_modal/README.md`](../external/elements/external/elements_modal/README.md)。 主要 API:

```cpp
// 独立 SDL_Window
bool run_modal(const std::string& json_utf8,
               const config& cfg,
               result& out_result);

// 既存サーフェスにオーバーレイ
class overlay_session {
    bool start(const std::string& json_utf8,
               int view_width, int view_height,
               float pixel_scale,
               event_callback external_cb = {});
    bool render_to_buffer(uint32_t* buffer, int buf_w, int buf_h,
                          int surface_w, int surface_h, render_rect& out_rect);
    void on_mouse_*  /* SDL → view 入力転送 */;
    void on_key_*   / on_text_input / on_pad_button / on_pad_axis;
    bool finished() / get_result();
};
```

`run_modal` は 2 パスで内容に合わせた window を作る:
1. JSON parse + view を要求サイズで作って `view.limits()` で自然サイズ測定
2. 自然サイズで SDL_Window を生成 (要求サイズが上限として機能)

ループ内では中央配置ロジックが残っているので、 ユーザがリサイズすれば自動センタリング/切詰めされる。

## JSON 仕様の要点

JSON / JSONC (コメント + 末尾カンマ) 対応。 要素タイプ・属性の全リストは [`external/elements/external/elements_modal/README.md`](../external/elements/external/elements_modal/README.md) を参照。 ダイアログ機構と密接に絡む特殊フィールド:

- **`"size": [w, h]`** (top-level) — ダイアログの希望論理サイズ (上限)。 実際は content の自然サイズにフィット縮小される (上側余白対策、 [project_elements_dialog_size] 系)。
- **`"align"`** + **`"margin"`** (top-level) — overlay 上での配置。 `align` は `"center"` (既定) / `"top"` / `"bottom"` / `"left"` / `"right"` と、 それらの組合せ `"top_left"` / `"top_right"` / `"bottom_left"` / `"bottom_right"` (文字列に `top`/`bottom`/`left`/`right` が含まれるかで縦横独立に判定)。 `margin` は非中央側のサーフェス端からの余白 px (既定 0)。 入力座標の補正 (overlay_session の last_rect) も同じ配置で行われるのでクリック判定はズレない。 全 overlay 経路 (showJson / showFlow / startFlow) で有効。 例: ゲーム画面左上にメニューを出す → `"align": "top_left", "margin": 24`。
- **`"initial_focus": true`** (focusable widget) — 起動時にフォーカスを当てる候補。 複数あった場合 build 順で先勝ち。
- **`text_area` ウィジェット** — 矩形に流し込む静的テキスト。 **本体 `Layer.drawShapedTextArea` と改行位置が一致する** (どちらも `glyphware::layoutBlock` を通るため。 行頭行末禁則つき) のが `text_box` との違いで、 加えて `"align"` / `"line_spacing"` / `"count_var"` (文字送り) を持つ。 字幕 / セリフ窓向け。 詳細は下の「矩形テキスト (`text_area`)」節。
- **`"close_on_click": true`** (button) — click で modal を閉じ、 `result.action = id` で確定する。 **デフォルト false** で、 click は `Dialog.onAction` を発火させるだけで終了させない。 OK / Cancel など「閉じるボタン」だけに付ける運用。 navigator フローでは、 画面遷移する button (transitions と組) と、 その場で動作させる button (close_on_click 無し → onAction のみ) を使い分ける。 なお TJS Dictionary 経由 (`showDict` 等) では true が int 1 で届くが、 bool 属性は number 0/非0 も真偽として受容する (elements_modal 2026-07-20 対応済。 古い pin では効かないので注意)。
- **`"gap"` (vtile/htile) / top-level `"style"` ブロック** — 既定で「詰まった」見た目になるのを避ける密度指定。 `{"type":"vtile","gap":8,...}` で子間に spacer 自動挿入相当、 top-level `"style": { "font_scale", "row_height", "tile_gap", "padding" }` で未指定値の既定をまとめて与える (詳細は elements_modal README「style ブロック」)。 いずれも省略で従来と完全一致。
- **`"input"`** (top-level) — ナビゲーション設定:
  ```jsonc
  "input": {
      "arrow_focus_nav": true,           // 矢印で 2D フォーカスナビ
      "focus_wrap": true,                 // 端で反対端へ回り込み (既定 false)
      "skip_disabled": true,              // disabled 要素を nav スキップ (既定 false)
      "repeat_delay_ms": 400,             // dpad/stick 長押しリピート開始
      "repeat_rate_ms": 80,               // 0 (既定) = 倒し量で 60〜250ms 可変
      "initial_focus": "BTN_START",       // id 指定の初期フォーカス (要素側フラグより優先)
      "dpad_mode":        "both",         // disabled / focus / value / both
      "left_stick_mode":  "focus",
      "right_stick_mode": "value",
      "trigger_mode":     "disabled",
      "stick_deadzone":   0.15,
      "stick_value_speed": 1.0,
      "pad_bindings": [
          // pad button → key への合成上書き (デフォルト A=Enter / B=Esc / X=Shift+Tab / Y=Tab)
          { "pad": "x", "key": "tab", "mods": ["shift"] }
      ],
      "shortcuts": [
          // 任意 key / pad → 要素 id のショートカット
          { "key": "f",  "mods": ["ctrl"], "target": "search_btn" },
          { "pad": "lb",                    "target": "cancel" },
          { "pad": "rb",                    "target": "ok", "force": true }
      ],
      "bindings": [
          // 入力 → named action (下記) のバインド。 組込デフォルトへの差分
          { "pad": "b",       "action": "cancel" },
          { "mouse": "right", "action": "none" },       // 既定バインドの無効化
          { "pad": "start",   "action": "open_menu" }   // 未知 action → onAction 通知
      ],
      "se": { "nav": "cursor.ogg", "accept": "ok.ogg", "cancel": "cancel.ogg" }
  }
  ```

`force: true` の shortcut は input_box 編集中でも反応する (リスト内編集中の save 押下を許容するケース等)。

### 矩形テキスト (`text_area`) — 本体と同じ折返し

Elements 側のテキスト折返しは元々 cycfi の素朴な幅貪欲 wrap で、**禁則も文字送りも
無く、本体 `Layer.drawShapedTextArea` と改行位置が揃わなかった**。 折返し本体を
glyphware (`glyphware::layoutBlock`) へ下ろしたのに合わせ、Elements にも
**新ウィジェット `text_area`** を足して同じロジックを消費させている。

- 経路: elements の注入 I/F `cycfi::elements::block_text_backend`
  (`external/elements/lib/include/elements/support/block_text.hpp`) を本体の
  `common/visual/elements/BlockTextBackend.cpp` が glyphware で実装し、
  `EnsureRuntimeInitialized` で `TVPInstallElementsBlockTextBackend()` が登録する。
  **バックエンドが決めるのは「どこで改行するか」「どこまで見せるか」だけ**で、
  グリフ描画は従来どおり ThorVG (`tvg::Text`) が行う。
- フォント連鎖は widget のフォント鍵 (`font::file()` = 登録時の storage キー) を
  先頭に、theme families (Latin → CJK → Emoji の並び) を鍵へ引き直して繋ぐ。
  ThorVG の per-codepoint フォールバックと同じ優先順になるので、計測と描画で
  使うフェイスがずれない。
- **既存 `label` / `text_box` は一切変えていない** (差し替えると既存画面の改行
  位置が動くため)。 使い分けは「従来互換 = `text_box` / 本体と揃える + 文字送り
  = `text_area`」。
- 文字送りは `"count_var"`。 ホストが `setVar("sub_count", "12")` するだけで進み、
  **折返しは全文で確定してから count を適用する**のでリフローしない。 数える単位は
  クラスタ (合字 / 結合列 / 絵文字 ZWJ シーケンスで 1) で、`Layer.shapedTextCount`
  と同じ。
- 落とし穴: `floating` / fit-to-content の親に置くと、パネルサイズが content の
  最小サイズから決まる。 絶対座標で置くなら top-level `"size": [w, h]` を明示する
  (指定しないとパネルが内容サイズまで縮み、`floating` の絶対座標が外へ出て何も
  見えなくなる)。

- `text_list` / `text_list_id` + `index_var` (指定番号表示 + i18n 言語切替追従) も
  label と同じ規約で使える。

フィールド一覧は elements_modal README の `text_area` 項を参照。

### named-action バインド (`"bindings"` / `input_defaults.jsonc`)

「閉じる / 決定 / ページ送り」等は**名前付きアクションへの 3 層バインド** (後勝ち)
で決まる: ①組込デフォルト → ②`input_defaults.jsonc` → ③画面別 `"input"."bindings"`。
詳細仕様は elements_modal README の「named-action と組込デフォルト標準バインド」を参照。

- **組込デフォルト**: Esc / B / **右クリック** → `cancel` (閉じる、`onClose` の action は "")、
  A → `accept`、 X/Y → `focus_prev/next`、 LB/RB → `page_prev/next` (tab_view のタブ送り)、
  ホイール → `scroll_up/down`。 Enter / Tab / 矢印 / PageUp/Down キーはネイティブ経路が
  同じ意味を実装済み (identity のため登録対象外)。
- **旧仕様との差分**: overlay の Esc は以前 `overlay_session` に hard-code されていたが、
  現在は escape→cancel の組込バインド (force=true) 経由。 画面 JSON で差替や
  `"action": "none"` による無効化ができる。 B ボタン・右クリックでも同様に閉じる。
- **`"action": "none"`** = 該当入力を消費して何もしない (下層バインドとネイティブ
  フォールスルーも遮断)。 組込以外の action 名は `onAction("<action>", 名前)` で
  TJS へ通知される (画面横断 quick action の実装口)。
- **`input_defaults.jsonc`** (プロジェクト共通層): **resource_base 直下**に置くと
  初回表示時に 1 回ロード・キャッシュされる (top-level は `"input"` ブロックと同形。
  変更反映はアプリ再起動)。 ⚠ `showFile` / `showJson` / `showDict` 系は
  resource_base が**空** (= 画面 jsonc 内のパスはプロジェクトルート相対で書く運用)
  なので、 置き場所は **data ルート直下**になる。 manifest フロー (`showFlow`) では
  manifest のディレクトリが resource_base。 ロード成否は stderr の
  `elements_modal: input defaults "...": loaded/not used` で確認できる。
- **⚠全画面透過の非モーダル overlay** (常駐 HUD 等) は描画矩形が全面のため
  右クリックが常にヒットし、 既定 cancel で意図せず閉じる。 その画面の
  `"input"."bindings"` に `{ "mouse": "right", "action": "none" }` を入れるか、
  ホスト側で右クリックを使うなら `Dialog.registerHotKey(VK_RBUTTON)` で
  ダイアログへ渡さずホストへバイパスする (「ホストホットキー」参照)。

### cursor-warp ナビ (`"cursor_warp"`)

`"input": { "cursor_warp": true }` (input_defaults.jsonc で全画面一括可) にすると、
**キー/パッドでフォーカスが動いたとき実マウスカーソルがフォーカス先へ warp** し、
カーソルは mcsTempHidden で一時非表示になる (実マウスを動かすと通常復帰)。
カーソルがフォーカス widget に乗るため、 **hover の見た目 (hilite フレーム /
hover 演出 / vars_on_focus) がキー操作のフォーカスに自然追従**する。 ホイールや
トラッククリック等のマウス操作もフォーカス位置が対象になる。

- 飛び先 = widget の **focus hot point**。 既定は bounds 中心、 slider は thumb
  中心 (トラッククリックの値ジャンプ防止)、 choice_nav グループは選択中メンバー。
  widget の `"focus_point": [ax, ay]` (0..1 アンカー比) で個別調整可。
- hover 由来 (hover_focus) のフォーカス移動では warp しない (実カーソルと喧嘩
  しない)。 マウス操作に戻ると次のキー操作まで warp は起きない。
- 実装: session が `take_key_focus_move()` でワンショット通知 → manager が
  PaintOverlay 終端で present 変換の逆写像で layer 座標化し
  `iTVPWindow::SetCursorPos` + `SetMouseCursorState(mcsTempHidden)`。
  warp が生む合成 mouse move は期待座標一致で判別し、 カーソル再表示させず
  session へは流す (= hover 更新)。 実マウスの move (座標不一致) で解除。

### SE フックと擬似 id (`onAction`)

`"se"` マップ (キー = カテゴリ `nav`/`accept`/`cancel`/`page`/`scroll`、 または
個別 action 名・button id) を宣言すると、 アクション発火時に
**`onAction("<se>", SE名)`** が TJS へ届く (Elements は音を鳴らさない。 再生は
TJS 側の責務 — kag.se 等)。 `nav` はフォーカス変化検出 (キー/dpad/stick/hover
どの経路でも)、 `accept` は button click で一元発火、 `cancel`/`page` 等は
アクションディスパッチ時。 SE 未宣言なら一切通知されない。
組込以外の action は **`onAction("<action>", action名)`** で届く。 通常の
widget id と混同しないよう、 TJS 側 router は `"<se>"` / `"<action>"` を
先に分岐すること。

## TJS Dictionary レイアウト (showDict / showModalDict)

JSON 文字列の代わりに TJS の Dictionary / Array でレイアウトを直接書ける。
`common/visual/elements/VariantJsonUtil.cpp` の `TVPVariantToJsonUtf8()` が
Dictionary → JSON テキストに変換して既存の JSON 経路へそのまま流す
(elements_modal 側の入口は JSON のまま)。

```tjs
var dlg = new Dialog();
dlg.showDict(%[
    size: [360, 220], background: [30, 30, 60, 245],
    content: %[ type: "vtile", children: [
        %[ type: "label", text: "タイトル", size_scale: 1.3 ],
        %[ type: "vsize", height: 38,
           child: %[ type: "button", id: "ok", text: "OK", close_on_click: true ] ],
    ] ],
]);
// モーダルも同様 (引数仕様は showModalJson と同じ):
//   dlg.showModalDict(dict);                    // overlay
//   dlg.showModalDict(dict, "Title", 560, 700); // 独立 window
```

- **`showFlowScreens` / `startFlowScreens` の画面マップ値も Dictionary 可**
  (JSON 文字列と混在できる)。
- **`Dialog.dictToJson(value)`** で変換結果の JSON 文字列を取得できる
  (デバッグ / JSON 資材の書き出し用)。
- 対応型: void→null / Integer / Real / String / Dictionary / Array。 Octet・
  それ以外のオブジェクト (関数等)・循環参照・非有限 Real は TJS 例外。

### TJS 側の言語制約 2 点

- **bool が書けない**: TJS2 に boolean 型はなく `true` は整数 1 なので、
  `close_on_click: true` は JSON の `1` (number) になる。 このため
  elements_modal の bool フィールド読み取り (`json_layout.cpp` の
  `bool_field` / `truthy_field`) は **number の 0 / 非 0 も真偽値として受け
  付ける** (JSON 文字列で書く場合は従来どおり true/false 推奨)。
- **空文字キーが書けない**: TJS Dictionary は `""` キーを保持できないため、
  navigator の既定遷移 `"": "<exit>"` は Dictionary 形式では書けない。
  未定義 action のフォールバック (entry なら exit / 子画面なら pop) で足りる
  ケースが大半。 明示したい画面だけ JSON 文字列で書いて混在させればよい。

## TJS Dialog の使い分け

```tjs
// 非モーダル (D mode): onAction で逐次反応、 close() で閉じる
class TestDialog extends Dialog {
    function onAction(id, payload) {
        switch (id) {
        case "ok": System.inform("OK"); close(); break;
        case "cancel": close(); break;
        }
    }
}
var dlg = new TestDialog();
dlg.showJson(json);   // 非ブロッキング、 close まで継続

// ブロッキングモーダル (E / O mode): close_on_click=true な button で閉じる
class ModalDialog extends Dialog {
    // close_on_click=false な button click が来たときだけ意味を持つ
    function onAction(id, payload) {
        dm(@"action: ${id} payload=${payload}");
    }
}
var dlg = new ModalDialog();
var result = dlg.showModalJson(json, "Title", 560, 700);  // 独立 window
//              dlg.showModalJson(json);                    // overlay
// result.action: 閉じた button の id (Esc / × は "")
// result.values: state widget の最終値マップ
```

`onAction` は state widget の値変化 / 全 button click に発火する。 `result.action` / `result.values` は close 時のスナップ。

### 例外への文脈付加

show* / showModal* / showFlow* / startFlow* の全 API 入口で、 elements /
elements_modal / host 別 runner 由来の C++ 例外 (`std::exception` および不明型)
を `Dialog.showModalFile(ui/xxx.jsonc): <what()>` 形式の TJS 例外へ変換する
(`WithDialogExceptionContext`、 `DialogIntf.cpp`)。 TJS 例外 (`TVPReadStream` の
ストレージエラー等) は元々メッセージ完備なのでそのまま透過。 画像読込失敗は
elements 側 pixmap が対象リソース名を例外メッセージに含め、 「不存在 (loader が
empty)」 と 「デコード失敗 (バイト数 + 拡張子付き)」 を区別する。

## 複数画面フロー (navigator)

1 つの overlay 上で **複数の画面 (JSON) を遷移**させる仕組み。 各画面 JSON の
top-level `"transitions"` ブロックが「閉じトリガの action id → 次手」を定義し、
`elements_modal::navigator` が push / pop / replace / stay / exit を解決する。
描画 / 入力 / ファイル読込は `tTVPElementsDialogManager` が担い、 画面切替時に
`onScreen` / `onScreenLeave` を発火する。 駆動は overlay ブロッキング (showModal
overlay と同じ nested pump)。

```tjs
class FlowDialog extends Dialog {
    function onScreen(name)            { dm(@"enter: ${name}"); }
    function onScreenLeave(name, act)  { dm(@"leave: ${name} (${act})"); }
    function onAction(id, payload)     { /* 各 widget の値変化 / click */ }
}
var dlg = new FlowDialog();

// (A) マニフェスト駆動 — app.jsonc を Storages から読む。 画面ファイルは
//     マニフェストと同じディレクトリ起点、 各画面の相対資材パスはその画面
//     ファイルのディレクトリを起点に Storages 解決する。
var result = dlg.showFlow("ui/app.jsonc");

// (B) インライン駆動 — ファイル I/O なし。 画面名→JSON文字列の辞書 + 起点画面名。
var screens = %[
    "menu":     '{ "background":[28,30,40,255], "transitions":{ "settings":"settings", "":"<exit>" }, "content": {...} }',
    "settings": '{ "background":[40,30,30,255], "transitions":{ "back":"<back>", "":"<back>" },     "content": {...} }'
];
var result = dlg.showFlowScreens(screens, "menu");
// result はフロー終了時 (スタックが空) に最後に閉じた画面の %[action, values]
```

`"transitions"` の target 語彙: `"<name>"` (= 画面名 push、 山括弧不要) / `"<back>"`
(pop) / `"<replace:name>"` / `"<stay>"` / `"<exit>"` (または空 target)。 未定義の
action は「entry なら exit / 子画面なら pop」にフォールバックする。 画面ごとの
focus と表示言語は navigator が遷移をまたいで記憶 / 復元する。

### 画面切替エフェクト (`effect`: fade / universal)

transitions のエントリを object 形式にすると、 画面切替時の遷移エフェクトを
宣言できる。 **krkrz overlay 側で配線済み** (CPU 合成なので SDL / WINVER /
GL 全 DrawDevice で同一動作):

```jsonc
"transitions": {
    "next": { "target": "s2", "effect": "fade", "duration": 300 },
    "back": { "target": "<back>", "effect": "universal",
              "rule": "rule.png", "vague": 64, "duration": 500 }
}
```

| キー | 意味 |
|---|---|
| `effect` | `"fade"` = クロスフェード / `"universal"` = rule 画像によるユニバーサルトランジション。 未対応名は警告ログ + 即切替 |
| `duration` | ms。 0 / 省略 = 200ms |
| `rule` | universal の rule 画像 (グレースケール、 値が小さい画素ほど早く次画面へ切替)。 解決順 = **遷移を宣言した画面 (旧画面) の resource_base 相対** → Storages パスそのまま → autopath 検索 |
| `vague` | 境界ぼかし幅 (rule 値スケール 0-255、 既定 64) |

実装メモ (ElementsDialogManager):
- session は finish 後に再描画できないため、 nav フローの各インスタンスは
  **直近描画フレームの複製 (`last_frame`) を毎フレーム保持**し、 遷移確定時に
  from 側スナップショットへ move する。 混色は `elements_modal/effects.h` の
  `blend_argb8888` (fade) / `blend_universal_argb8888` (universal、 4ch 対応) を
  新画面の staging バッファへ in-place 適用 (テクスチャ upload 前)。
- rule 画像は `TVPLoadGraphic` (glmGrayscale) → バイリニアで buffer サイズへ展開。
  ロード失敗時は fade へフォールバック (警告ログ)。
- 新旧で buffer サイズが変わった場合 (画面サイズ / renderScale 変更) は即切替
  フォールバック。 旧画面が一度も描画されていない場合も即切替。

### 退場 (exit) 演出と close の協調

要素の `"animate"` に `"on": "exit"` を付けると、 画面が閉じる / 遷移するとき
退場演出を再生してから finish する (overlay_session 内で自動協調)。 これは
close_on_click / Esc 等の画面内トリガに加え、 **TJS `Dialog.close()` からの
外部 close でも発火する** (manager が `session->close()` 経由で閉じ、 演出完了後に
teardown する。 transitions は解決せずフローごと終了)。 Window close 等の即時
破棄経路 (`ForceClose` / handler 破棄) は演出なしで即 teardown。

### 非モーダル (常駐) フロー — `startFlow` / `startFlowScreens`

`showFlow` (ブロッキング) に対し、 `startFlow` は **即 return し、 画面に
出しっぱなしのまま** フローを常駐させる。 画面遷移は DrawDevice の
`PaintOverlay` が毎フレーム駆動し、 イベントは `onScreen` / `onScreenLeave` /
`onAction` で受ける。 ゲーム画面の上にメニューを常駐させ、 背景でサンプルを
動かしながらメニュー操作を続ける、 といった用途に使う。

```tjs
dlg.startFlow("ui/menu/app.jsonc");   // 非ブロッキング、 戻り値 = 起動成否 (bool)
// dlg.startFlowScreens(screensDict, entry);   // インライン版
// dlg.active  … この dlg が今アクティブなフロー/ダイアログのオーナーか (getter)
// dlg.close() … 閉じる (次フレームで teardown)
```

メニュー常駐パターンの肝:
- **画面遷移 (push/pop)** する button は `"close_on_click": true` + `transitions`。
  `close_on_click` で session が finish → `PaintOverlay` が `advance` して遷移。
- **その場で動作させる (閉じない)** button は `close_on_click` 無し。 click すると
  `onAction` だけ発火し、 フローは現画面に留まる。 ホストはそこで背景サンプルを
  起動 (= メニュー出しっぱなしで背景動作)。
- **別の非モーダル UI / modal の重ね出し** は R6 以降そのまま併存できる。 常駐メニュー
  (`modal=false`) を出したまま `showModalOverlayJson` 等で `modal=true` を重ねると、
  modal が最前面で入力を独占し、 下の常駐メニューは描画だけ維持される。 modal を閉じれば
  メニュー操作に戻る。 旧来は単一インスタンス制約のため `close()` → `dlg.active` 待ち →
  遅延起動が必要だったが、 **その回避策はもう不要**。

実例は `data/startup.tjs` の `FlowMenuDialog` / `MyWindow.openMenu` /
`dispatchSample` と `data/ui/menu/*.json`。

## 複数 Dialog / 複数インスタンスの ownership

各 TJS `Dialog` インスタンスは自分の `handler` ポインタで manager 内の自分の overlay
インスタンスを識別する。 `Dialog::Invalidate` / `Close` は **自分の handler が active な
ときだけ** `mgr.Close(this)` を呼ぶ:

```cpp
auto& mgr = tTVPElementsDialogManager::Instance();
if (mgr.IsHandlerActive(this)) {
    mgr.Close(this);   // 自分のインスタンスだけを閉じる (他人を巻き込まない)
}
```

R6 で複数インスタンスが並存できるようになったため、 `Close()` (引数なし = 最前面を閉じる)
ではなく **`Close(handler)`** で自分のインスタンスだけを対象にするのが要点。 `dlg.active`
getter も `IsHandlerActive(this)` を返す。 ブロッキングモーダルの pump ループも
`IsModalActive()` (= 何か 1 つでも) ではなく `IsHandlerActive(handler)` (= 自分のが閉じたか)
で終了判定するので、 背景に非モーダル常駐 UI が居ても正しく抜けられる。

## フェーズ状態

| Phase | 内容 | 状態 |
|---|---|---|
| 1 | submodule で `external/elements` 追加 | 完了 |
| 1.5 | Elements スタンドアロンビルド確認 | 完了 |
| 2 | DialogManager 骨組み + DrawDevice インターセプト + Show() フック | 完了 |
| 3 | SDL3 アダプタで MVP | 完了 |
| 4 | OGL アダプタ追加 | 完了 |
| 5 | WINVER `BasicDrawDevice` アダプタ (`tTVPD3D11DialogRenderer` + `iTVPD3D11DialogHost`) + WM_CHAR テキスト入力 + overlay-modal (nested Win32 pump) + フォント埋込 (resources.rc の `BINARY` 型を `register_font_buffer`) | 完了 |
| 5b | 描画アダプタ提供口の汎用化 (`iTVPDialogRendererHost` + `dialogRendererHost` TJS プロパティ + tp_stub 公開)。 プラグイン / 差し替え DrawDevice 対応 | 完了 |
| 6a | JSON レイアウト構築層 (krkrz JsonLayout) | 完了 |
| 6b | TJS バインディング (`Dialog.showJson` / `showFile` / `close` / `onAction`) | 完了 |
| 6c | TJS `showModalJson` / `showModalFile` (独立 window + overlay 両モード) | 完了 |
| 6d | `Dialog.onAction` を showModal* でも発火 + `close_on_click` で閉じる ボタン明示 | 完了 |
| 7  | UserConfig を Elements で実装 (SDL3 ビルド) | 完了 |
| 7d | JsonLayout 要素拡張 / VT_String 編集 UI / plugin sidecar JSON 等 | 未着手 |
| 8  | navigator 複数画面フロー (`showFlow` / `showFlowScreens` + `onScreen` / `onScreenLeave`、 manifest/inline 両対応、 Storages 資材解決) | 完了 |
| 8t | **画面切替エフェクト** (`effect: fade / universal` + `rule` / `vague`、 CPU 合成で全 DrawDevice 対応) + `Dialog.close()` の exit 演出協調 | 完了 (SDL / WINVER 実機検証済) |
| R6 | **複数インスタンス同時表示** (z-order インスタンスリスト + layer 単位テクスチャ + `modal` フラグ + ヒットテスト入力ルーティング + 素通し)。 非モーダル常駐 UI の並存 / modal の重ね出しが可能に | 完了 (描画・ロジック実装済、 GUI 実機での重ね操作検証は未) |

## Phase 7d 拡張候補 (JsonLayout 要素)

汎用化想定の JSON UI 仕様メモを基に、 現実装に未対応のものを優先度順に挙げる。

| カテゴリ | 候補 type | 備考 |
|---|---|---|
| レイアウト | `vtile_spaced` `htile_spaced`(spacing) / `deck` / `hgrid` `vgrid`(widths) | spaced は spacer で代替可能だが冗長 |
| 配置 | `margin_left` `margin_top` / `halign` `valign`(align 値) / `fixed_size`(w/h 直指定、 現状は spacer 経由) / `max_size` | 中身に依らずサイズを縛りたいケース |
| コントロール | `icon_button` / `radio_button`(group) / `slider`(init/min/max) / `text_box`(read_only) | radio group 管理と slider は仕組みが要 |
| 装飾 | `pane`(title) (現状 `group` に集約済) / `rbox`(radius) / `frame` | pane と frame は実質 group で代用可 |
| テキスト | `font_size` `font_color` `font_family` `font_weight` `font_slant` `text_align` / `icon` 名前マップ | label/button の表現力 |
| 画像 | `image`(path/scale/fit) — krkrz Storage 経由 | 別系統 |
| 値 API | id ベースの `get_text` / `set_text` / `get_bool` / `set_bool` / `get_value` / `enable` / `select` / `show` / `refresh` | 現状は `OnAction` 一方向のみ |
| 色形式 | `"#RGB"` `"#RRGGBB"` `"#RRGGBBAA"` / 名前色対応 | 現状は `[r,g,b,a]` 配列のみ |
| 拡張 | plugin sidecar JSON 自動 scan (UserConfig 拡張) | |

## 残課題

- `selection_menu` でドロップダウン選択後にフォーカスが下のボタンに戻ってしまう (Elements 側)
- 一部 widget で文字が重なる挙動 (font metrics 起因の可能性、 [feedback_elements_font_init_order] とも関係)
- DPI 全体仕様再検討 ([project_dialog_dpi_spec_rework] 参照)
- ~~WINVER: DPI スケーリングの異なるモニター間へ window を移動すると DestRect がオフセット~~
  → **修正済** (`SetDrawDeviceDestRect` の windowed 分岐で layer×zoom を実クライアントへ letterbox
  フィット。 `doc/D3D11Migration.md` 参照)。
- ~~WINVER: 独立 OS ウィンドウ modal の本実装~~ → **実装済** (`TVPRunElementsModalWindow`、 上記参照)。
- ~~WINVER: Agent 駆動 API~~ → **対応済** (`common/environ/AgentControlIntf.cpp` + `AgentInput` seam)。
- WINVER の起動時 UserConfig UI (`-userconf`) は **Win32 ネイティブ版** (`ConfigFormUnit.cpp`) が既存で
  動作する。 Elements 版 UserConfig (`SDLElementsUserConfig.cpp`) は SDL 専用のまま
  (ゲーム窓生成前の独立ウィンドウ = 上記 modal 基盤とは別に UI 移植が要る)。
