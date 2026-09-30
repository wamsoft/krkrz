# バージョン番号の扱い

エンジン (krkrz 本体) のバージョン番号がどこで決まり、どこへ流れるかをまとめる。

## 1. 単一の供給元

**`CMakeLists.txt` 冒頭の `PROJECT_VERSION`** が唯一の供給元。

```cmake
set(PROJECT_VERSION 2.0.0)          # M.m.r
set(KRKRZ_VERSION_BUILD "0" CACHE STRING "...")   # 4 桁目 (既定 0)
```

ここから configure_file で 2 つの生成物ができる。

| 生成物 | 用途 | テンプレート |
|---|---|---|
| `${CMAKE_BINARY_DIR}/krkrz_version.h` | C++ 側 (SDL / LIB の版情報) | `cmake/krkrz_version.h.in` |
| `${CMAKE_BINARY_DIR}/krkrz_version.rc` | Windows の VERSIONINFO (WINVER / SDL 双方) | `cmake/krkrz_version.rc.in` |

実行時の版情報の取り方はバリアントで違うが、供給元は同じなので値は一致する。

- **WINVER**: `win32/msg/MsgImpl.cpp` の `TVPGetVersion()` が**自分の exe の
  VERSIONINFO** を読む → 生成 rc の値
- **SDL / LIB**: `generic/base/PluginImpl.cpp` の `TVPGetFileVersionOf()` が
  `krkrz_version.h` のマクロを返す

> 2026-08-16 まで、WINVER は `win32/vcproj/tvpwin32.rc` 手書きの 2.0.0.0、
> SDL は `PluginImpl.cpp` 手書きの 1.0.0.1 と**食い違っていた**。
> 同じソースから作った 2 つの exe が別の版を名乗る状態だったため一本化した。

## 2. 番号を上げる

`PROJECT_VERSION` を書き換えるだけ。 rc / ヘッダ / exe プロパティ /
`System.versionString` がすべて追従する。

4 桁目 (`KRKRZ_VERSION_BUILD`) は**既定 0 のまま**にしておく。CI のビルド番号を
入れたい場合だけ `-DKRKRZ_VERSION_BUILD=N`。手元ビルドと CI で値が変わると
「同じソースなのに版が違う」問題が再発するため、常用はしない。

### 上げ方の基準 (semver 準拠 + krkrz 固有)

| 変更 | 上げる桁 |
|---|---|
| TJS API の非互換変更 / セーブデータ互換の破壊 | メジャー |
| **実験中 API の非互換変更** | **マイナー** (下記) |
| プラグイン ABI に影響する変更 (tp_stub の再生成が要る) | 最低でもマイナー |
| 後方互換のある機能追加 | マイナー |
| 修正のみ | パッチ |

### 実験中 API (安定保証の対象外)

semver は「公開 API が安定している」前提の仕組みなので、**まだ形が固まって
いない部分**をそのまま当てはめるとメジャー番号が意味を失う。 導入したばかりで
呼び名すら動いている API のために 3.0.0 を出す、というのは実態と合わない。

そこで **安定保証の対象外を明示**し、そこの非互換変更はマイナーで通す
(Node の Stability index や Kubernetes の alpha/beta/GA と同じ考え方)。

- 印の付け方 = `doc/manual/<Class>.manual.tjs` の javadoc に **`@experimental`**
  (補足文を続けて書ける)。 `tjsdoc.py` がリファレンス冒頭へ警告を出す
- 大事なのは**事前に引けること**。 利用側が知りたいのは「これに乗ってよいか」で、
  それは番号の桁ではなく印でしか伝わらない
- 実験中でも**非互換変更はリリースの移行メモに必ず書く** (下記)

現在 実験中 のもの:

| API | 理由 |
|---|---|
| `ElementsDialog` / `ElementsPanel` | Elements ベースの UI 機構。API 語彙がまだ動いている (2026-09-04 に `Dialog` から改名、互換エイリアス無し) |
| `Agent` / `WebServer` | 開発・検証用。REPL / デバッグ窓の整備にあわせて変わる |

**外すときの目安**: 案件 2 本以上で使われ、1 リリースを跨いで API を変えずに
済んだら安定とみなして `@experimental` を落とす。 決めておかないと永久に
実験中のままになる。

### 移行メモ

**非互換変更があるリリースは、サマリとタグメッセージの冒頭に一覧を置く**
(末尾だと見落とされる)。 実験中 API の変更もここには必ず書く — 番号で
警告しない代わりに、ここが唯一の告知になる。

develop では番号を触らず、**リリース枝へ反映するときに確定してタグを打つ**。
develop でこまめに上げるとマージのたびに衝突する。

## 3. 製品名 (VERSIONINFO の文字列)

`KRKRZ_PRODUCT_NAME` を CMake が組み立てる。 末尾のプラットフォーム名だけ
バリアントで変わる。

```
TVP(KIRIKIRI) Z Core Wamsoft Edition / Scripting Platform for Win32
TVP(KIRIKIRI) Z Core Wamsoft Edition / Scripting Platform for SDL3
TVP(KIRIKIRI) Z Core Wamsoft Edition / Scripting Platform for Library
```

`InternalName` は `tvp2/<プラットフォーム名>`、`OriginalFilename` は
実行ファイル名 (`krkrz64.exe` / `krkrz.exe`) が入る。

Windows 以外 (Linux / macOS / Android / wasm) には VERSIONINFO が無いので、
版情報は `krkrz_version.h` 経由のみ。

## 4. 確認方法

```
# exe のプロパティ (Windows)
(Get-Item krkrz64.exe).VersionInfo | Format-List FileVersion,ProductName

# 実行時
krkrz64.exe <data> -demotest        # 起動ログの「実行コア/M.m.r.b」
# REPL から
System.versionString
```

両バリアントで同じ値が出ること、exe プロパティの末尾だけが
`Win32` / `SDL3` で変わることを確認する。
