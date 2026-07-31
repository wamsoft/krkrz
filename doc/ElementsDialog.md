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

例: 左上メニュー (`grabFocus=true`) + 右下 HUD パネル (`grabFocus=false`) を同時表示。
メニューにフォーカスがあるが、 メニューが使わない `P` キーは passthrough でゲームに届き、
ゲーム側ホットキーが生きる (`data/startup.tjs` の openMenu / togglePanel)。

`modal=true` なインスタンスが最前面なら、 矩形外のクリックも含めて全入力を独占するので、
従来のモーダルダイアログと同じ「下を触れない」挙動になる。 非モーダルの常駐メニュー +
背景のゲーム動作を併存させたいときは `startFlow` (= `modal=false`) を使う。

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

Elements 側はこれを受けて [keyboard / arrow / gamepad ナビゲーション](https://github.com/wamsoft/elements/blob/develop/docs/keyboard-navigation.md) で動く。 デフォルト bind は A=Enter / B=Esc / X=Shift+Tab / Y=Tab / D-Pad=矢印。

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
- **`"close_on_click": true`** (button) — click で modal を閉じ、 `result.action = id` で確定する。 **デフォルト false** で、 click は `Dialog.onAction` を発火させるだけで終了させない。 OK / Cancel など「閉じるボタン」だけに付ける運用。 navigator フローでは、 画面遷移する button (transitions と組) と、 その場で動作させる button (close_on_click 無し → onAction のみ) を使い分ける。
- **`"input"`** (top-level) — ナビゲーション設定:
  ```jsonc
  "input": {
      "arrow_focus_nav": true,           // 矢印で 2D フォーカスナビ
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
      ]
  }
  ```

`force: true` の shortcut は input_box 編集中でも反応する (リスト内編集中の save 押下を許容するケース等)。

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
focus と表示言語は navigator が遷移をまたいで記憶 / 復元する。 `effect`
(`"fade"` 等) は仕様としては解釈されるが、 krkrz overlay 側の演出配線は後フェーズ。

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
| 8  | navigator 複数画面フロー (`showFlow` / `showFlowScreens` + `onScreen` / `onScreenLeave`、 manifest/inline 両対応、 Storages 資材解決) | 完了 (fade 演出は後フェーズ) |
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
