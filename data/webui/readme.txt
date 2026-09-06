webui — ブラウザ UI (内蔵 HTTP + SSE サーバ)
============================================

吉里吉里Z 本体は HTTP + SSE サーバ (`WebServer`) を内蔵している。
これを使って「ブラウザ側にツール UI を置き、走っているゲームを操作する」
仕組みを最小構成で組んだデモ。デバッグ用パネル、実機 (手元にキーボードが
無い環境) 向けの操作 UI、外部ツール連携などの土台になる。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。
  起動したらブラウザで http://127.0.0.1:8900/ui/ を開く
  (パネルの「ブラウザで開く」でも開ける)。

■ 1. サーバの起動

  WebServer.start(port)     127.0.0.1 で起動。戻り値は稼働状態
  WebServer.active / url    稼働状態と待受 URL
  WebServer.stop()          停止

  このデモは 8900 番を使う (`-replweb` の既定 8899 とぶつけないため)。
  `-replweb` で既にサーバが動いている場合は **start せずに相乗り**し、
  シーンを離れるときも stop しない (自分で起動したときだけ止める)。

  ※ `KRKRZ_REPL_WEB=OFF` ビルドには WebServer クラス自体が無い。
    デモは `typeof global.WebServer` で存在を見てから使う。

  ※ **組み込みルートは register / serveStatic より先に判定される**ので、
    次のパスは自前ハンドラで上書きできない。別の接頭辞を使うこと。

      /  /events  /cmd  /sub/<ch>  /watch  /state  /pad/exec  /pad/file  /bye

    (このデモは /ui/ と /api/ なので衝突しない)

  ※ `-replweb` で起動したサーバは **ブラウザが全部閉じると本体も終了する**
    (`-replwebidle`、既定 5 秒)。一方 **`WebServer.start()` で立てた場合は
    武装しない** — アプリが自分で管理しているサーバを勝手に落とさないため。

■ 2. ページの配信 (静的)

  WebServer.serveStatic("/ui/", <dir>)

  prefix 以下の GET を Storages 経由で配信する (".." は 403)。
  このデモは同梱の `web/index.html` を配信している。ページ側は素の
  HTML + fetch + EventSource だけで、外部リソースを一切使わない。

  配信元ディレクトリは単体起動 / gallery / hub のどこから呼ばれても
  見つかるよう、候補パスを順に `Storages.isExistentStorage` で探している
  (scene.tjs の findWebDir)。

■ 3. 動的エンドポイント

  WebServer.register(prefix, handler)

  handler(req) の req は %[ method, path, query, body, bytes ]。
  戻り値でレスポンスが決まる:

    文字列 → 200 application/json    オクテット → 200 octet-stream
    整数   → そのステータスで空ボディ  void → 204
    %[status, mime, body] → 指定どおり  例外 → 500

  このデモが登録しているもの:

    GET  /api/state     現在の状態を JSON で返す
    POST /api/message   body = 画面に出す文字列 (UTF-8)
    POST /api/color     body = "rrggbb"
    POST /api/bump      ?by=N でカウンタを増やす (既定 1)
    それ以外の /api/*   404

  ★ **ハンドラは必ずメインスレッドで実行される**ので、TJS の状態を
    そのまま読み書きしてよい (ロック不要)。

  ★ **日本語などマルチバイトの値は body で受ける**。クエリ文字列の
    percent-encoding を戻す手段が本体側に無いビルドがある
    (`System.urldecode` は Windows 拡張で、SDL ビルドには無い)。
    query は ASCII の数値・識別子だけに使うのが安全。

■ 4. ゲーム → ブラウザの push (SSE)

  WebServer.broadcast(channel, text)

  ページ側は `new EventSource("/sub/<channel>")` で購読する。
  このデモは channel = "webui"。パネルの「ブラウザへ push」を押すと
  ゲーム側の変化がブラウザのログへ流れる。

  curl でも確認できる:
    curl -sN http://127.0.0.1:8900/sub/webui
    → data: ゲーム側でカウンタを 6 にしました

■ 5. curl で API を叩く (ブラウザ無しの確認)

  curl http://127.0.0.1:8900/api/state
  curl -X POST --data-binary "6aa8ff" http://127.0.0.1:8900/api/color
  curl -X POST --data-binary "こんにちは" http://127.0.0.1:8900/api/message
  curl -X POST "http://127.0.0.1:8900/api/bump?by=5"

■ 関連リファレンス

  doc/reference/WebServer.md   register / serveStatic / broadcast /
                               start / stop / openBrowser / active / url
  src/core/doc/REPL.md         「ブラウザ REPL / Web サーバ」節
                               (組込ルート一覧、ブラウザ UI のタブ構成、
                                ブラウザとアプリの寿命、起動オプション一覧)

■ メモ

  - 実利用例として krkrthreepp のブラウザ編集 UI (/ui/ + /app/ +
    /api/three/) がある。
  - JSON の組み立ては json プラグイン (Scripts.toJSONString) を使ってもよいが、
    このデモは core だけで完結させるため手組みしている。
  - TJS の文字取り出しは `s[i]`。`s[i, 1]` と書くとカンマ式になり
    **常に s[1] を返す**ので注意 (このデモの実装中に踏んだ)。
