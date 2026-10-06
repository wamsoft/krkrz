a11y — 読み上げ (スクリーンリーダー対応)
========================================

ゲーム画面を OS のスクリーンリーダー (Windows のナレーター / macOS の
VoiceOver / Linux の Orca) から読めるようにする口を 1 画面で試すデモ。
Elements のダイアログは何もしなくても読まれる。このデモでは、それ以外の
「Layer に描いた UI」を読ませる 2 つの方法と、任意の文を読ませる announce を扱う。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

操作:
  ↑ ↓          選択肢を選ぶ
  Enter / クリック 決定 (決めた結果を announce で読ませる)
  ← →          選択肢 → ボタン → … とフォーカスを移す (音量の上では値を変える)
  Tab          右下の操作パネルへ (パネルの中は Tab / 矢印で移動)

  読み上げを聞くには、デモを起動してからナレーター (Win + Ctrl + Enter) などを
  起動し、ウィンドウを前面にする。スクリーンリーダーが繋がると右の状態表示が
  「接続中」になる。

■ 1. Layer に描いた選択肢 (ElementsDialog.setGameA11y)

  選択肢は 1 枚の Layer に描いているだけなので、そのままでは読み上げツリーに
  何も出ない。スクリプトがノードの表 (list と list_item、各項目の名前と矩形)
  を setGameA11y に渡して載せる。

  - 選択が変わるたびに setGameA11y を呼び直す (表は丸ごと差し替え。差分は本体が取る)
  - 第 2 引数の focus は、選択肢のレイヤにフォーカスがある間だけ渡す
    (渡している間はスクリーンリーダーのフォーカスがそちらに固定される)
  - スクリーンリーダーからの操作は ElementsDialog.onGameA11yAction に届く。
    本体はフォーカスを動かさないので、focus / click を受けたらゲーム側の
    選択を変えて setGameA11y を呼び直す
  - 選択肢のレイヤ自身は a11yHidden = true で自動 (下の 2) から外している

■ 2. フォーカス連鎖の Layer を自動で載せる (ElementsDialog.a11yLayers)

  a11yLayers = true にすると、focusable な Layer が読み上げツリーに載る。

  - セーブ / ロード / タイトルへ … 名前は hint。click は既定の処理
    (フォーカスして Enter) で届くので、Enter で押せる作りにしておけば足りる
  - 見出し … フォーカスできないが a11yName + a11yRole = "heading" で載る
  - オートモード … a11yRole = "check_box"。状態は Layer の checked メンバから読まれる
  - 音量 … a11yRole = "slider"、a11yValue (property) が値。値の変更
    (increment / decrement / set_value) は onA11yAction で受ける
  - 飾り … a11yName を持っていても a11yHidden = true なら読まれない
  - Layer.name が読み上げツリーの id (layer:<name>) になる

  クラスで a11yName などを持たせるときは var で宣言すること
  (宣言せずにメソッド内で代入すると「メンバが見つかりません」になる)。

■ 3. announce と画面 JSON の "a11y" キー (右下パネル)

  - ElementsDialog.announce(text, assertive) で任意の文を読ませる
  - Hello / Time のボタンは、表示の文字と別の名前を "a11y" で付けている
    ("a11y": "名前" / "a11y": %[ label, description ])
  - 見出しは "a11y": %[ role : "heading" ]、区切り線は "a11y": %[ hidden : true ]

■ REPL での確認

  krkrz64.exe <このフォルダ> -replfile=<dir> で起動すると、スクリーンリーダー
  無しでも次の口で確かめられる (右の「読み上げログ」にも出る)。

    .a11y                         読み上げツリー (JSON)
    .a11ylog                      読み上げログ (おおよそ何と読むか)
    .a11ydo choice1 click         選択肢「南へ行く」を決定
    .a11ydo layer:volume set_value 30
    .say こんにちは                announce

■ 関連ドキュメント

  doc/specification/accessibility.md (umbrella)   仕様の全体
  doc/guide/Accessibility.md (umbrella)           使い方のガイド
  doc/reference/ElementsDialog.md                 announce / setGameA11y / a11yLayers ほか
  doc/reference/Layer.md                          Layer に生やすメンバ (クラス説明)
  doc/reference/Agent.md                          a11yTree / a11yLog / a11yAction

■ メモ

  - Web (wasm) 版には読み上げの口が無い。
  - 読み上げの設定 (setGameA11y / a11yLayers / onGameA11yAction) はクラス全体に
    効くので、このデモはシーンを抜けるときに元に戻している (onExit)。
