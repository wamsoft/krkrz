//---------------------------------------------------------------------------
// Elements 用 Storages-backed resource_loader 実装
//
// external/elements を ELEMENTS_FILE_IO_SUPPORT=OFF でビルドし、 アプリ側
// (この実装) で `cycfi::elements::set_resource_loader()` を差し込む。
// バイト取得は全て TVPReadStream / TVPIsExistentStorage 経由。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "StoragesResourceLoader.h"

#include "StorageIntf.h"
#include "CharacterSet.h"   // TVPUtf8ToUtf16 / TVPUtf16ToUtf8
#include "DebugIntf.h"      // TVPAddLog / TVPAddImportantLog

#include <elements/support/font.hpp>
#include <elements/support/resource_loader.hpp>
#include <elements/support/theme.hpp>

// ThorVG: Text::info() で読込み済みフォントから embedded family / style 名を
// 取り出す。 元データの再パース無しで、 ロード時 ThorVG が掴んだ FT_Face の
// 情報をそのまま返してくれる API (Experimental)。
#include <thorvg.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

//---------------------------------------------------------------------------
// utf-8 <-> ttstr 変換補助 (Storages API は ttstr = utf-16 を期待)
//---------------------------------------------------------------------------
ttstr Utf8ToTtstr(std::string_view u8)
{
	tjs_string ts;
	std::string tmp(u8);   // TVPUtf8ToUtf16 は const std::string& を取る
	TVPUtf8ToUtf16(ts, tmp);
	return ttstr(ts.c_str());
}

//---------------------------------------------------------------------------
// resource_loader 実装本体
//
// read():   TVPReadStream で全バイト読込み (内部で TVPCreateStream → 例外 OK)。
// exists(): TVPIsExistentStorage で auto-path search 込みの存在確認。
//
// 失敗時はどちらも empty / false を返す。 例外は throw しない方針:
//   - elements 側の呼出箇所 (font.cpp, pixmap.cpp) は bytes.empty() で
//     失敗判定する設計なので、 throw すると挙動が変わる。
//---------------------------------------------------------------------------
class krkrz_storages_loader : public cycfi::elements::resource_loader
{
public:
	std::vector<std::uint8_t> read(std::string_view name) override
	{
		if (name.empty()) return {};
		ttstr path = Utf8ToTtstr(name);
		try {
			tjs_uint64 flen = 0;
			auto buf = TVPReadStream(path.c_str(), &flen);
			if (!buf || flen == 0) return {};
			std::vector<std::uint8_t> out(static_cast<std::size_t>(flen));
			std::memcpy(out.data(), buf.get(), out.size());
			return out;
		} catch (...) {
			// auto-path 解決失敗 / open 失敗 / read エラー等。 silent fail で良い
			// (呼出側 register_font は bytes.empty() を見て return する)。
			return {};
		}
	}

	bool exists(std::string_view name) override
	{
		if (name.empty()) return false;
		ttstr path = Utf8ToTtstr(name);
		try {
			return TVPIsExistentStorage(path);
		} catch (...) {
			return false;
		}
	}
};

//---------------------------------------------------------------------------
// install (idempotent)
//---------------------------------------------------------------------------
std::once_flag g_install_once;

void DoInstall()
{
	auto loader = std::make_shared<krkrz_storages_loader>();
	cycfi::elements::set_resource_loader(loader);
	TVPAddLog(TJS_W("ElementsResourceLoader: installed (Storages-backed VFS)"));
}

//---------------------------------------------------------------------------
// directory 列挙 + register_font ヘルパ
//
// elements の load_fonts_from_directory は std::filesystem を使うため
// ELEMENTS_FILE_IO_SUPPORT=OFF で no-op になる。 代わりに TVPGetStorageListAt
// で列挙して同等処理を行う。 ファイル名から family / weight / slant / stretch
// を推定するロジックは external/elements/lib/src/support/font.cpp の
// parse_font_filename をそのまま移植している。
//---------------------------------------------------------------------------
class CollectingLister : public iTVPStorageLister
{
public:
	std::vector<ttstr> files;
	void TJS_INTF_METHOD Add(const ttstr& file) override
	{
		files.push_back(file);
	}
};

bool HasExt(const std::string& stem_lc, const char* needle)
{
	return stem_lc.find(needle) != std::string::npos;
}

struct FontFileInfo
{
	std::string family;
	cycfi::elements::font_constants::weight_enum  weight  =
		cycfi::elements::font_constants::weight_normal;
	cycfi::elements::font_constants::slant_enum   slant   =
		cycfi::elements::font_constants::slant_normal;
	cycfi::elements::font_constants::stretch_enum stretch =
		cycfi::elements::font_constants::stretch_normal;
};


// "OpenSans" → "Open Sans"。 元 (Elements) 実装と同じ。
std::string ExpandCamelCase(const std::string& name)
{
	std::string result;
	for (std::size_t i = 0; i < name.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(name[i]);
		if (i > 0 && std::isupper(c) &&
		    std::islower(static_cast<unsigned char>(name[i - 1])))
			result += ' ';
		result += name[i];
	}
	return result;
}

// 全文字小文字なら先頭を titlecase する。 krkrz の resource/ 同梱フォントが
// `roboto-regular.ttf` `notosansjp-regular.otf` のように全小文字の規約なため、
// そのままだと family が "roboto" / "notosansjp" になって theme.label_font の
// 既定 "Open Sans" などと噛み合わない。 先頭だけ大文字化して "Roboto" /
// "Notosansjp" に揃える (= match() の lookup key として扱いやすくする)。
std::string TitlecaseIfAllLower(const std::string& s)
{
	if (s.empty()) return s;
	for (unsigned char c : s) {
		if (std::isupper(c)) return s;   // 既に大文字を含むなら触らない
	}
	std::string r = s;
	r[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r[0])));
	return r;
}

// 登録済 family 名を順序保持で蓄積。 EnsureRuntimeInitialized 末尾で
// Elements の theme.label_font 等を上書きする際に load 順で families に並べる。
std::vector<std::string>& RegisteredFamilies()
{
	static std::vector<std::string> v;
	return v;
}

void NoteRegisteredFamily(const std::string& family)
{
	auto& v = RegisteredFamilies();
	if (std::find(v.begin(), v.end(), family) == v.end())
		v.push_back(family);
}

// "Noto Sans JP" / "NotoSansSC" / "Source Han Sans" 等の CJK 向けフォントは
// theme families の末尾に回し、 1byte 系を先頭に積む (1byte 系を primary に
// したいという krkrz 慣習)。 family 名のサフィックス語幹マッチ。
// 確実性は低いが、 ThorVG の FT loader はどのみちグリフ単位で fallback する
// ので、 theme primary を間違えても表示は崩れない (ASCII が CJK 用フォントで
// 描かれるだけ)。
bool IsCJKFamilyName(const std::string& family)
{
	static const char* kCJKMarkers[] = {
		// 言語サフィックス (Noto/Adobe 系)
		" JP", " SC", " TC", " HK", " KR", " CJK",
		"JP", "SC", "TC", "HK", "KR", "CJK",
		// 書体名語幹
		"Han",        // Source Han Sans / Noto Sans Han
		"Mincho", "Gothic", "Kaku",   // 日本語書体名
		"Ming",       // 中国語書体名 (Ming-style)
		"Hangul"      // 韓国語書体名
	};
	for (const char* m : kCJKMarkers) {
		if (family.find(m) != std::string::npos) return true;
	}
	return false;
}

FontFileInfo ParseFontFilename(const std::string& stem)
{
	using namespace cycfi::elements::font_constants;
	FontFileInfo info;

	auto dash = stem.find('-');
	std::string family_part = (dash != std::string::npos) ? stem.substr(0, dash) : stem;
	std::string style_part  = (dash != std::string::npos) ? stem.substr(dash + 1) : "";

	info.family = TitlecaseIfAllLower(ExpandCamelCase(family_part));

	if (info.family.find("Condensed") != std::string::npos)
		info.stretch = condensed;

	std::string style_lc = style_part;
	std::transform(style_lc.begin(), style_lc.end(), style_lc.begin(),
		[](unsigned char c){ return static_cast<char>(std::tolower(c)); });

	if (HasExt(style_lc, "variablefont")) {
		if (HasExt(style_lc, "italic")) info.slant = italic;
		return info;
	}

	if      (HasExt(style_lc, "thin"))       info.weight = thin;
	else if (HasExt(style_lc, "extralight") ||
	         HasExt(style_lc, "extra_light"))info.weight = extra_light;
	else if (HasExt(style_lc, "semibold")  ||
	         HasExt(style_lc, "semi_bold") ||
	         HasExt(style_lc, "demibold"))   info.weight = semi_bold;
	else if (HasExt(style_lc, "extrabold"))  info.weight = extra_bold;
	else if (HasExt(style_lc, "bold"))       info.weight = bold;
	else if (HasExt(style_lc, "black"))      info.weight = black;
	else if (HasExt(style_lc, "medium"))     info.weight = medium;
	else if (HasExt(style_lc, "light"))      info.weight = light;

	if      (HasExt(style_lc, "italic"))  info.slant = italic;
	else if (HasExt(style_lc, "oblique")) info.slant = oblique;

	return info;
}

bool EndsWithLower(const std::string& s, const char* suffix)
{
	std::size_t n = std::strlen(suffix);
	if (s.size() < n) return false;
	for (std::size_t i = 0; i < n; ++i) {
		char c = static_cast<char>(std::tolower(
			static_cast<unsigned char>(s[s.size() - n + i])));
		if (c != suffix[i]) return false;
	}
	return true;
}

void SplitStemExt(const std::string& filename, std::string& stem, std::string& ext_lc)
{
	auto dot = filename.rfind('.');
	if (dot == std::string::npos) {
		stem = filename;
		ext_lc.clear();
		return;
	}
	stem = filename.substr(0, dot);
	ext_lc = filename.substr(dot);
	std::transform(ext_lc.begin(), ext_lc.end(), ext_lc.begin(),
		[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
}

std::string TtstrToUtf8(const ttstr& s)
{
	std::string out;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(out, ts);
	return out;
}

} // anonymous

//---------------------------------------------------------------------------
// 公開 API
//---------------------------------------------------------------------------
void TVPInstallElementsResourceLoader()
{
	std::call_once(g_install_once, DoInstall);
}

void TVPRegisterElementsFontsFromStorageDir(const ttstr& dir)
{
	if (dir.IsEmpty()) return;

	// install を保証 (呼出順序にゆるみがあっても安全に動くように)
	TVPInstallElementsResourceLoader();

	CollectingLister lister;
	try {
		// TVPGetStorageListAt は media driver 経由でディレクトリリストを返す
		// (file: media なら std::filesystem 等、 XP3 media なら archive index)。
		// 内部で path 正規化される。
		TVPGetStorageListAt(dir, &lister);
	} catch (...) {
		// dir が存在しない / アクセス権なし等は warning だけ出して終了
		TVPAddLog(ttstr(TJS_W("ElementsResourceLoader: font dir not found: ")) + dir);
		return;
	}

	if (lister.files.empty()) {
		TVPAddLog(ttstr(TJS_W("ElementsResourceLoader: no fonts in: ")) + dir);
		return;
	}

	// dir の末尾を `/` で揃える (Storages convention)
	ttstr base = dir;
	if (!base.IsEmpty()) {
		tjs_char last = base.c_str()[base.length() - 1];
		if (last != TJS_W('/') && last != TJS_W('\\'))
			base += TJS_W("/");
	}

	int registered = 0;
	for (const auto& entry : lister.files) {
		std::string fname_utf8 = TtstrToUtf8(entry);
		std::string stem, ext_lc;
		SplitStemExt(fname_utf8, stem, ext_lc);
		if (ext_lc != ".ttf" && ext_lc != ".otf") continue;

		FontFileInfo info = ParseFontFilename(stem);

		ttstr full = base + entry;
		std::string full_utf8 = TtstrToUtf8(full);

		// register_font は ThorVG が読み取った embedded family name を返す。
		// 取れたらそちらを優先して theme に積む。 取れなければ filename 由来
		// の info.family が残るので、 そちらを使う。
		std::string embedded = cycfi::elements::register_font(
			info.family, full_utf8, info.weight, info.slant, info.stretch);
		const std::string& canonical = !embedded.empty() ? embedded : info.family;
		NoteRegisteredFamily(canonical);
		++registered;
	}

	TVPAddLog(ttstr(TJS_W("ElementsResourceLoader: registered ")) +
		ttstr((tjs_int)registered) + TJS_W(" font(s) from ") + dir);
}

ttstr TVPRegisterElementsFont(const ttstr& family, const ttstr& path,
	int weight, int slant, int stretch)
{
	if (family.IsEmpty() || path.IsEmpty()) return ttstr();

	TVPInstallElementsResourceLoader();

	using namespace cycfi::elements::font_constants;
	auto w = static_cast<weight_enum>(weight);
	auto s = static_cast<slant_enum>(slant);
	auto t = static_cast<stretch_enum>(stretch);

	std::string family_utf8 = TtstrToUtf8(family);
	std::string path_utf8   = TtstrToUtf8(path);

	std::string embedded =
		cycfi::elements::register_font(family_utf8, path_utf8, w, s, t);
	// embedded family が取れたらそちらを優先 (caller が知りたいのは「実際に
	// 登録された family 名」)。 取れなければ caller 指定をそのまま使う。
	const std::string& canonical = !embedded.empty() ? embedded : family_utf8;
	NoteRegisteredFamily(canonical);
	TVPAddLog(ttstr(TJS_W("ElementsResourceLoader: registered font: ")) +
		Utf8ToTtstr(canonical) + TJS_W(" <- ") + path);
	return Utf8ToTtstr(canonical);
}

//---------------------------------------------------------------------------
// Theme override
//
// Elements の default theme は label_font="Open Sans" / heading_font="Roboto" /
// system_font="Lucida Grande" 等を参照していて、 これらが krkrz で実際に登録
// された family と一致しないと「テキストが全く描画されない」状態になる。
// EnsureRuntimeInitialized 末尾でここを呼ぶと、 これまでに登録した family を
// load 順で並べた families 文字列を theme の全フォントスロットに当てはめる。
// per-codepoint fallback は ThorVG FT loader が自動でやるので、 families に
// 先頭から順に並べておけば多言語表示も自然に動く。
//
// 注意: `font_descr::_families` は std::string_view なので、 元の std::string
// は theme と同じ寿命を持つ必要がある (一時オブジェクトに作ると dangling)。
// プロセス寿命の static で保持する。
//---------------------------------------------------------------------------
static std::string& ThemeFamiliesStorage()
{
	static std::string s;
	return s;
}

static void ApplyThemeFromStoredFamilies()
{
	const std::string& fams = ThemeFamiliesStorage();
	if (fams.empty()) return;

	std::string_view fams_view(fams);   // static の寿命を持つ
	auto thm = cycfi::elements::get_theme();
	thm.label_font       = cycfi::elements::font_descr{fams_view, thm.label_font._size};
	thm.heading_font     = cycfi::elements::font_descr{fams_view, thm.heading_font._size};
	thm.text_box_font    = cycfi::elements::font_descr{fams_view, thm.text_box_font._size};
	thm.mono_spaced_font = cycfi::elements::font_descr{fams_view, thm.mono_spaced_font._size};
	thm.system_font      = cycfi::elements::font_descr{fams_view, thm.system_font._size};
	// icon_font は "elements_basic" 専用フォント (✓ ▼ 等の独自グリフ)。
	// krkrz には組まないので、 ここでは触らない (元の "elements_basic" のまま)。
	cycfi::elements::set_theme(thm);
}

void TVPApplyRegisteredFontsToElementsTheme()
{
	const auto& fams = RegisteredFamilies();
	if (fams.empty()) return;

	// 1byte (= Latin/ASCII 主体) 系を先頭、 CJK 系を後尾に並べる。 load 順は
	// 同じ群内では維持する (stable partition 相当)。 これにより
	// theme.label_font の primary は Roboto 等の Latin 系になり、 日本語等は
	// ThorVG の per-codepoint fallback で CJK 系フォントに自動的に切替わる。
	std::vector<const std::string*> latin;
	std::vector<const std::string*> cjk;
	for (const auto& f : fams) {
		(IsCJKFamilyName(f) ? cjk : latin).push_back(&f);
	}

	std::string& joined = ThemeFamiliesStorage();
	joined.clear();
	bool first = true;
	auto append = [&](const std::string* s) {
		if (!first) joined += ", ";
		joined += *s;
		first = false;
	};
	for (auto* s : latin) append(s);
	for (auto* s : cjk)   append(s);

	ApplyThemeFromStoredFamilies();

	TVPAddLog(ttstr(TJS_W("ElementsResourceLoader: theme fonts set to: ")) +
		Utf8ToTtstr(joined));
}

void TVPSetElementsDefaultFontFamily(const ttstr& families)
{
	if (families.IsEmpty()) return;
	ThemeFamiliesStorage() = TtstrToUtf8(families);
	ApplyThemeFromStoredFamilies();

	TVPAddLog(ttstr(TJS_W("ElementsResourceLoader: theme fonts set to: ")) + families);
}

ttstr TVPGetElementsDefaultFontFamily()
{
	const std::string& s = ThemeFamiliesStorage();
	if (s.empty()) return ttstr();
	return Utf8ToTtstr(s);
}
