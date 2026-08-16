//---------------------------------------------------------------------------
//! @file
//! @brief GDI 経由でインストール済みフォントのバイト列を取得 (glyphware 用)
/**
 * @note WINVER 限定。tNativeFreeTypeFace と同じ GetFontData 機構でシステム
 *       フォント (addFont が AddFontMemResourceEx で登録したファイルパスを
 *       持たないフォント含む) の全バイトをメモリへスナップショットし、
 *       glyphware の memory-face の裏付けにする。TTC は name テーブル一致で
 *       face index を特定する。
 */
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include "GlyphwareGDIFont.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4819)
#endif
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <map>
#include <mutex>
#include <vector>

extern FT_Library FreeTypeLibrary;
extern void TVPInitializeFont();

#define TVP_TT_TABLE_ttcf  (('t' << 0) + ('t' << 8) + ('c' << 16) + ('f' << 24))
#define TVP_TT_TABLE_name  (('n' << 0) + ('a' << 8) + ('m' << 16) + ('e' << 24))

namespace {
std::mutex g_gdiFontMutex;
// Cache by requested name. An entry with empty bytes is a negative result.
std::map<tjs_string, tTVPGDIFontData> g_gdiFontCache;

// Whether a font with exactly `name` is installed. EnumFontFamiliesEx fires the
// callback only for a real facename match (including localized names), so unlike
// CreateFontIndirect it never reports success for an unknown name via GDI font
// substitution. This is what lets us reject "not installed" (e.g. the color
// emoji font) instead of silently getting a substituted default face.
static int CALLBACK GdiEnumProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
	*reinterpret_cast<bool*>(lp) = true;
	return 0;   // one match is enough
}
static bool GdiFontExists(HDC dc, const wchar_t* name) {
	LOGFONTW lf;
	memset(&lf, 0, sizeof(lf));
	lf.lfCharSet = DEFAULT_CHARSET;
	wcsncpy(lf.lfFaceName, name, LF_FACESIZE - 1);
	bool found = false;
	EnumFontFamiliesExW(dc, &lf, reinterpret_cast<FONTENUMPROCW>(GdiEnumProc),
	                    reinterpret_cast<LPARAM>(&found), 0);
	return found;
}
} // namespace

const tTVPGDIFontData* TVPGetGDIFontData(const tjs_string& fontname) {
	std::lock_guard<std::mutex> lk(g_gdiFontMutex);

	auto it = g_gdiFontCache.find(fontname);
	if (it != g_gdiFontCache.end())
		return it->second.bytes.empty() ? nullptr : &it->second;

	// insert a negative entry up-front; std::map node addresses are stable so
	// returning &slot stays valid across later inserts.
	tTVPGDIFontData& slot = g_gdiFontCache[fontname];
	slot.faceIndex = 0;

	TVPInitializeFont();

	HDC dc = GetDC(0);
	if (!dc) return nullptr;

	// reject names GDI doesn't actually have (would substitute a default face).
	if (!GdiFontExists(dc, reinterpret_cast<const wchar_t*>(fontname.c_str()))) {
		ReleaseDC(0, dc);
		return nullptr;
	}

	LOGFONT l;
	memset(&l, 0, sizeof(l));
	l.lfHeight = -12;
	l.lfWeight = FW_NORMAL;
	l.lfCharSet = DEFAULT_CHARSET;
	l.lfOutPrecision = OUT_TT_PRECIS;   // prefer a real TrueType/OpenType face
	l.lfQuality = DEFAULT_QUALITY;
	l.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	TJS_strcpy((tjs_char*)l.lfFaceName, fontname.c_str());
	l.lfFaceName[LF_FACESIZE - 1] = TJS_W('\0');

	HFONT newfont = CreateFontIndirect(&l);
	HFONT oldfont = static_cast<HFONT>(SelectObject(dc, newfont));

	std::vector<unsigned char> nameTag;
	std::string fullbytes;
	int faceIndex = 0;
	bool ok = false;

	do {
		// 'name' タグが取れる = GetFontData で扱える TrueType/OpenType フォント
		DWORD nsz = GetFontData(dc, TVP_TT_TABLE_name, 0, NULL, 0);
		if (nsz == GDI_ERROR || nsz == 0) break;
		nameTag.resize(nsz);
		if (GetFontData(dc, TVP_TT_TABLE_name, 0, nameTag.data(), nsz) == GDI_ERROR) break;

		// TTC 判定 ('ttcf' が取れればコレクション)
		bool isTTC = false;
		unsigned char probe[4];
		if (GetFontData(dc, TVP_TT_TABLE_ttcf, 0, probe, 1) == 1) isTTC = true;

		const DWORD tag = isTTC ? TVP_TT_TABLE_ttcf : 0;
		DWORD fsz = GetFontData(dc, tag, 0, NULL, 0);
		if (fsz == GDI_ERROR || fsz == 0) break;
		fullbytes.resize(fsz);
		if (GetFontData(dc, tag, 0, (void*)fullbytes.data(), fsz) == GDI_ERROR) break;

		// TTC 内で GDI が選択した face を name タグ一致で特定する
		if (isTTC && FreeTypeLibrary) {
			for (int idx = 0; idx <= 64; ++idx) {
				FT_Face face = nullptr;
				if (FT_New_Memory_Face(FreeTypeLibrary,
						reinterpret_cast<const FT_Byte*>(fullbytes.data()),
						static_cast<FT_Long>(fullbytes.size()), idx, &face))
					break;   // これ以上 face は無い
				FT_ULong len = 0;
				bool matched = false;
				if (!FT_Load_Sfnt_Table(face, TTAG_name, 0, NULL, &len) &&
				    len == nameTag.size()) {
					std::vector<unsigned char> ftname(len);
					if (!FT_Load_Sfnt_Table(face, TTAG_name, 0, ftname.data(), &len) &&
					    memcmp(ftname.data(), nameTag.data(), len) == 0)
						matched = true;
				}
				FT_Done_Face(face);
				if (matched) { faceIndex = idx; break; }
			}
		}
		ok = true;
	} while (false);

	if (oldfont) SelectObject(dc, oldfont);
	if (newfont) DeleteObject(newfont);
	ReleaseDC(0, dc);

	if (!ok) return nullptr;   // slot は空のまま (negative cache)
	slot.bytes = std::move(fullbytes);
	slot.faceIndex = faceIndex;
	return &slot;
}

#endif // KRKRZ_USE_GLYPHWARE
