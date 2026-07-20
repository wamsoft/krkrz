sysinfo — システム情報表示デモ
==============================

■ 概要

System / Storages の主要な情報 (バージョン・ビルドバリアント・パス類・
画面情報など) を一覧表示する最小のデモ。demolib (デモ共通ヘルパ) の
動作確認を兼ねる。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/sysinfo

Web (krkrz_web) ではランチャの「システム情報」から起動。

■ 操作

  ESC : 終了
  P   : Elements パネル再表示 (SDL3 ビルド + KRKRZ_USE_ELEMENTS=ON のみ)
  F   : FPS 表示
  パネル: 情報再取得 / パネルを閉じる / デモ終了

■ ヘッドレステスト

  krkrz64.exe src/core/data/sysinfo -demotest
  → 60 フレーム後に "@demotest:sysinfo props=..." と "@demotest:ok" を
    出力して終了する。

■ 対応ドキュメント

  doc/reference/System.md / doc/reference/Storages.md
