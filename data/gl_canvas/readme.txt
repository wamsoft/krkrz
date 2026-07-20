gl_canvas — OpenGL / Canvas 基礎デモ
====================================

■ 概要

OGLDrawDevice に切り替え、Canvas クラスの機能をページ単位で見せる GL デモ。

  1. drawTexture + Matrix32 … テクスチャ描画とアフィン変換 (回転/拡縮/平行移動)
  2. カスタムシェーダ        … ShaderProgram でフラグメント加工 (波打ち)。
                               u_time / u_amp を時間で更新
  3. Offscreen (RTT)        … オフスクリーンに描いてから画面へ再描画 (2 パス)
  4. ポストエフェクト        … beginEffect/endEffect のコマンド
                               (grayscale / gamma / light / colorize / blur …)
  5. マスククリップ          … beginMaskClip/endMaskClip。円形アルファでくり抜き

■ 実装メモ

- drawDevice に OGLDrawDevice 派生を代入すると onInit / onDraw が呼ばれる。
  onDraw 内で canvas を直接操作して GL 描画する。
- ウィンドウのレイヤツリーは onDraw 内で this.texture (合成済みテクスチャ)
  として取れる。情報 HUD はレイヤに描いて最後に this.texture を重ねている。
- **カスタム頂点シェーダは drawTexture の規約に従う必要がある**:
  attribute a_pos / a_texCoord、uniform a_modelMat4 / a_size を宣言し、
  a_size で ortho 射影を作って gl_Position を計算する (既定頂点シェーダと
  同じ)。a_size が無いと "Not found a_size in shader" で描画時に例外になる。
  加工はフラグメントシェーダ側で行う。
- 情報テキストは primary 直下ではなく子レイヤに描く (エンジン既知の
  描画問題回避)。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/gl_canvas

SDL3 + OpenGL ビルドが必要 (OGLDrawDevice が無い環境では案内を表示)。

■ 操作

  ←→ / 1〜5 : ページ切替
  E         : ポストエフェクトのコマンド切替 (ページ 4)
  SPACE     : アニメーション一時停止
  ESC       : 終了

■ 素材の生成

  krkrz64.exe src/core/data/gl_canvas -genassets
  → image/testcard.png (テストカード) と image/mask.png (円形アルファ) を生成

■ ヘッドレステスト

  krkrz64.exe src/core/data/gl_canvas -demotest
  → "@demotest:gl_canvas ..." と "@demotest:ok" を出力して終了

■ 対応ドキュメント

  doc/reference/Canvas.md / ShaderProgram.md / Offscreen.md / Texture.md /
  Matrix32.md / OGLDrawDevice.md
  doc/topics/core/canvas_effect.md (エフェクト/クリップのコマンド一覧)
