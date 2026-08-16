elements_bench — Elements overlay 負荷計測
==========================================

■ 概要

Elements overlay (Dialog) の描画コストを、更新パターン別のシナリオを
切り替えながら Dialog.renderStats (描画パイプラインの区間計測) で
数値確認するベンチマーク画面。renderCache の効果確認と、部分再描画
(ダーティ矩形化) の要否判断・効果測定に使う。

■ シナリオ (数字キーで切替)

  1: idle       … 静的パネル。renderCache 有効なら rasters/s = 0
  2: caret      … input_box focus 中のキャレット点滅。約 500ms ごとに
                   パネル全面を再ラスタ (部分再描画の本命ユースケース)
  3: counter    … 毎フレーム setVar で HUD の数値 1 個だけ更新。
                   変化は小さいが現状は全面再ラスタになる
  4: anim 小    … 小パネル内の無限 yoyo アニメ (毎フレーム dirty)
  5: anim 広域  … 大パネル (760x560) 全面が毎フレーム再ラスタ
  6: 複合       … counter + idle + caret の 3 パネル同時
  0: なし       … ベースライン (overlay 無し)

  R: renderCache ON/OFF (A/B 比較)   P: partialRedraw ON/OFF
  C: 計測リセット
  ※ シナリオ切替キーはホストホットキー登録なので、caret パネルが
     focus を持っていても効く (テキスト入力中も有効)。

  パッド (キーボードの無い実機向け):
  十字←→ = シナリオ巡回   X = renderCache   Y = 計測リセット

■ 自動計測モード (-benchauto)

  krkrz64.exe src/core/data/elements_bench -benchauto
  (NX: make run RUNOPT="... -benchauto" 等、アプリ引数に付ける)

  入力なしで scenario 1/3/4/5/6 × renderCache ON/OFF を各 3 秒計測し、
  結果を "@bench cache=.. partial=.. scen=.. parts=.. fps=.. r_s=..
  tot_f=.. ras_f=.. acq_f=.. upl_f=.. pre_f=.. share=.." 行でログに出力し
  自動終了する。RunOnTarget 等のログ回収だけで実機計測が完結する。
  時間値は us、*_f = 1 フレーム平均、ras_r = ラスタ 1 回平均、
  parts = 部分再描画できた回数、share = CPU 占有率 (%)。

  -benchcaret を併せて指定すると、末尾に caret の partialRedraw ON/OFF
  (部分再描画の A/B) を追加する。caret は click しないと点滅が始まらない
  ため Agent で入力欄へクリックを注入するが、**テキスト欄に focus すると
  Switch / Android ではソフトウェアキーボード (OS ダイアログ) が開いて
  画面が覆われ計測にならない**。デスクトップでのみ使うこと。

■ 計測結果の見方 (500ms ごと更新)

  フレーム       … PaintOverlay 呼出回数と fps
  ラスタ         … render_to_buffer 実行回数 (ThorVG CPU ラスタ)
  cached 提示    … renderCache によるラスタ省略提示の回数
  1 フレーム平均 … total / update / raster / acquire / upload / present (us)
  Elements CPU 占有率 … PaintOverlay 総時間の実時間比 (%)

  典型的な確認ポイント:
  - idle で rasters/s = 0 → renderCache が効いている
  - R で OFF にすると idle でも毎フレームラスタが走り、占有率が跳ねる
  - caret/counter は「変化は小さいのに全面再ラスタ」= 部分再描画の
    改善余地。導入後は raster/upload の減少をここで確認する

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe  src/core/data/elements_bench
  (トップ data/ ギャラリー・core gallery からも選択可)

■ 対応ドキュメント

  doc/ElementsDialog.md (renderCache / renderStats)
  doc/reference/Dialog.md
