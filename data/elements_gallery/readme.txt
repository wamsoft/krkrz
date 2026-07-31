elements_gallery — Elements ウィジェットギャラリー
==================================================

■ 概要

Elements (Dialog クラス) の主要ウィジェットを「実物 + 定義ソース +
イベントログ」の 3 点セットで確認できるカタログデモ。

- 左: ギャラリーダイアログ。上部の segmented_picker でページ切替
- 右: 表示中ページの定義ソース (TJS Dictionary)。そのまま写して使える
  (pages.tjs の行配列を表示し、同じものを eval してレイアウトに使って
  いるので、表示と実物は常に一致する)
- 下: onAction(id, payload) のイベントログ (直近 3 件)

レイアウトは Dialog.showDict (TJS Dictionary レイアウト) で表示している。

■ ページ構成 (順次追加中)

  基本    : label / button / toggle_button / check_box / radio_button /
            slide_switch
  値入力  : slider / slider_with_range / input_box
  (予定)  : 選択 (cycle・segmented picker / selection_menu) / レイアウト
            (tile / margin / align / scroller / tab_view) / atlas 系 /
            アニメ (animate) / テーマ (vars)

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/elements_gallery

KRKRZ_USE_ELEMENTS=ON が必要 (SDL3 / WINVER 両ビルドで動作する。
KRKRZ_USE_ELEMENTS=OFF ビルドでは Dialog クラスが無い旨を表示して待機する)。

■ 操作

  ページ切替 : ダイアログ上部のピッカー
  ホイール   : ソースパネルのスクロール
  ESC        : 終了

■ ヘッドレステスト

  krkrz64.exe src/core/data/elements_gallery -demotest
  → "@demotest:gallery pages=N panel=1 ..." と "@demotest:ok" を出力して終了

■ 対応ドキュメント

  doc/reference/Dialog.md (showDict / onAction)
  ウィジェット/属性の詳細: elements_modal の json_layout
