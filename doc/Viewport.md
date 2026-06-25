# ゲーム画面の表示画角制御 (ビューポート)

SDL ビルドで、外側ウインドウ (surface) の中に内側ゲーム画面を任意のサイズ・
位置・倍率で配置し、周囲の余白を背景色や壁紙画像で埋める機能。

## 概念モデル

```
[外側 surface] = Window.innerWidth / innerHeight = OS ウインドウ client
        │  ビューポート設定 (fit / zoom / align / offset)
        ▼
[内側ゲーム]  = primaryLayer のサイズ (TJS から setSize で指定)
```

- **外側 surface** = `innerWidth/innerHeight`。Elements UI オーバレイもこの座標系。
  WINVER/SDL とも物理クライアント領域を指し、意味は共通。
- **内側ゲーム** = primaryLayer のサイズ。`setInnerSize()` とは独立に
  `primaryLayer.setSize(w, h)` で決まる。
- 両者を一致させれば従来どおり全面等倍。異なるサイズにすると、下記の設定に従って
  ゲームが surface 内へ配置され、余白が生じる。

マウス座標は `tTVPDrawDevice` の既存変換 (`TransformToPrimaryLayerManager` =
`x * layerW / DestRect幅`) でゲーム論理座標へ自動補正されるため、どの fit/zoom でも
正しくゲーム内部に届く。

## 対応状況

- **SDL ビルドのみ** (`__GENERIC__`)。3 つの DrawDevice すべてで動作:
  `tTVPSDLDrawDevice` (SDL_Renderer) / `tTVPSDLOGLDrawDevice` / `tTVPOGLDrawDevice` (GL)。
- **WINVER は据え置き** (従来の zoom ロック動作のまま、API は登録されない)。

## TJS API (`Window` のメンバー)

| メンバー | 型 | 説明 |
|---|---|---|
| `viewportFit` | string | フィット方式。下表参照。既定 `"contain"` |
| `viewportZoom` | real | `"custom"` 時の倍率 (例 1.8 = 180%)。既定 1.0 |
| `viewportAlignX` | real | 水平配置 0=左 / 0.5=中央 / 1=右。既定 0.5 |
| `viewportAlignY` | real | 垂直配置 0=上 / 0.5=中央 / 1=下。既定 0.5 |
| `viewportOffsetX` | int | 水平オフセット (px, surface 座標)。align 後に加算 |
| `viewportOffsetY` | int | 垂直オフセット (px)。 |
| `viewportBgColor` | int | 余白の背景色 `0xRRGGBB` (上位 8bit は alpha)。既定 黒 |
| `setViewport(fit [,zoom [,alignX [,alignY [,offsetX [,offsetY]]]]])` | method | まとめて設定 |
| `setViewportWallpaper(image [,fit="cover" [,alignX=0.5 [,alignY=0.5]]])` | method | 余白の壁紙画像を設定。`image` はストレージ名 (文字列) または `Layer` / `Bitmap` オブジェクト |
| `clearViewportWallpaper()` | method | 壁紙を解除 |

### fit 方式

| 値 | 動作 |
|---|---|
| `"contain"` | アスペクト維持で収まる最大 (letterbox)。**既定** |
| `"cover"` | アスペクト維持で埋める最小 (はみ出しは clip) |
| `"fill"` | アスペクト無視で surface 全面へ引き伸ばし |
| `"none"` | 原寸 (倍率 1.0) |
| `"integer"` | 収まる範囲で最大の整数倍 (最低 1 倍、ドット等倍維持) |
| `"custom"` | `viewportZoom` の倍率を使用 |

## 使用例

```tjs
var win = new Window();
win.setInnerSize(1280, 720);       // 外側ウインドウ

var lay = new Layer(win, null);    // primaryLayer
lay.setSize(640, 400);             // 内側ゲーム解像度
win.add(lay);

win.viewportBgColor = 0xff203060;  // 余白の色
win.viewportFit = "integer";       // 整数倍 (640x400 → 1280x800 は不可なので 1 倍)
// または 180% センタリング:
win.setViewport("custom", 1.8);
// 余白に壁紙 (ストレージ名でもオブジェクトでも可):
win.setViewportWallpaper("bg_pattern.png", "cover");
// Layer / Bitmap を直接渡すこともできる (参照保持される):
// win.setViewportWallpaper(myBitmap, "cover");
```

## 実装メモ

- 配置計算: `common/visual/ViewportConfig.h` の `TVPCalcViewportDestRect()` (純粋関数)。
  `TTVPWindowForm::CalcDestRect()` (generic/environ/WindowForm.cpp) がこれを使う。
- 設定の保持: `TTVPWindowForm` (配置 spec + 背景色 + 壁紙オブジェクトの `tTJSVariant`)。
- 余白描画: `iTVPDrawDevice::SetViewportBackgroundColor/SetViewportWallpaper`
  (既定 no-op、`tTVPDrawDevice` が保持)。各 DrawDevice の `Show()` が
  クリア色 + 壁紙を「ゲーム描画より前」に描く。GL 系は
  `common/visual/opengl/OGLViewportBackground.h` の共用ヘルパ。
- 橋渡し: `tTJSNI_Window` が TJS API → Form (配置) / DrawDevice (余白) を仲介。
  余白は変更時のみ `UpdateContent` で push (dirty flag)。
- **壁紙データは `tTJSVariant` のまま (Layer/Bitmap オブジェクト参照) で受け渡す。**
  DrawDevice は engine 内部型 (`tTVPBaseBitmap`) を直接受け取らない (DrawDevice は
  プラグインとして実装され得るため)。描画時に `tTVPDrawDevice::GetViewportWallpaperImage()`
  がオブジェクトの `imageWidth` / `imageHeight` / `mainImageBuffer` /
  `mainImageBufferPitch` プロパティ (Layer・Bitmap 共通) から画像イメージを取得する。
  文字列を渡した場合は `tTJSNI_Window` が `Bitmap` を生成してロードし、その
  オブジェクトを保持する。`Bitmap` には Layer と同名のプロパティ別名を追加済み
  (`BitmapIntf.cpp`)。
- 画像は ARGB8888 (メモリ上 B,G,R,A)。各 DrawDevice がテクスチャ化し、GL は
  BGRA→RGBA swizzle してアップロード。世代カウンタで再アップロードを最小化。

## 動作確認

`testdata_viewport/startup.tjs` が 640x400 ゲームを 1280x720 ウインドウ内に置くデモ。
数字キー 1-6 で fit 切替、W で壁紙、C で壁紙クリア。
