# 外枠ビルドから本体を組み込む口 (機種移植)

本体 (`src/core`) は単独でデスクトップ版 (Windows / macOS / Linux) をビルドできるが、
Android / iOS / Web / Linux 配布 / Xbox (GDK) / コンソール機などは、**外枠リポジトリ**が
本体を `add_subdirectory` して組み込む。このページは、外枠が本体の作りを変えずに
機種の差を差し込むための口をまとめたもの。

外枠の例: krkrz_android / krkrz_ios / krkrz_web / krkrz_linux / krkrz_gdk
(いずれも「本体 + 共通ビルド (外枠) + 作品フォルダ」の 3 層。作品フォルダの資材の集め方
`assetPack` は外枠どうしで同じ書式)。

## CMake の口

外枠は `add_subdirectory(<本体>)` の**前に**次の変数を決めておく。

| 変数 / 関数 | 用途 |
|---|---|
| `KRKRZ_SRC_PLATFORM` | 機種専用のソースを本体の実行ファイルへ足す |
| `KRKRZ_INC_PLATFORM` | 同じくインクルードディレクトリ |
| `KRKRZ_LIB_PLATFORM` | 同じくリンクするライブラリ |
| `KRKRZ_DEFINES_PLATFORM` | 同じくコンパイル定義 (本体とプラグインの両方に効く) |
| `KRKRZ_DESKTOP` | デスクトップ機能 (REPL / LogCore / `.cf`・`.cfu` / シェル連携など) の有無。未定義なら OS から判定する (WIN32・macOS・Linux = ON)。**`WIN32` が真でもデスクトップではない機種** (Xbox など) は外枠が `OFF` を先に決める |
| `KRKRZ_APP_PLATFORM_OVERRIDE` | 真にすると `sdl3/environ/stdapp.cpp` を外す。外枠が `KRKRZ_SRC_PLATFORM` で機種専用の Application (`GetSDL3Application` / `TVPAppInit` / `InitPath` / `InitDataPath` など) を供給する |
| `KRKRZ_RESOURCE_EXTERNAL` | 真にするとエンジンリソース (`config.cf` / メッセージ / フォント等) を exe に埋め込まず (`generic/app/winres.cpp` を外す)、Application が `ResourcePath` を `file://` で指す前提になる。Win32 のリソース列挙 API が無い機種向け (wasm と同じ考え方) |
| `KRKRZ_AUDIO_PLATFORM_OVERRIDE` | 真にすると miniaudio + SDL3 の音声出力 (`common/sound/AudioStream.cpp` / `sdl3/sound/audio.cpp`) を外し、機種専用の `iTVPAudioStream` を使う |
| `KRKRZ_VARIANT_OPTION` (定義) | `System.buildVariantName` の末尾に付く文字列 (例 `_GDK` → `SDL_GDK`)。スクリプトが機種を見分ける用 |
| `krkrz_platform_target_hook(TARGET)` | 外枠でこの関数を定義しておくと、本体が実行ファイルのターゲットを作った直後に呼ぶ。機種の DLL を出力先へコピーする、などに使う |

描画系の選択は通常のオプションで行う: `KRKRZ_USE_OPENGL` (GL 版) / `KRKRZ_CANVAS_GPU`
(SDL_GPU 版。GL と排他。→ [CanvasGPU.md](CanvasGPU.md))。
REPL まわり (`KRKRZ_REPL` / `KRKRZ_REPL_WEB` / `KRKRZ_REPL_WEB_BIND_ANY`) は
[REPL.md](REPL.md)。

プラグインは機種では静的リンクにする。外枠が各プラグインのライブラリと登録表
(`StaticPlugins.cpp` 等) を用意し、`KRKRZ_SRC_PLATFORM` / `KRKRZ_LIB_PLATFORM` で渡す。
generic でのプラグイン解決の注意は [GenericPluginLoading.md](GenericPluginLoading.md)。

## ソース側の判定マクロ

| マクロ | 意味 |
|---|---|
| `KRKRZ_DESKTOP` | CMake の `KRKRZ_DESKTOP` が ON のとき定義 |
| `TVP_WIN32_DESKTOP` | Win32 の**デスクトップ向け API** (コンソール / シェル / GDI / リソース列挙 / `GetVersionEx` など) が使えるとき定義 (`common/tjs2/tjsTypes.h`、`WINAPI_PARTITION_DESKTOP` で判定)。Xbox (`WINAPI_FAMILY_GAMES`) は `_WIN32` だがこれらが無いので、**該当 API を呼ぶ箇所は `_WIN32` ではなくこちらで分ける** |
| `TVP_NEED_BITMAPINFO_DEF` | `_WIN32` だが `wingdi.h` の `RGBQUAD` / `BITMAPINFO` が無い環境 (`WINAPI_FAMILY_GAMES`)。本体 / tp_stub が自前の定義を使う (`BITMAPINFOHEADER` は SDK 側にある) |

新しく Win32 API を使うコードを書くときは、それがデスクトップ専用 (`WINAPI_PARTITION_DESKTOP`)
でないかを確かめ、専用なら `TVP_WIN32_DESKTOP` で囲むこと。囲み忘れは Xbox 向けのビルドで
初めて分かる。

## 機種専用 Application が決めること

`KRKRZ_APP_PLATFORM_OVERRIDE` で Application を差し替えるとき、デスクトップ版
(`stdapp.cpp`) と同じ意味の値を返す:

- `InitPath`: データの場所 (`data.xp3` → exe 結合 xp3 → `data/startup.tjs` の順、など機種の配置に合わせる)
  と、`KRKRZ_RESOURCE_EXTERNAL` のときのリソースの場所
- `TempPath`: 一時フォルダ
- `InitDataPath`: セーブデータの場所 (`System.dataPath`)。**ネイティブのパス**で返す (`file://` で返すと
  「データの保存場所が存在しない」警告が出る)
- `getPlatformTagSpec`: `config_<tag>.cf` の選択や `System.platformTag` に使う機種タグ。`;` 区切りで
  「一般 → 具体」の順に複数返せる (例 `xbox;scarlett`)。タグの呼称は本体に持たず機種側が返す

`user://` の差し替えは [UserStorage.md](UserStorage.md)。

## 起動オプションファイル

`<exe名>.cf` / `<exe名>.cfu` を読むのは**デスクトップだけ**で、非デスクトップでは exe と同じ場所の
`config.cf` (と埋め込みの `config_<tag>.cf`) だけを読む。機種のパッケージにオプションを入れるときは
`config.cf` として置く。優先順位の全体は [CommandLinePresets.md](CommandLinePresets.md)。

## 機種で注意すること

- **全画面**: コンソール機は常に全画面。`sdl3/environ/form.cpp` は SDL の機種マクロ
  (`SDL_PLATFORM_XBOXONE` / `SDL_PLATFORM_XBOXSERIES` など) で全画面固定にしている。機種を足したらここも見る
- **未処理例外のダイアログ**: 例外を表示する間はシステムイベントが止まる。Elements のモーダルは、
  止まっている間も入力イベントとウィンドウ更新だけは自分で配送する (`SDLElementsModalRunner.cpp`)。
  これが無いとダイアログが描かれず、パッドの入力も届かない。→ [ElementsDialog.md](ElementsDialog.md)
- **動画**: overlay 系動画の再生矩形 (`setPos` / `setSize`) は presenter 経由で描画先へ写す。
  独自の DrawDevice や presenter を足すときは `CoversScreen()` (画面を覆うか) を見て、覆わないときは
  ゲーム画面を描いてから動画を重ねること。→ [MovieMFMigration.md](MovieMFMigration.md) 「overlay 動画の表示位置・大きさ」
- **GPU シェーダー**: 実行時コンパイルに頼れない機種は、組み込み・スクリプトのシェーダーを
  `gpushaderc.py` で事前変換したパックを持たせる (D3D12 は `--target dxil`)。→ [CanvasGPU.md](CanvasGPU.md)
