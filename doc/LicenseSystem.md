# ライセンス情報の内部保持と収集 (LicenseIntf)

本体・プラグイン・プロジェクトデータの 3 系統からライセンス文を収集し、
TJS (`System.getLicenseList` / `getLicenseText`) とプラグイン (tp_stub) から
参照できるようにする仕組み。実装 = `common/base/LicenseIntf.{h,cpp}`。

## 3 系統の収集ソース

| ソース | 保持形態 | 供給方法 |
|---|---|---|
| 本体内蔵 (`builtin`) | zlib deflate 圧縮テーブル | `licenses/manifest.json` + `licenses/texts/` から `licenses/gen_licenses_c.py` が `common/base/BuiltinLicenses.cpp` を生成 (コミット対象の生成物) |
| プラグイン登録 (`plugin`) | 圧縮 (参照保持) または生テキスト | tp_stub の `TVPRegisterLicense` / `TVPRegisterLicenseText`。生成器の `--mode plugin` でプラグイン用登録関数を生成できる |
| storage (`storage`) | プロジェクトの `licenses/*.txt`・`.md` | 実行時に列挙・読み込み (案件が追加フォント等のライセンス文を置くだけで一覧に載る) |

- 同名は **プラグイン登録 > 内蔵 > storage** の優先で 1 件に畳む
  (プラグインが本体内蔵と同じコンポーネントを登録しても二重表示しない)。
- 収録テキストは UTF-8 (BOM 無し・LF)。表示名は ASCII 推奨。
- storage の表示名はストレージ層の正規化 (小文字化) を受ける。

## 本体内蔵分の更新手順

```
# 1. licenses/texts/ にライセンス文を追加/更新し manifest.json へ登録
# 2. 再生成 (バイト一致なら書き換えない)
python licenses/gen_licenses_c.py
```

収録内容は `licenses/manifest.json` 参照 (エンジン依存ライブラリ+同梱フォント
一式、37 件 / うち 2 件は構成依存 / 元テキスト約 101KB → 圧縮約 44KB)。収録元の由来:

- vcpkg 依存 → `vcpkg_installed/*/share/<port>/copyright`
- FetchContent 依存 → `build/*/_deps/<name>-src/LICENSE|COPYING`
- vendored → 各ツリー内 LICENSE
- フォント → OFL 1.1 全文 (Noto) / Apache-2.0 表記 (Roboto) / fontello

## manifest のフィールド

| キー | 必須 | 意味 |
|---|---|---|
| `name` | ○ | 表示名。一覧・`System.getLicenseText()` のキー・配布物のファイル名 (slug) の元 |
| `group` | ○ | 分類。本体は `engine` / `font-engine` / `audio` / `video` / `ui` / `platform` / `font`、プラグインは `plugin:<名前>` |
| `license` | ○ | ライセンス種別 (`MIT` / `BSD-3-Clause` 等)。**一覧表の SSOT**。本文からの推測はしない |
| `file` | ○ | ライセンス原文へのパス (manifest からの相対) |
| `when` | | 構成依存エントリの条件マクロ (下記) |
| `note` | | 一覧表の備考欄。デュアルライセンスや非 OSS など、注意が要るものだけ書く |

`license` と `note` は C++ 生成物には影響しない (一覧表の生成にだけ使う)。

## 本体内モジュールの分離収録 (`--mode module`)

本体へ静的リンクされるが、ビルド構成によって入ったり入らなかったりするモジュール
(公開リポジトリに含めないものを含む) は、**本体の manifest に載せず、モジュール側に
manifest を置く**。一覧へはそのモジュールを含むビルドでだけ実行時に載る。

```
python licenses/gen_licenses_c.py --mode module \
    --manifest <module>/licenses/manifest.json \
    --out <module>/LicensesGen.cpp --func RegisterXxxLicenses
```

- 生成物は tp_stub ではなく `LicenseIntf.h` を直接使う (互換ガード無し)。
- モジュール側の CMake で生成物をソースに加え、
  `KRKRZ_DEFINES` に `TVP_CUSTOM_LICENSES=<登録関数名>` を足す。
- `LicenseIntf.cpp` が一覧を引く直前 (`TVPGetLicenseList` /
  `TVPGetLicenseText` / `TVPEnumLicenses`) に一度だけ呼ぶので、
  静的初期化順に依存しない。マクロ未定義のビルドでは何も起きない。
- 配布物へライセンス文を出したい場合は、モジュール側の CMake が自分で
  `install(FILES ... DESTINATION licenses RENAME <slug>.txt)` を書く。
  本体の install ループは本体 manifest しか見ない。

こうすると、本体の `licenses/manifest.json` にも公開ドキュメントの一覧表にも
そのモジュールの名前が出ない。表示は実行時の `System.getLicenseList()` 経由のみになる。

## 一覧表の生成 (`--mode table`)

本体とプラグインの manifest を横断して、Markdown の対応表を生成する。
出力先は umbrella の docs:

```
python src/core/licenses/gen_licenses_c.py --mode table \
    --plugins src/plugins --out doc/guide/BundledLicenses.md
```

- 本体 (`group != "font"`) / プラグイン / その他 (フォント) の 3 節 + 構成依存の一覧。
- `--plugins` は `<dir>/<名前>/licenses/manifest.json` を走査する。省略すると本体分だけ。
- `license` が無いエントリがあるとエラーで止まる (書き忘れ検出)。
- 出力はバイト一致なら書き換えない。manifest を触ったら再生成してコミットに含める。

## 構成依存の同梱物 (`when`)

ビルド構成によっては 1 行もコンパイルされないコンポーネントがある。そうした
ものを一覧へ載せないため、manifest のエントリに `"when": "MACRO"` を付けられる。

```json
{ "name": "JerryScript", "group": "ui", "file": "texts/jerryscript.txt",
  "when": "TVP_LICENSE_WITH_LOTTIE" }
```

- 生成器はそのエントリのバイト配列とテーブル行を `#if defined(MACRO)` で囲む
  (プラグインモードなら `TVPRegisterLicense()` 呼び出しを囲む)。
- `TVPBuiltinLicenseCount` は配列サイズから求まるので件数のずれは起きない。
- `make install` のライセンス書き出しも同じ判定を行う (マクロが
  `KRKRZ_DEFINES` に入っている構成でのみ `licenses/<slug>.txt` を出す)。
- マクロを定義するのは CMake 側。構成判定のすぐ隣に置き、manifest の `when` と
  対で読めるようにする。

現状の実例:

| マクロ | 定義条件 | 対象エントリ |
|---|---|---|
| `TVP_LICENSE_WITH_LOTTIE` | `TVG_LOADER_LOTTIE` が ON (`src/core/CMakeLists.txt` の Elements 節 / layerExVector は `target_compile_definitions`) | JerryScript, RapidJSON |
| `TVP_LICENSE_WITH_D3DCOMPILER` | WIN バリアント (`src/core/CMakeLists.txt` の win32 節)。d3dcompiler へ直接リンクするのはこの構成だけ | D3DCompiler |
| `TVP_LICENSE_WITH_ANGLE` | **定義箇所なし**。ANGLE の libEGL / libGLESv2 を配布物へ同梱する構成が現れたら、その同梱処理の隣で定義する | ANGLE |

Lottie ローダは JerryScript (Lottie Expressions の式評価) と RapidJSON を道連れに
するが、既定構成では OFF なので両者は exe にも layerExVector.dll にも入らない。

D3DCompiler と ANGLE は「同梱しているのに載っていない」ではなく逆で、**どの構成でも
入らないのに一覧へ出ていた**ため `when` を付けた。SDL バリアントの Windows ビルドは
SDL3 が `d3dcompiler_47.dll` を必要に応じて動的に読むだけで、DLL 自体は再頒布して
いないので載せない。ANGLE はエンジンのどのビルドもリンクしておらず、同梱もしていない。

## プラグインへの適用手順 (layerExVector が実例)

```
# プラグイン側に licenses/manifest.json を用意 (同梱コンポーネントを列挙)
python <core>/licenses/gen_licenses_c.py --mode plugin \
    --manifest <plugin>/licenses/manifest.json \
    --out <plugin>/LicensesGen.cpp --func RegisterXxxLicenses
# 生成された LicensesGen.cpp をソースに追加し、リンク時コールバック
# (NCB_PRE_REGIST_CALLBACK 等) から RegisterXxxLicenses() を呼ぶ
```

group は `plugin:<プラグイン名>` を推奨。呼び出しは ncbind プラグインなら
`NCB_PRE_REGIST_CALLBACK(RegisterXxxLicenses);` の 1 行で済む。

**バージョン非対応環境との互換**: tp_stub は License API と同時に
`TVP_HAS_LICENSE_API` を定義する。生成物 LicensesGen.cpp はこのマクロで
全体をガードしており (未定義なら空関数)、**旧 tp_stub / 旧本体ヘッダと
組み合わせたビルド (環境あわせの static リンク等) でもそのまま通る**。
プラグイン側で TVPRegisterLicense 系を直接呼ぶ場合も同じマクロで
ガードすること (tjsDataPack の LICENSE_SHOW が実例)。実行時の旧本体
(エクスポート無し) は tp_stub が catchable 例外を投げるため、呼び出しを
try/catch で包めば併せて安全になる。

適用済み:

- src/plugins (2026-08-10): layerExVector / krkrthreepp / krkreffekseer / krkrlive2d
  (proprietary notice 文) / psdfile / minizip / expat / krkr_richtext
- src/plugins (2026-09-04): 第三者コードを同梱しているのに未登録だった 4 件を追加 —
  layerExImage (CxImage。`LICENSE.TXT` はあったが manifest が無く、配布物にも実行時
  一覧にも出ていなかった。旧方式の `TVPAddImportantLog()` 全文出力は撤去) /
  layerExSave (LodePNG) / sigcheck (LibTomCrypt + LibTomMath) /
  clipfile (clipparse + 同梱ビルドの SQLite)。
- 旧 plugins_utf8 系 (svn r9315-9316): Win32 RCDATA + `ShowLicense()` 方式
  (Windows 専用) を全 13 プラグインでこの機構へ置換済み — wfstkeff /
  wfdspfilter / toml / cpp_whisper / msdfatlasgen / layerMultiStore /
  wuwavpack / wumtrack / budoux / m2vdec / rectpack2D / xlsxWriter / nullc。
  `ShowLicense()` は登録ラッパ (旧本体では catch でスキップ) になり、
  ログへの全文出力は廃止。plugins_utf8 直下の tp_stub コピーも更新済。
  未移行: tjsDataPack (license_bin.c 独自方式のまま)。

## TJS / tp_stub API

- TJS: `System.getLicenseList()` → `[%[name, group, source], ...]` /
  `System.getLicenseText(name)` → 文字列 or void。
  ゲーム側のライセンス表示 UI (フォント選択画面等) はこれで自由に組める
  (例: `group == "font"` でフィルタ)。
- tp_stub: `TVPRegisterLicense(name, group, deflated, deflatedSize, originalSize)` /
  `TVPRegisterLicenseText(name, group, text)` / `TVPGetLicenseText(name, &text)` /
  `TVPEnumLicenses(sink)`。

## 起動オプションからの取り出し (`-license` / `-about`)

本体内部専用の口 (`LicenseIntf.h` 末尾。tp_stub 非公開):

| 関数 | 用途 |
|---|---|
| `TVPGetLicenseListText()` | 収集結果を桁揃えした一覧テキストへ整形 (改行は `TJS_TEXT_OUT_CRLF` 準拠) |
| `TVPGetAboutFallbackString()` | about 用ライセンス文がリソースから読めないときの代替 (内蔵の `Kirikiri Z` エントリ) |
| `TVPCheckPrintLicense()` | `-license` 系を処理して標準出力へ書き出す。処理したら true (起動を打ち切る) |

### `-license` (全バリアント)

```
krkrz64.exe -license              # 一覧 (名前 / 分類 / 供給元) を標準出力へ
krkrz64.exe -license=FreeType     # その 1 件の全文
krkrz64.exe -license=all          # 全件の全文 (`======== <名前> ========` 区切り)
```

- 名前が見つからないときは `TVPLicenseNotFound` に続けて一覧を出す。
- 判定位置はプラグインのロード直後 (WINVER = `TVPLoadPluigins()` の後、
  SDL3 = `InitializeApplication()` の後)。**プラグインが登録した分と
  プロジェクトの `licenses/*.txt` も載った状態**で出る。
- ただしここで載るのは**自動ロードされたプラグインだけ** (exe 隣 / `system/` /
  `plugin(64)/` の `*.tpm`)。`Plugins.link()` で起動スクリプトから読むぶんは
  この時点でまだロードされていないので出ない。プラグインの登録漏れを
  `-license` で確認したいときは、対象 DLL を `plugin64/<名前>.tpm` として
  置いてから実行する。ゲーム内表示 (`System.getLicenseList()`) と配布物の
  `licenses/plugins/` はこの制約を受けない。
- 出力は `TVPWriteStdOutText()` (`common/utils/WinConsole.{h,cpp}`) 経由。
  本体は GUI サブシステムなので CRT の `stdout` は親シェルに繋がらない。
  Windows では `STD_OUTPUT_HANDLE` へ直接書く (真のコンソール = `WriteConsoleW`
  で UTF-16、パイプ / リダイレクト = UTF-8 で `WriteFile`)。
  受け取り側はパイプかリダイレクトを使うこと (`-printdatapath` と同じ制約)。
- `-license` 指定時はデータフォルダの選択ダイアログを出さない
  (WINVER = `win32/base/SysInitImpl.cpp` の `-nosel` 相当の扱い、
  SDL3 = `sdl3/environ/stdapp.cpp` の `HasInfoOnlyOption()` で exe の場所を
  プロジェクトパスとして進む)。プロジェクトを指定して起動すれば、その
  `licenses/*.txt` も一覧に載る。
- 重要ログ (`!` 付きの起動時情報) も同じ標準出力に出るため、機械処理する
  場合は一覧部分を切り出すこと。この制約は `-printdatapath` と同じ。

### `-about` / `System.licenseText`

`TVPGetLicenseString()` (= `System.licenseText`) と `TVPGetAboutString()`
(`common/msg/MsgIntf.cpp`) は、全バリアント共通で次を連結して返す:

```
TVPGetVersionInformation()   バージョン行 (メッセージ TVPVersionInformation、%DATE%/%TIME% 置換込み)
TVPGetEngineLicenseText()    本体条項 = 内蔵テーブルの "Kirikiri Z" (= LICENSE)
TVPGetLicenseListText()      収録コンポーネントの一覧
TVPGetImportantLog()         環境情報 (-about のみ)
```

**リソース埋め込みの合本 (旧 `win32/vcproj/license.txt`、73KB / 1518 行) は廃止した**
(2026-08-31)。中身は `LICENSE` = 内蔵の `Kirikiri Z` エントリと重複しており、
per-library 部分は本機構の管轄。同時に `tvpwin32.rc` の `IDR_LICENSE_TEXT`
エントリと `TVPReadAboutStringFromResource()` (win32 / generic の `MsgImpl.cpp`)
も削除している。これで**バリアントごとの分岐が無くなり**、SDL3 でも
(従来 `resource/license.txt` が無く "Resource Read Error." だった) 正しい
ライセンス文が出る。

収録テキストは UTF-8 / LF 固定なので、表示前に `TVP_TEXT_EOL` (Windows = CRLF、
`TJS_TEXT_OUT_CRLF` 準拠) へ正規化する。**Win32 の EDIT コントロールは LF 単独
では改行しない**ため、about ダイアログではこれが必須。

**`-about` のダイアログ表示自体は WINVER のみ** (`TVPCheckAbout()` →
`TVPShowVersionForm()`。`System.showVersion()` も WINVER のみ)。文字列は
全バリアント共通で作れるので残るのは表示手段だけ。SDL 版への用意は
umbrella の `TODO.md` 「将来課題」に登録済み。

## 配布物への同梱 (make install)

`install` 時に以下が配布物へ書き出される (CMakeLists 末尾の
「ライセンス文の配布同梱」節):

| 出力先 | 内容 |
|---|---|
| `license.txt` (ルート) | `src/core/LICENSE` |
| `licenses/<slug>.txt` | 本体 `licenses/manifest.json` の全エントリ (内蔵テーブルと同一内容) |
| `licenses/plugins/<slug>.txt` | **実際にビルドしたプラグイン**の `licenses/manifest.json` の全エントリ |

slug = 表示名の小文字化 + 記号を `-` 化。書き出しは
`krkrz_install_license_manifest()` の共通処理で、`"when"` の判定も本体と
プラグインで共通。

- プラグイン分は**プラグインごとには分けず `licenses/plugins/` へ一括**で出す。
  同じ資材を複数のプラグインが挙げている場合 (ThorVG / minizip-ng 等) は 1 本に
  畳む。本体と同名でも別に出す (本体無しでプラグインだけ配る構成でも成立させる)。
- 対象は `TVP_PLUGINS` で実際にビルドしたぶんだけ。ビルドしないプラグインの
  ライセンス文は入らない。
- プラグイン側が target のコンパイル定義で `"when"` のマクロを立てる場合は、
  install から見えるよう GLOBAL プロパティ `KRKRZ_LICENSE_ACTIVE_MACROS` へも
  追記する (layerExVector が実例)。
- 実行時一覧 (`System.getLicenseList()`) はこの install とは独立で、各プラグインの
  `LicensesGen.cpp` が DLL ロード時に登録する。

## LICENSE ファイルとの役割分担 (2026-08-10 再構成済)

- `src/core/LICENSE` (= 配布物の license.txt) は **このソースツリーに含まれる
  コード限定**に再構成した: 本体条項 + in-tree 第三者コード
  (MT19937 / glad 生成コード / PL_MPEG) の notice + 「リンクされる外部
  ライブラリは licenses/ 分離・実行時 API 参照」の案内。旧来の歴史的合本
  (theora/DirectShow フィルタ群/opusfile/wcs* 等、現在リンクしていないものを
  含む 1400 行超) は撤去した。
- リンクされる外部ライブラリと同梱データ資材の全文は本機構が SSOT。
  リポジトリ内にソースツリーを持つもの (movie-player / libyuv / nestegg /
  glyphware / ThorVG) は manifest がそのツリーの LICENSE を直接参照し、
  外部取得 (vcpkg / FetchContent) 分だけ texts/ にコピーを置く。
- `LICENSE` 自体も "Kirikiri Z" エントリとして収録されるので、実行時一覧には
  本体条項 + in-tree notice が 1 エントリで載る。
- **`-about` / `System.licenseText` が表示する本体条項もこのエントリ**
  (2026-08-31 以降)。表示用に別の合本を持たないので、`LICENSE` を直せば
  配布物の `license.txt`・実行時一覧・about ダイアログが同時に追従する。
