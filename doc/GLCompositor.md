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

## 未対応 (予定)

- 入口の一般化 (画像用途の Layer 引数を Bitmap or Layer 受けにする横断対応) は
  プラグイン対応含め別枠計画で扱う。
- 実素材での合成挙動のさらなる目視確認。
