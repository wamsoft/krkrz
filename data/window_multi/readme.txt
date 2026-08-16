window_multi — 複数ウィンドウ / モーダル / 画面情報
====================================================

Window を 2 枚以上開いたときの振る舞いと、ウィンドウ属性 (位置・サイズ・
枠スタイル・最前面・表示倍率・フルスクリーン)、画面情報 (解像度・DPI) を
1 画面で試すデモ。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

■ 1. サブウィンドウ (非モーダル)

  「サブを開く」で SubWindow (Window 派生) を生成する。 サブウィンドウは
  独立した primaryLayer とレイヤツリーを持ち、親と同時に操作できる。
  ウィンドウ上をクリック / ESC / × で閉じる。

  右の情報欄に位置・サイズ (外側 = 装飾込み / inner = クライアント)、
  枠スタイル、最前面、表示倍率、フルスクリーン状態が出る。

■ 2. モーダル表示 (showModal)

  「モーダルで開く」は showModal() を呼ぶ。 これは **そのウィンドウが閉じる
  まで戻らない** ブロッキング呼出で、戻ってきた後に情報欄へ「閉じるまで
  N ms ブロックした」と表示される。

  モーダル中はネストしたイベントループが回るので、描画・タイマー・REPL は
  動き続ける。 止まるのは他ウィンドウへのユーザ入力だけ。

  制約:
    - 呼び出す時点でウィンドウは非表示でなければならない
    - ウィンドウが 1 つしか無い状態では例外になる
    - ウィンドウを複数作れない環境 (モバイル / コンソール) では 2 枚目の
      生成自体が失敗するため、そもそも呼ばない作りにする

■ 3. ウィンドウ属性

  borderStyle (0:bsNone 1:bsSingle 2:bsSizeable 3:bsDialog
               4:bsToolWindow 5:bsSizeToolWin)
      枠の有無とリサイズ可否。 SDL ビルドでは「枠あり/なし」と
      「リサイズ可/不可」の組み合わせへ写像される。
  stayOnTop   常に最前面。
  表示倍率     setZoom(numer, denom)。 SDL / generic では「レイヤサイズ×倍率」を
              ウィンドウの内側サイズにする (ウィンドウがリサイズされ、中身が
              拡縮される)。 WINVER は DestRect 計算にしか倍率を使わないため、
              windowed では見た目が変わらない (TODO.md 参照)。
  全画面切替   fullScreen プロパティ。

■ 4. 画面情報

  System.screenWidth / screenHeight    画面 (ディスプレイ) の解像度
  Window.displayDensity                DPI (96 = 100%)
  System.desktop*                      WINVER 限定。 SDL ビルドには無いので
                                       typeof で存在を見てから読むこと。

■ 5. 画面キャプチャ

  「画面を PNG 保存」で System.captureScreen を呼ぶ。 overlay (Elements
  パネル) 込みの実画面が保存先 (System.dataPath = -datapath 指定の場所、
  既定は savedata/) に書き出される。

■ 関連リファレンス

  doc/reference/Window.md
    showModal / borderStyle / stayOnTop / setZoom / zoomNumer / zoomDenom /
    fullScreen / setPos / setSize / setInnerSize / displayDensity
  doc/reference/System.md
    screenWidth / screenHeight / captureScreen / dataPath

■ メモ

  - サブウィンドウも描画は primaryLayer 直下の不透明 stage に対して行う
    (primary へ直接描くと現行エンジンの既知の描画問題を踏む)。
  - onCloseQuery は既定ハンドラへ true を渡さないと閉じられない。
  - showModal 中は REPL のファイルチャネルも 1 コマンド分ブロックしたまま
    になる (コマンドの実行が終わらないため)。 自動テストから閉じたい場合は
    Timer から close() する。
