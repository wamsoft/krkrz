text_font — フォントと文字描画
==============================

■ 概要

Layer.font (Font クラス) と drawText の機能を一覧で見せるデモ。

  サイズ         … font.height を変えた描画
  装飾           … bold / italic / underline / strikeout とその組み合わせ
  アンチエイリアス … drawText の aa 引数 (true / false) の比較
  影             … shadow (level / color / width / offset)
  文字幅計測     … getTextWidth / getTextHeight (計測値ぴったりの枠を描画)
  色指定の注意   … drawText の色は 0xRRGGBB (24bit)。0xffffffff のように上位
                   8bit を立てるとシステムカラー扱いで黒化する実演
  フォント一覧   … Font.getList で列挙したフォントを各 face で描画

■ 実装メモ

- drawText の色は必ず 6桁 0xRRGGBB。上位 8bit が 0 以外だと TVPToActualColor
  でシステムカラー番号として解釈され意図しない色になる (fillRect 等は
  0xAARRGGBB で別扱い)。→ doc/reference/Layer.md の drawText 注記参照。
- 装飾は Font のプロパティ (bold/italic/underline/strikeout) を都度 reset して
  から設定する (前の描画の設定が残らないように resetFont を用意)。
- Font.getList はプラットフォームによっては列挙数が少ない / 0 のことがある
  (その場合は「列挙が使えません」と表示)。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/text_font

■ 操作

  ESC : 終了

■ ヘッドレステスト

  krkrz64.exe src/core/data/text_font -demotest
  → "@demotest:text_font faces=N" と "@demotest:ok"

■ 対応ドキュメント

  doc/reference/Font.md / doc/reference/Layer.md (drawText / getTextWidth)
