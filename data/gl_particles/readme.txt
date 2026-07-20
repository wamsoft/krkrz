gl_particles — Canvas によるステートレス GPU パーティクル
========================================================

■ 概要

VertexBuffer に「放出位置・初速・誕生時刻・種」を焼き込み、位置はシェーダが
時刻からパラメトリックに計算する「ステートレス」方式の GPU パーティクル。
TJS 側の毎フレーム更新が無いので数万粒子でも軽い (実測 50000 粒子で ~55fps)。
ポイントスプライト (GL_POINTS + gl_PointCoord) で 1 粒子 = 1 頂点。

  粒子ごとの頂点属性 (interleave, 6 float = 24 byte):
    a_origin (vec2) … 放出位置
    a_vel    (vec2) … 初速
    a_birth  (float)… 誕生時刻オフセット (寿命内で位相をずらし連続放出に)
    a_seed   (float)… 色/サイズ変化用の乱数

  頂点シェーダが age = mod(u_time + a_birth, u_life) から現在位置
  (origin + vel*age + 0.5*gravity*age^2) と gl_PointSize を計算。
  フラグメントは gl_PointCoord で円形に整形し、寿命でフェード、種で色相を決める。

■ 操作

  パネル (Elements) : 粒子数 / エミッタ形状 / ブレンド を変更、一時停止
  SPACE : 一時停止 / 再開
  B     : ブレンド切替 (加算 bmAdd ⇔ 通常 bmAlpha)
  G     : エミッタ切替 (噴水 / 放射 / 上昇)
  ESC   : 終了
  ※ 右上に FPS を常時表示

■ 実装メモ

- drawDevice に OGLDrawDevice 派生を代入し、onDraw で
  canvas.drawMesh(shader, count, VertexBuffer.ptPoints) で一括描画。
- attribute は VertexBinder をシェーダのプロパティに設定 (描画後 void で解除)。
  uniform も同様にプロパティ設定。u_screen で ortho 射影を作る。
- 粒子バッファの再構築 (粒子数/エミッタ変更) は device.rebuild() を
  イベントハンドラから直接呼ぶ (VertexBuffer 生成はメインスレッドで実行)。
- FPS / 情報 HUD はレイヤに描き、onDraw 末尾で this.texture として合成。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/gl_particles

SDL3 + OpenGL ビルドが必要。

■ ヘッドレステスト

  krkrz64.exe src/core/data/gl_particles -demotest
  → 途中で粒子数/エミッタを変えつつ "@demotest:gl_particles count=.. fps=.."
    と "@demotest:ok" を出力して終了

■ 対応ドキュメント

  doc/reference/Canvas.md (drawMesh) / ShaderProgram.md /
  VertexBuffer.md / VertexBinder.md / OGLDrawDevice.md
