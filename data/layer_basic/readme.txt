layer_basic — 合成モード (ブレンド) と不透明度
==============================================

■ 概要

同じ背景の上に同じ「明るいレインボーグラデーション」を、異なる合成モードで
重ねた結果を 3×3 のグリッドで並べて比較するデモ。Elements パネルで不透明度
(opacity) を変えると全サンプルに反映される。

  omOpaque / omAlpha / omAdditive / omSubtractive / omMultiplicative /
  omScreen / omDodge / omDarken / omLighten

■ 実装メモ (重要)

合成は Layer.operateRect(dx,dy, src, sx,sy,sw,sh, mode, opa) を使う。
operateRect は転送先レイヤの**ビットマップ上で CPU 合成**するため、描画
デバイスに依存せず、どの合成モードも正しく再現できる。

  ※ 当初は「子レイヤの Layer.type にブレンドタイプを設定して重ねる」方式で
    作ったが、非全画面の入れ子 (孫) レイヤに ltMultiplicative / ltDodge 等
    (さらには ltOpaque / ltAlpha も) を設定すると、そのセルが黒くなる現象を
    確認した (SDLOGL / SDL 両デバイスで同一、face 設定でも変わらず)。
    現行のレイヤツリー合成器の制約と思われる。ブレンドの比較には
    レイヤツリー合成 (Layer.type) ではなく operateRect (演算モード omXXX) を
    使うのが確実。→ engine-layer-blend メモ参照。

描画は 1 枚の全画面 opaque レイヤ (display) に集約し、各セルの背景と
operateRect の結果、ラベルを全てそこへ描く (レイヤツリーには 1 枚だけ)。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/layer_basic

■ 操作

  パネル : 不透明度 (opacity 0〜255) を変更
  ESC    : 終了

■ ヘッドレステスト

  krkrz64.exe src/core/data/layer_basic -demotest
  → "@demotest:layer_basic modes=9 opacity=.." と "@demotest:ok"

■ 対応ドキュメント

  doc/reference/Layer.md (operateRect / type / 合成モード omXXX)
