system_debug — デバッグ支援 (式の評価 / 例外 / ログ)
====================================================

「動いている状態をその場で覗く」手段と「落ちたときに情報を残す」手段を
1 画面にまとめたデモ。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

■ 1. 式の評価 (Scripts.eval + Debug.prettyPrint)

  パネルの入力欄に TJS2 の式を書いて「評価する」を押すと、その場で評価して
  結果を整形表示する。

    Scripts.eval(expr, name, line)   式を評価して値を返す。
                                     **中で起きた例外は握り潰されない**ので
                                     呼び出し側の try/catch で捕まえられる
    Debug.prettyPrint(v, depth, compact)
                                     任意の値を辞書/配列の中身ごと文字列化。
                                     depth 既定 2 / compact 既定 false

  初期値は `%[ name : "krkrz", ver : System.versionString, ok : true ]`。
  辞書がそのまま展開されて出るので、prettyPrint の使いどころが分かる。

■ 2. 例外オブジェクト

  「ゼロ除算 / 未定義 / throw」の 3 ボタンで例外を発生させ、try/catch で
  捕まえた例外オブジェクトの中身を表示する。

    e.message   エラーメッセージ (エンジンの例外は日本語メッセージ)
    e.trace     呼び出し履歴。**-debug 起動時のみ中身が入る**
                (デバッグモードが無効だと空文字列。
                 Scripts.getTraceString() も同じ条件)

  ※ TJS2 の組込例外クラスは `Exception` のみ。 種類で分けたい場合は
    message を見るか、`Exception` から派生した自前クラスを投げる。
  ※ 整数除算 `\` はゼロ除算で例外になるが、`/` は浮動小数除算なので
    例外にならない (inf になる)。

■ 3. 未捕捉例外 (System.exceptionHandler)

  「exceptionHandler を使う」を ON にすると、シーンが
  `System.exceptionHandler` を自前の関数へ差し替える。 この状態で
  「未捕捉例外を投げる」を押すと、one-shot タイマの中から投げた例外が
  どこにも捕捉されずに本体まで届き、ハンドラが呼ばれて画面に表示される。

    - ハンドラが **真を返すと**、本体の既定動作
      (System.eventDisabled = true / Debug.logAsError() /
       エラーダイアログ表示) は**行われない**
    - 偽を返す (またはハンドラ未設定) と既定動作になる

  そのため OFF のままでは「未捕捉例外を投げる」は実行しない
  (既定動作でエラーダイアログが出てデモが止まるため)。
  シーンを離れるとき (onExit) にハンドラは元へ戻す。

  ※ パネルの onAction から直接 throw するとダイアログ側で握られる可能性が
    あるので、one-shot タイマから投げている。

■ 4. エンジンのログ (Debug.addLoggingHandler)

  `Debug.addLoggingHandler(handler)` でエンジンのログ出力を関数で受け取り、
  最新 9 行を画面に流している。 起動直後の GL 初期化ログなどがそのまま
  流れてくるのが分かる。

    Debug.message(msg)   通常ログ
    Debug.notice(msg)    重要ログ
    Debug.getLastLog(n)  未出力ぶんのログを取得
    ※ ハンドラの中でログを出しても再帰呼び出しにはならない (本体側で無視)

  「ログを出す」ボタンで message / notice を 1 行ずつ出せる。
  シーンを離れるときに removeLoggingHandler する。

■ 関連リファレンス

  doc/reference/Scripts.md  eval / exec / evalStorage / getTraceString /
                            setCallMissing / getClassNames
  doc/reference/Debug.md    message / notice / prettyPrint /
                            addLoggingHandler / removeLoggingHandler /
                            getLastLog / startLogToFile / logAsError
  doc/reference/System.md   exceptionHandler / eventDisabled

■ メモ

  - REPL (-repl / -replfile) 起動中は、未捕捉例外でもエンジンがダイアログを
    出さずログに書くだけになる。 既定動作を確かめたいときは REPL 無しで起動する。
  - システム情報そのものの一覧は sysinfo デモ、描画/メモリの計測は
    perf_stats デモを参照。
