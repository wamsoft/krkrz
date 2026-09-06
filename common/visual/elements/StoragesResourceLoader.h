//---------------------------------------------------------------------------
//!@file Elements 用 Storages-backed resource_loader
//
// external/elements は ELEMENTS_FILE_IO_SUPPORT=OFF でビルドされているため、
// `cycfi::elements::register_font` / `pixmap(fs::path)` 等は
// `cycfi::elements::get_resource_loader().read(name)` を経由してバイトを
// 取得する。 ここでは krkrz の Storages 系 (TVPCreateStream / TVPReadStream /
// TVPIsExistentStorage / TVPGetStorageListAt) に橋渡しする実装を提供する。
//
// 利点:
//   - Storages 経由なので XP3 アーカイブ内に置いたフォントも読める。
//   - Auto path search が効くため、 短いファイル名指定 ("Noto Sans.ttf") でも
//     krkrz の検索パスから解決される。
//   - std::filesystem が使えないプラットフォームでも krkrz の StorageMedia を
//     差し替えるだけで Elements 側のリソース I/O が動く。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_STORAGES_RESOURCE_LOADER_H
#define ELEMENTS_STORAGES_RESOURCE_LOADER_H

#include "tjsCommHead.h"

//! @brief krkrz Storages 経由の resource_loader を Elements に install する。
//!        多重呼出は no-op (内部 std::call_once でガード)。 Elements を
//!        最初に触る前に呼ぶこと。 EnsureRuntimeInitialized から呼ばれている。
void TVPInstallElementsResourceLoader();

//! @brief 指定 dir 配下の .ttf / .otf を列挙して `cycfi::elements::register_font`
//!        に流す。 elements 側の `load_fonts_from_directory` 相当だが、
//!        TVPGetStorageListAt 経由なので XP3 内のフォントもピックアップする。
//!        family / weight / slant / stretch はファイル名 (CamelCase + `-Style`)
//!        からヒューリスティックに推定する (元の Elements 側ロジックの移植)。
//!        directory が存在しないか空の場合は no-op。
void TVPRegisterElementsFontsFromStorageDir(const ttstr& dir);

#ifdef __WINVER__
//! @brief WINVER host: exe に埋め込まれた ("BINARY" 型リソース) .ttf / .otf を
//!        列挙して register_font_buffer で登録する。 SDL/generic の
//!        TVPRegisterElementsFontsFromStorageDir に相当する WINVER 向け経路。
//!        WINVER はリソースが exe 埋め込み (CMakeLists.txt の resources.rc,
//!        `<filename> BINARY "<path>"`) で、 storage 列挙が効かないため、 Win32
//!        リソース API (EnumResourceNames / FindResource) で直接読み出す。
//!        フォント名から family / weight / slant / stretch を推定するのは
//!        StorageDir 版と同一ロジック。
void TVPRegisterElementsFontsFromWinResources();
#endif

//! @brief 単一フォントを明示登録する (TJS ElementsDialog.registerFont のバックエンド)。
//!        weight / slant / stretch は `cycfi::elements::font_constants` の整数値:
//!          weight  : 10 thin / 20 extra_light / 30 light / 40 normal /
//!                    50 medium / 60 semi_bold / 70 bold / 80 extra_bold /
//!                    90 black / 95 extra_black
//!          slant   : 0 normal / 1 italic / 2 oblique
//!          stretch : 25 ultra_condensed ... 50 normal ... 200 ultra_expanded
//!        path は krkrz Storage パス。 失敗時 (resource_loader.read が空を返す)
//!        は silent fail (= 何も登録しない)。
//!        戻り値: ThorVG が読み取った embedded family name。 取れない場合は
//!        caller 指定の family がそのまま返る。
ttstr TVPRegisterElementsFont(const ttstr& family, const ttstr& path,
	int weight = 40, int slant = 0, int stretch = 50);

//! @brief これまでに登録した family を全部並べた families 文字列で
//!        cycfi::elements の theme.label_font / heading_font / text_box_font /
//!        mono_spaced_font / system_font を上書きする。 ThorVG FT loader の
//!        per-codepoint fallback で多言語表示が効くようになる。
//!        EnsureRuntimeInitialized が初期フォント走査後に自動で呼ぶ。
void TVPApplyRegisteredFontsToElementsTheme();

//! @brief 任意の families 文字列 (comma 区切り) を theme フォントとして
//!        強制的に当てはめる。 TJS `ElementsDialog.defaultFontFamily = "..."` から
//!        呼ばれる。 families が登録済かどうかは検査しない (FT loader が
//!        フォールバックで処理する)。
void TVPSetElementsDefaultFontFamily(const ttstr& families);

//! @brief 現在の theme に当てはまっている families 文字列を返す。
//!        未設定 (空) なら elements のデフォルト名前を返さず空文字を返す。
//!        TJS `ElementsDialog.defaultFontFamily` プロパティ getter。
ttstr TVPGetElementsDefaultFontFamily();

//! @brief Elements の block text バックエンド (矩形テキストの折返し/禁則/
//!        count 制限) を glyphware 実装に差し替える。 これで Elements の
//!        text_area ウィジェットと本体 Layer.drawShapedTextArea が同じ
//!        折返しロジック (glyphware::layoutBlock) を通る。 未呼出なら
//!        Elements 内蔵の素朴な幅貪欲 wrap にフォールバックする。
//!        EnsureRuntimeInitialized から呼ばれる (多重呼出は無害)。
void TVPInstallElementsBlockTextBackend();

//! @brief 実行時画像ストアへ storage path のファイルを name で登録する。
//!        jsonc からは "mem://<name>" で参照 (image ウィジェット等)。
//!        セーブサムネイル等、 実行時に変わる画像を Elements へ渡す仕組み。
//!        画面 build 時に pixmap が読み直されるので、 再登録 → 画面再オープンで
//!        表示が更新される。 読込失敗時は false (登録しない)。
//!        TJS `ElementsDialog.registerImage(name, path)` のバックエンド。
bool TVPRegisterElementsImageFile(const ttstr& name, const ttstr& path);

//! @brief 実行時画像ストアから name の登録を削除する。
void TVPUnregisterElementsImage(const ttstr& name);

//! @brief 実行時画像ストアを全消去する。
void TVPClearElementsImages();

#endif
