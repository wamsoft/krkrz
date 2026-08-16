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
一式、36 件 / 元テキスト約 180KB → 圧縮約 65KB)。収録元の由来:

- vcpkg 依存 → `vcpkg_installed/*/share/<port>/copyright`
- FetchContent 依存 → `build/*/_deps/<name>-src/LICENSE|COPYING`
- vendored → 各ツリー内 LICENSE
- フォント → OFL 1.1 全文 (Noto) / Apache-2.0 表記 (Roboto) / fontello

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

適用済み (2026-08-10):

- src/plugins: layerExVector / krkrthreepp / krkreffekseer / krkrlive2d
  (proprietary notice 文) / psdfile / minizip / expat / krkr_richtext
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

## 配布物への同梱 (make install)

`install` 時に以下が配布物へ書き出される (CMakeLists 末尾の
「ライセンス文の配布同梱」節):

- `LICENSE` → 配布物ルートの `license.txt`
- `licenses/manifest.json` の全エントリ → `licenses/<slug>.txt`
  (slug = 表示名の小文字化 + 記号を `-` 化。内蔵テーブルと同一内容)

プラグインが登録する分 (plugin:*) はここには含まれない — 各プラグインの
リポジトリ/DLL 側が保持し、実行時 API で参照する。

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
