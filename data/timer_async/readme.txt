timer_async — タイマーと非同期イベントのタイミング可視化
========================================================

■ 概要

Timer / AsyncTrigger / 連続ハンドラ (onContinuous 相当) のタイミングを
可視化するデモ。

  Timer        … 異なる周期 (150/350/700/1500ms) の複数タイマー。拍
                インジケータ・次発火までの進捗バー・発火回数
  連続ハンドラ … 毎フレーム (demolib onDemoFrame) 駆動。getTickCount から
                2 秒周期で往復するマーカー、FPS 表示
  AsyncTrigger … cached=true。同一フレーム内で trigger() を複数回呼んでも
                onFire は 1 回に合流することを実演 (trigger 回数 vs onFire 回数)

■ 実装メモ

- Timer は owner + actionname で受ける。4 本を区別するため onT0〜onT3 の
  別メソッドで受け、共通の fired(i) に流す。
- AsyncTrigger は cached=true にすると、1 回のイベントディスパッチ内の複数
  trigger() が 1 回の onFire にまとめられる (負荷の高い処理を「次の機会に
  1 回だけ」実行したいときの定石)。パネルの「5回同時」で trigger +5 / onFire
  +1 になるのを確認できる。
- 連続ハンドラは demolib の onDemoFrame (System.addContinuousHandler 経由)。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/timer_async

■ 操作

  パネル : AsyncTrigger を発火 (1回 / 5回同時 / リセット)
  ESC    : 終了

■ ヘッドレステスト

  krkrz64.exe src/core/data/timer_async -demotest
  → "@demotest:timer_async timers=4 counts=[..]" と "@demotest:ok"

■ 対応ドキュメント

  doc/reference/Timer.md / AsyncTrigger.md / System.md (getTickCount /
  addContinuousHandler)
