text_font — フォントと文字描画
==============================

■ 概要

フォントと文字描画のデモ 4 シーンを束ねたもの (PgUp/PgDn で切替)。

1. text_font — Layer.font (Font クラス) と drawText の機能一覧

  サイズ         … font.height を変えた描画
  装飾           … bold / italic / underline / strikeout とその組み合わせ
  アンチエイリアス … drawText の aa 引数 (true / false) の比較
  影             … shadow (level / color / width / offset)
  文字幅計測     … getTextWidth / getTextHeight (計測値ぴったりの枠を描画)
  色指定の注意   … drawText の色は 0xRRGGBB (24bit)。0xffffffff のように上位
                   8bit を立てるとシステムカラー扱いで黒化する実演
  フォント一覧   … Font.getList で列挙したフォントを各 face で描画

2. text_emoji — 絵文字レンダリング (Font.emojiMode / rasterizer 切替)

3. text_glyphware — 多言語シェイピング描画 (drawShapedText 系)

  混植/RTL       … drawShapedText によるラテン+日本語、アラビア語 (連結)、
                   ヘブライ語、BiDi 混在の 1 行描画。font は Font オブジェクト
                   1 個で渡す (face カンマ区切り = フォールバック連鎖)
  計測           … measureShapedText (インク境界+ベースライン可視化)
  矩形内折り返し … drawShapedTextArea (ワード/CJK 文字単位 + 簡易禁則)
  タイプライタ   … shapedTextCount + count 制限の自動再生。RTL 混在文が
                   論理順 (読み順) で 1 クラスタずつ現れる様子を観察できる

4. text_rasterizer — 同一内容の FreeType / GDI / glyphware 描画比較

■ 実装メモ

- drawText の色は必ず 6桁 0xRRGGBB。上位 8bit が 0 以外だと TVPToActualColor
  でシステムカラー番号として解釈され意図しない色になる (fillRect 等は
  0xAARRGGBB で別扱い)。→ doc/reference/Layer.md の drawText 注記参照。
- 装飾は Font のプロパティ (bold/italic/underline/strikeout) を都度 reset して
  から設定する (前の描画の設定が残らないように resetFont を用意)。
- Font.getList はプラットフォームによっては列挙数が少ない / 0 のことがある
  (その場合は「列挙が使えません」と表示)。
- text_glyphware の RTL/絵文字フォント (Noto Sans Arabic / Hebrew /
  Color Emoji) は data/fonts.json の宣言前提。単体起動 (datadir=text_font)
  では解決されず代替字形になる — フル表示はトップ data/ ギャラリーから。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/text_font

■ 操作

  ESC : 終了

■ ヘッドレステスト

  krkrz64.exe src/core/data/text_font -demotest
  → "@demotest:text_font faces=N" と "@demotest:ok"

■ 対応ドキュメント

  doc/reference/Font.md / doc/reference/Layer.md
    (drawText / getTextWidth / drawShapedText / drawShapedTextArea /
     measureShapedText / shapedTextCount)
  doc/guide/FontSystem.md (フォントシステム全体のガイド)
