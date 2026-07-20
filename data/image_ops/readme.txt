image_ops — 画像処理 (Bitmap 演算)
==================================

■ 概要

1 枚のソース画像に各種の画像処理を施した結果を 3×3 グリッドで比較するデモ。

  doGrayScale        … グレースケール化
  adjustGamma        … ガンマ補正 (明 2.0 / 暗 0.45)
  flipLR / flipUD    … 左右 / 上下反転
  doBoxBlur          … ボックスぼかし
  affineCopy         … アフィン変換 (回転、3 隅指定)
  colorRect          … 半透明色の掛け合わせ (色掛け)

■ 実装メモ

- doGrayScale / adjustGamma / doBoxBlur / flip* は「レイヤ全体」に効くため、
  セルごとに独立レイヤへソースを copyRect してから適用する。
- 各セルは stage 直下の子レイヤ (ltOpaque)。非全画面でも直下の子なら
  正しく表示される (孫レイヤに Layer.type ブレンドを設定すると黒くなる件は
  layer_basic の readme 参照。ここは合成タイプを使わないので無関係)。
- affineCopy(src, sl,st,sw,sh, affine=false, x0,y0, x1,y1, x2,y2, type, clear)
  は 3 隅 (TL/TR/BL) の転送先座標で指定する。回転は中心基準で 3 隅を回して
  与える (右下は自動計算)。※ 入れ子関数は外側 local を参照できないので座標計算は
  インラインで書く。
- R キーでソースを再生成 (円の配置がランダムに変わる)。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/image_ops

■ 操作

  R   : ソース再生成
  ESC : 終了

■ ヘッドレステスト

  krkrz64.exe src/core/data/image_ops -demotest
  → "@demotest:image_ops cells=9" と "@demotest:ok"

■ 対応ドキュメント

  doc/reference/Layer.md (doGrayScale / adjustGamma / flipLR / flipUD /
  doBoxBlur / affineCopy / colorRect / copyRect)
