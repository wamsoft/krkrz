elements_audit — Elements 仕様精査の確認サンプル
================================================

■ 概要

doc/ElementsAudit.md で挙げた 6 項目を、修正の前後で同じ手順で確認するための
シナリオ集。各シナリオは「現状はこうなる / 修正後はこうなるはず」を画面上の
数値で見られるようにしてあり、機械可読の "@audit ..." 行もログへ出す。

修正に入る前にまず現状値を採り (before)、修正後に同じ操作で採り直して
(after) 比べる、という使い方を想定している。

■ シナリオ (数字キーで切替)

  1: warp echo   … cursor-warp グリッド (4x4)。十字/矢印の長押しでフォーカスを
                   回し、「フォーカス往復 (振動)」の回数を数える。
                   Z/X でウィンドウ倍率を変えて present_scale != 1 にできる。
                   → ElementsAudit.md §1
  2: list 描画   … 多行 list + 離れた位置のキャレット点滅。renderStats の
                   ラスタ時間を表示。+/- で行数を変えて「行数に比例して
                   増えるか」を見る。
                   → ElementsAudit.md §2
  3: atlas 常駐  … アトラスを持つ画面を開閉して常駐量の推移を見る。
                   要 -genassets (下記)。
                   → ElementsAudit.md §3
  4: metrics     … 毎フレーム異なる文字列を描き、text_metrics キャッシュの
                   全 clear によるスパイクを検出する。
                   → ElementsAudit.md §4
  5: 押しっぱなし … 十字を押したまま上に画面を開く/閉じる。背面のフォーカスが
                   動き続けていないかを数える。
                   → ElementsAudit.md §5
  6: リピート周期 … 長押し中のフォーカス送り間隔を実測 (ms)。
                   -paddelay / -padinterval の効きを確認する。
                   → ElementsAudit.md §6

  0: なし   C: 計測リセット   Z/X: ウィンドウ倍率 -/+   +/-: 行数 (シナリオ 2)

  ※ シナリオ切替キーはホストホットキー登録なので、パネルが focus を
     持っていても効く。

■ 実行方法 (umbrella ルートから)

  bin/<preset>/<config>/krkrz64.exe src/core/data/elements_audit

  入力系の調査では -navlog を併用する:
  bin/<preset>/<config>/krkrz64.exe src/core/data/elements_audit -navlog

  リピート周期の確認 (シナリオ 6):
  ... src/core/data/elements_audit -paddelay=1000 -padinterval=200

■ 資材の生成 (シナリオ 3 を使う場合のみ)

  krkrz64.exe src/core/data/elements_audit -genassets

  image/ へ大きめのアトラス PNG を 8 枚生成して終了する (リポジトリには
  バイナリを置かない方針のため、使う人が 1 回だけ走らせる)。生成済みなら
  スキップされる。シナリオ 3 は資材が無ければその旨を表示するだけ。

■ 実画面の保存 (-auditshot=<dir>)

  ... -audittest -auditshot=C:/path/to/dir

  各シナリオの区間末 (の手前) で実画面を PNG 保存する。描画経路を触ったときの
  目視確認用。ファイル名は audit_s<シナリオ>_r<行数>_<canvas|vtile>.png。
  captureScreen は「次フレームの present 込み」で効くので、区間の最後ではなく
  少し手前で投げている (最後に投げると次シナリオの画面が撮れてしまう)。

■ シナリオ 1 を測るときの注意 (重要)

  cursor-warp は **仮想カーソル位置** を動かす方式になったので (2026-09-12、
  doc/VirtualCursor.md)、テスト中に実ポインタが飛ぶことはなくなった。

  測定中に人がマウスをウィンドウ上で動かすとナビ種別が mouse へ倒れ、warp の
  発火条件 (直近のナビ入力がキー/パッド) から外れて warp が起きなくなる。
  これを避けるには **-ignoremouse=yes を付ける** — 実マウスの入力を捨てて
  Agent の注入だけを通すので、ポインタを触っていても測定が汚れない。

    ... -audittest -ignoremouse=yes

  付けない場合はマウスから手を離した状態で走らせること。

  ※ 画面表示の "実マウス移動" カウンタは弱い指標でしかない (パネル表示中は
     オーバーレイがマウス移動を消費してシーンまで届かない)。

■ 判定の目安

  シナリオ 1: 倍率を変えても「振動」が 0 のままなら OK。
              現状は present_scale != 1 で振動が増える (ログに
              "warp guard: MISMATCH" と "nav_source key -> mouse" が出る)
  シナリオ 2: 行数を 8 -> 64 に増やしてもラスタ時間が横ばいなら OK。
              現状は行数に比例して増える
  シナリオ 3: 開く -> 閉じる -> trimAtlasCache(0) を自動で踏み、@audit 行へ
              atlas_open / atlas_closed / atlas_trimmed / atlas_freed を出す。
              「閉じただけでは減らない (atlas_closed = atlas_open)」「trim で
              落ちる (atlas_trimmed = 0)」が確認できれば OK。要 -genassets
  シナリオ 4: update 区間に周期的なスパイクが出なければ OK
  シナリオ 5: 背面フォーカス移動が 0 なら OK
  シナリオ 6: -paddelay/-padinterval を変えて送り間隔が追従すれば OK。
              現状は CLI を変えても 400ms/60ms のまま

■ 対応ドキュメント

  doc/ElementsAudit.md    (精査記録 = SSOT。各項の詳細と直し方)
  doc/ElementsDialog.md   (renderStats / renderCache / partialRedraw)
  doc/Gamepad.md          (パッド入力とリピート)
