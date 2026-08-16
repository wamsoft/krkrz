# GLCompositor — 本体オフスクリーン GL 合成

`GLCompositor` は、画面の描画デバイスが OpenGL でない状況 (WINVER 既定の
`BasicDrawDevice`=D3D11、SDL の `-drawdevice=sdl` 等) でも、**裏で GLES による
オフスクリーン合成**を行い、その結果を `Layer` へ書き戻すための本体クラス。

旧 `krkrgles` プラグインの `GLESAdaptor` が担っていた用途 (旧吉里吉里に GL が
無かった時代に、HWND から自前で EGL/ANGLE を初期化してオフスクリーン合成する)
を、**本体内蔵**かつ `Canvas` / `Offscreen` の既存 GL コードを共有する形で提供する。
`krkrgles` は旧吉里吉里向けとして残す。

## 仕組み

- 描画デバイスに依存せず、ウィンドウの native window から
  `iTVPGLContext::GetContext()` で GL コンテキストを取得し、自前で `MakeCurrent()`
  する。OGL 描画デバイスが稼働中なら (WINVER は HWND 単位でキャッシュ+refcount
  される `tTVPEGLContext` を) 共有する。
- **present (Swap) は一切しない**。常にオフスクリーン FBO へ描き、`glReadPixels`
  で読み戻すだけなので、D3D11 が画面を持っている裏で安全に併用できる。
- 描画そのものは内部の `Canvas` / `Offscreen` に委譲する (GL 描画コードは
  二重実装しない)。

## TJS API

```tjs
var comp = new GLCompositor(window);   // ウィンドウから GL コンテキストを得る
comp.capture(layer, function(canvas, w, h, param) {
    // canvas は内部 Canvas。Canvas のフル API が使える
    //   canvas.drawTexture(new Texture(bmp), ...);
    //   canvas.beginEffect(); ...; canvas.endEffect(commands);
}, param, 0xff000000 /* クリア色 */);
// layer に合成結果が書き戻されている
```

| メンバ | 説明 |
|---|---|
| `new GLCompositor(window)` | window の GL コンテキストで初期化 |
| `capture(layer, callback, param, color)` | layer サイズの FBO を color でクリア→`callback(canvas,w,h,param)` で描画→layer へ読み戻し |
| `drawLayer(layer, a,b,c,d,tx,ty, opacity=255)` | layer をアフィン変換 (2x2=a,b,c,d + 平行移動 tx,ty) + 不透明度で現在の描画先へ描く。`capture` のコールバック内で使う |
| `copyLayer(layer, left, top)` | layer を (left,top) にそのまま描く (`drawLayer` の平行移動版) |
| `canvas` (RO) | 内部 Canvas。`drawTexture` / `beginEffect` / `endEffect` / クリップ等フル API |
| `blendMode` | 内部 Canvas の blendMode |
| `unpremultiply` | `capture` の読み戻しで premultiplied-alpha を straight-alpha へ戻すか (既定 false)。旧 `GLESAdaptor.unpremultiply` 相当 |
| `setScreenSize(w,h)` / `screenWidth` / `screenHeight` | 既定サイズ |
| `makeCurrent()` | GL コンテキストをカレントに |
| `GLGetProcAddress` (RO) | GL エントリポイント解決関数のポインタ (tjs_int64)。GLES 系プラグインの oglbase として使う (例: `new EffekseerDevice(comp)`) |

## GLES 系プラグイン用 GLGetProcAddress

krkreffekseer 等の GLES プラグインは、oglbase オブジェクトの `GLGetProcAddress`
プロパティ (GL エントリポイント解決関数へのポインタ) を読み、現在カレントな GL
コンテキストへ描画する。これを以下の 2 箇所に用意している (中身はどちらも
`TVPGLGetProcAddress` のポインタ):

- **`GLCompositor.GLGetProcAddress`** … オフスクリーン合成用。GLCompositor が
  自前コンテキストを持つので、これを oglbase にすると `capture` 中に描ける。
- **`Window.GLGetProcAddress`** … 汎用。**初回アクセス時に GL コンテキストが未
  初期化なら遅延生成してカレントにする**ため、OGLDrawDevice / Canvas を使う場合でも
  ウィンドウから取得できる。取得できない環境では 0 (null) を返す。
  (OGLDrawDevice 稼働中は同一ウィンドウの GL コンテキストを共有するので、
  OGLDrawDevice 側に別途口を設けなくても Window から取得すれば足りる。)

`drawLayer` / `copyLayer` は、レイヤのメインイメージを `Layer::CopyFromMainImage`
(内部ビットマップ参照の移送で安価) で Bitmap へ移し、そこから `Texture` を構築して
Canvas の `DrawTexture` で描く。旧 krkrgles の同名メソッド互換。

## 実装

- `common/visual/opengl/GLCompositorIntf.{h,cpp}`
- `Canvas::Capture(tTVPBaseBitmap*, x,y,w,h)` — TJS Bitmap を介さず Layer の
  メインイメージへ直接読み戻すオーバーロード (`CanvasIntf.{h,cpp}`)。
- `ScriptMgnIntf.cpp` で `TVP_USE_OPENGL` 下に登録、`sources.cmake` に追加。
  全バリアント (WINVER/SDL/generic) 対象・新規依存なし。

## 検証状況

- WINVER (D3D11 既定) 実機で確認済 (SDL/WINVER 両ビルド確認済):
  - off-device コンテキスト生成 + `capture`→レイヤ書き戻し (緑塗り→pixel 0x00FF00)。
  - `copyLayer`: 上緑/下赤 32x32 を (16,16) へ配置 → 上=緑(0x00FF00)/下=赤(0xFF0000)/
    外=黒。位置・向き (反転なし)・クリップ OK。
  - `drawLayer` 不透明度: 全面緑を opacity=128 で黒地へ → 0x008000 (緑 50%)。

## SDL/GL 画面デバイス上での GL コンテキスト分離 (2026-08-09 修正)

**症状 (修正前)**: SDL3 ビルド (`bin/x64-windows`、画面デバイス = `SDLOGLDrawDevice`
= GL) で GLCompositor の `capture` を毎フレーム回すと、**レイヤ書き戻しは正常なのに
画面提示が真っ黒**になった (`-shot` で層 bitmap を CPU 合成すると絵は出る = 描画自体は
成功、ライブ画面だけ黒)。利用例: `d:/test/vrmsys`(VRM 立ち絵) を SDL ビルドで起動。

**根因**: SDL3 の `iTVPGLContext::GetContext` (`sdl3/environ/form.cpp` の
`SDL3GLContext`) が別コンテキストを作らず、その時点でカレントな SDL GL コンテキストを
そのまま流用していた。`SDLOGLDrawDevice` も GLCompositor も同じ `GetContext` を呼ぶため
両者が**同一の `SDL_GLContext`** を共有し、GLCompositor.capture が自前 FBO を bind →
threepp 描画 → 読み戻し した後、同一コンテキストの FBO バインド/GL ステートが残って
`SDLOGLDrawDevice` の画面合成 (FBO 0 への present) を壊していた。
`SDLOGLDrawDevice::Show()` は毎フレーム `MakeCurrent()` するが、**同一コンテキストなので
MakeCurrent が実質 no-op** になり、汚れたステートが持ち越されるのが黒画面の原因。

**WINVER (D3D11 画面) では問題にならない**理由: ① `GetContext` が `tTVPEGLContext`
(HWND 単位でキャッシュ+refcount された EGL/ANGLE の独立コンテキスト) に解決される
② そもそも画面デバイスが D3D11 (非GL) なので GL コンテキストの競合が起きない。
GLCompositor の設計上の主目的 (画面が非GLの環境でオフスクリーン GL 合成) がまさにこれ。

**修正**: `iTVPGLContext::GetContext` に `separateShared` 引数を追加
(`GetContext(nativeWindow, separateShared=false)`)。

- 画面デバイス (`SDLOGLDrawDevice` 等) は従来どおり `separateShared=false` で主コンテキストを
  取得 (挙動不変)。
- **GLCompositor は `separateShared=true`** で取得する。SDL 版はこのとき、現在のコンテキストが
  あれば `SDL_GL_SHARE_WITH_CURRENT_CONTEXT` を立てて**専用の GL コンテキストを新規生成**する
  (テクスチャ/シェーダ等リソースは画面コンテキストと共有・**FBO/VAO 等のコンテナと GL ステートは
  独立**)。生成後は直前のカレントコンテキストへ戻して画面デバイスの状態を乱さない。この専用
  コンテキストは GLCompositor が所有し、`Invalidate`→`Release` で `SDL_GL_DestroyContext` する
  (画面用コンテキストは従来どおり破棄しない)。
- こうすると画面デバイスの毎フレーム `MakeCurrent()` が**別コンテキストへ実際に切り替わり**、
  FBO バインド/ステートが分離されるので黒画面が解消する。
- WINVER 版は `separateShared` を無視 (画面 D3D11 で競合しないため主 EGL コンテキスト共有で足りる)。

**★もう一段の要点 — 主コンテキストの確定キャッシュ**: 分離コンテキスト生成だけでは不足だった。
`separateShared=false` (画面用) が SDL 実装で `SDL_GL_GetCurrentContext()` (=「主」ではなく
「現在」カレントなコンテキスト) を返していたため、画面デバイスがコンテキストを確定する初回
ペイント時点で、先に生成された **GLCompositor の分離コンテキストがカレントだと、それを主
コンテキストと誤採用**してしまう (→ 画面と compositor が再び同一コンテキストへ縮退し黒画面)。
対策として `sMainGLContexts` (SDL_Window→SDL_GLContext マップ) を設け、`separateShared=false` は
`TVPGetOrCreateMainGLContext()` で**ウィンドウ毎に一度だけ主コンテキストを確定してキャッシュ**し、
以降はカレントに依らず同じものを返す (初回確定は compositor 生成前の SetWindowInterface /
InitGLES 時に起きるので真の主コンテキストを捕捉できる)。破棄は `DestroyNativeWindow` で
キャッシュした主コンテキストを確定的に行い、compositor の分離コンテキストは所有元
(`SDL3GLContext` mOwned) の `Release` に任せて二重破棄を避ける。

実装: `common/visual/opengl/OpenGLContext.h` (I/F)、`sdl3/environ/form.cpp` (`SDL3GLContext`
に `mOwned`/refcount + 分離コンテキスト生成、`sMainGLContexts` 主コンテキストキャッシュ +
`TVPGetOrCreateMainGLContext` + `DestroyNativeWindow` での確定破棄)、
`win32/visual/OpenGLPlatform.cpp` (フラグ無視)、
`common/visual/opengl/GLCompositorIntf.cpp` (取得を `separateShared=true` に)。

**代替手段 (引き続き有効)**: SDL/GL ビルドで単に 3D 立ち絵を出すだけなら、GLCompositor
(オフスクリーン+readback) ではなく **OGLDrawDevice 直描き** (画面と同一 backbuffer へ直接
描く。readback 無しで軽い。実例: `d:/test/rpgsys` の `RpgDrawDevice`) の方が適する場合がある。

## 未対応 (予定)

- 入口の一般化 (画像用途の Layer 引数を Bitmap or Layer 受けにする横断対応) は
  プラグイン対応含め別枠計画で扱う。
- 実素材での合成挙動のさらなる目視確認。

## `unpremultiply` プロパティ (旧 GLESAdaptor 相当・移植済 2026-08-09)

旧 `GLESAdaptor` の `unpremultiply` プロパティを `GLCompositor` へ移植した。`true` に
すると `capture` の読み戻し時に **premultiplied-alpha を straight-alpha へ戻して**
(RGB = RGB×255÷A、四捨五入・255 クランプ、A=0 は全 0) Layer のメインイメージへ書く。
既定は `false` (従来どおり素通し)。

MSAA を効かせた 3D (threepp/VRM 立ち絵) の**半透明縁が premultiplied のまま読み戻ると、
通常アルファ合成 (ltAlpha) で縁に白フリンジ**が出る。これを防ぐためのもの
(threepp 側の VRM 輪郭 AA と組で使う。旧利用例:
`d:/test/vrmsys/data/startup.tjs` / `tachie.tjs` の `adaptor.unpremultiply=true`)。

```tjs
comp.unpremultiply = true;   // 以後の capture 読み戻しで un-premultiply
comp.capture(layer, function(w, h, param) { /* MSAA 3D 描画 */ }, param, 0x00000000);
```

実装は `GLCompositor::Capture` で `Canvas::Capture`(premultiplied のまま読み戻し)後に、
Layer メインイメージ (32bit ARGB) を **in-place で un-premultiply する後処理パス**
(`Canvas` 自体は汎用のまま変更しない)。旧 `GLESAdaptor` の読み戻しループ内変換と
同一アルゴリズム (`src/plugins/krkrgles/src/GLES.cpp`)。
