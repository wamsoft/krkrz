input — 入力イベントの可視化
============================

■ 概要

Window の入力イベントをリアルタイムに可視化するデモ。

  キーボード … onKeyDown / onKeyUp。VK 名 + コード、修飾キー
               (Shift/Ctrl/Alt/Repeat) の状態、直近のキー履歴
  マウス     … onMouseDown/Up/Move/Wheel。座標・ボタン (L/M/R)・
               ホイール累積・ドラッグ軌跡 (十字カーソル付き)
  ゲームパッド … System.padAxisLeftX/Y・RightX/Y・LeftTrigger/RightTrigger を
               毎フレーム読んでスティック / トリガを表示

■ 実装メモ

- キー名は主要な VK コードの対応表を持ち、無いコードは 0xNN 表示に
  フォールバック (A-Z は文字コード → 文字の $ 演算子で生成)。
- マウスイベントの座標は Window 座標系。表示レイヤ内の相対座標へ補正して
  十字カーソルを描く。
- パッド軸はイベントが無いので onDemoFrame (毎フレーム) で System.padAxis* を
  ポーリング。生値が [-1,1] を外れることがあるため、ノブ表示は枠内にクランプし、
  数値はそのまま出す。
- 描画は stage 下の子レイヤ (kb / ms / pad) に分け、各イベントで該当部だけ
  再描画する。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/input

■ 操作

  実際にキー / マウス / パッドを操作して確認。ESC で終了。

■ ヘッドレステスト

  krkrz64.exe src/core/data/input -demotest
  → postInputEvent で疑似キー入力を注入しつつ
    "@demotest:input ..." と "@demotest:ok" を出力して終了

■ 対応ドキュメント

  doc/reference/Window.md (onKeyDown/onMouseMove/onMouseWheel/postInputEvent 等)
  doc/reference/System.md (padAxis* / hasJoypad)
