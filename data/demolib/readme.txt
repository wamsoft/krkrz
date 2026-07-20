demolib — krkrz デモ共通ヘルパ
==============================

各デモ (data dir として起動される 1 フォルダ) から共通で使うヘルパ集。
SSOT はここ (src/core/data/demolib)。umbrella (krkrz_dev) の data/ 直下の
デモや、krkrz_web でステージングされた Web 版デモからも相対パスで参照する。

■ 読み込み方 (各デモの startup.tjs 冒頭に置くブロック)

  // --- demolib 読み込み -----------------------------------------------
  var demolibPath = void;
  {
      // デモの置き場所ごとに demolib の相対位置が違うので順に探す:
      //   ../demolib/                    … core デモ (src/core/data/<demo>)
      //                                    および Web 合成後の core/<demo>
      //   ../core/demolib/               … umbrella デモ (Web 合成後)
      //   ../../src/core/data/demolib/   … umbrella デモ (デスクトップ)
      var cands = ["../demolib/", "../core/demolib/", "../../src/core/data/demolib/"];
      for (var i = 0; i < cands.count; i++) {
          if (Storages.isExistentStorage(cands[i] + "demo_common.tjs")) {
              demolibPath = cands[i];
              break;
          }
      }
  }
  if (demolibPath === void) {
      System.inform("demolib/demo_common.tjs が見つかりません。\n"
                    + "リポジトリ配置のまま実行してください。");
      System.exit(1);
  }
  Scripts.execStorage(demolibPath + "demo_common.tjs");
  // ----------------------------------------------------------------------

■ 提供 API

  demo_common.tjs 冒頭のコメントを参照。概要:
    dm / nf / tryPlugin / hasElements
    DemoWindow (stage / setInfo / enableFps / showPanelJson / showPanelDict /
                onPanelAction / onDemoFrame / onDemoTest)
    DemoPanel  (Elements Dialog が使える環境でのみ定義)

  重要: デモの描画は必ず DemoWindow.stage (とその子レイヤ) に対して行う。
  primary レイヤへの直接 drawText と primary 直下の ltAlpha レイヤは、
  現行エンジンの既知の描画問題 (グリフ崩れ / 合成異常) を踏む
  (2026-07-20 時点、SDL ビルドで確認。修正されるまでの運用規約)。

■ ヘッドレス自動テスト

  krkrz64.exe <demo_dir> -demotest
  で起動すると DemoWindow は 60 フレーム後に "@demotest:ok" を出力して
  終了する。CI や実機確認の自動化に使う。

■ 注意

  - このフォルダを編集したら、依存する全デモ (core / umbrella) の動作を
    確認すること。互換を壊す変更は DEMOLIB_VERSION を上げる。
  - Elements (Dialog) が無いビルド (WINVER 等) でも読み込み自体は成功する
    ように保つこと (DemoPanel は条件付き定義)。
