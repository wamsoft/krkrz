# エンジン設定 (-userconf) — ブラウザ UI

> 状態: 実装済み (2026-10-12)。WINVER / SDL3 共通。

## 概要

本体を `-userconf` 付きで起動すると、ゲームを起動する代わりに設定画面をブラウザで開き、
エンドユーザーが変えた設定を `<exe名>.cfu` (データ保存場所) に保存して終了する。

- 以前は WINVER が Win32 のダイアログ (`ConfigFormUnit`)、SDL3 が Elements の画面
  (`SDLElementsUserConfig`) と別々だった。どちらも削除し、標準ツール (krkrz_tools) と同じ
  appserve を使うブラウザ UI 1 本にした。Elements を外したビルド (`KRKRZ_USE_ELEMENTS=OFF`) でも使える
- 量産ビルド (`MASTER`) でも入る (`-replweb` の Web サーバとは別。あちらは MASTER で無効)
- デスクトップ (`KRKRZ_DESKTOP`) のみ。CMake オプション `KRKRZ_USERCONF_WEB` (既定 = デスクトップで ON)

## 構成

| 部品 | 場所 |
|---|---|
| 入口 `TVPExecuteUserConfig()` | `common/base/UserConfig.h` / `UserConfigWeb.cpp`。WINVER は `win32/environ/Application.cpp`、SDL は `generic/environ/Application.cpp` から呼ぶ |
| .cfu の場所 `TVPGetUserConfigFilePath()` | 各 `SysInitImpl.cpp` (起動時に .cfu を読む場所と同じ。`-datapath` を反映) |
| 項目一覧 | `TVPGetEngineCommandDesc()` (本体の optiondesc。OS 言語 / `-language` で選ぶ) + `TVPMergeAllPluginCommandDesc()` |
| プラグインの項目 | WINVER: exe と同じフォルダ・`plugin64\` (`plugin\`) の .dll / .tpm のリソース。SDL3: exe と同じフォルダ・プラグインフォルダの `<basename>.options.json` |
| サーバ | `external/appserve` (submodule、MIT。ライブラリ `appserve::core` だけをリンク) |
| 画面 | `common/base/userconf_web/` (index.html / app.js / style.css)。configure 時に appserve の JS ランタイム (`lib/appserve.js`) と一緒に集め、exe に埋め込む (`appserve_embed_web`) |
| ライセンス | `licenses/manifest.json` の appserve (`TVP_LICENSE_WITH_APPSERVE`。このビルドだけ一覧に載る) |

## 流れ

1. `TVPInitializeBaseSystems()` の後 (起動オプションと .cf / .cfu の読み込み後)、ウィンドウを作る前に
   `-userconf` を見る
2. 127.0.0.1 の空いているポートで appserve を起動し、トークン付きの URL をブラウザで開く
   (Edge / Chrome のアプリ表示 → だめなら既定のブラウザ。標準ツールと同じ)。URL はログにも出る
3. 画面が API を呼ぶ:

   | API | 内容 |
   |---|---|
   | `GET /api/userconf` | `{lang, file, categories:[{name, options:[{name, caption, description, type, length, values:[{value, desc}], default, current}]}]}`。`user: true` の項目だけ。`current` は今の値 (.cf / .cfu / 起動引数を反映したもの) |
   | `POST /api/userconf/save` | `{"values":{name:value}}` を .cfu へ書く。選択肢に無い値は 400 |
   | `POST /api/userconf/close` | 終了する |

4. 保存して閉じる / 保存しないで閉じる / ブラウザの窓を閉じる、のどれかで終了する
   (窓を閉じた場合は appserve のアイドル監視で約 10 秒後)。サーバが止まると窓も閉じる

## .cfu の書き方

書式は旧ダイアログと同じ (先頭に注意書き、`name="\xNN..."` で UTF-16 のコード単位を 16 進)。

書くのは **この画面で変えた項目** と **もともと .cfu にあった項目** だけ。変えていない項目は書かない。
ゲーム側の .cf や埋め込みの設定で決まっている値をそのまま .cfu へ写すと、後で制作側が .cf を
変えても利用者の .cfu が上書きして効かなくなるため。画面で扱わない行 (手で足した行など) は残す。

## 起動オプション

| オプション | 内容 |
|---|---|
| `-userconf` | 設定画面を開く |
| `-userconfbrowser=app\|default\|none` | ブラウザの開き方。既定 `app` (アプリ表示)。`none` は開かずに URL をログに出す (検証や別の端末から開く場合) |
| `-language=` | 画面と項目の説明の言語 (ja / en / chs / cht) |
| `-datapath=` | .cfu の保存先 (データ保存場所) |

## 確認 (2026-10-12)

SDL3 (x64) と WINVER (x64) で、`-userconfbrowser=none` + API と内蔵ブラウザで確認:
項目一覧 (本体 46 項目)、既存の .cfu の値を今の値として読む、変えた項目だけ書く / 前から .cfu にあった
項目を保つ / .cf 由来の値を写さない、選択肢に無い値の拒否 (400)、`-language=en` で英語、保存した後・閉じた後に
プロセスが終わる。

## 残り

- 旧ダイアログのリソース (`IDD_CONFIG_DIALOG`) は `win32/vcproj/tvpwin32.rc` に残っている (使われていない)。
  rc ファイルが特殊な文字コードなので、リソースエディタで消すのが安全
