#include "tjsCommHead.h"
#include "GlyphwareFontRasterizer.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include "GlyphwareHost.h"      // resolve / effective key / build chain
#include "FontVariations.h"     // 可変軸の実効座標 (Font.weight / Font.variations)
#include "LayerBitmapIntf.h"    // tTVPNativeBaseBitmap::GetFont
#include "CharacterSet.h"       // TVPUtf16ToUtf8
#include "StorageIntf.h"        // TVPGetResourcePath (同梱リソースの置き場)
#include "MsgIntf.h"            // TVPFontRasterizeError
#include "glyphware/glyphware.h"
#ifdef __WINVER__
#include "TVPSysFont.h"         // TVPGetFontList
#endif

#include <cmath>
#include <string>

// emoji-mode helpers + font registration/listing shared with the FreeType path
extern tjs_int TVPResolveEmojiMode(tjs_int fontmode);
extern const tjs_char* TVPGetEmojiFaceName(tjs_int mode);
extern bool TVPAddFontToFreeType(const ttstr& storage, std::vector<tjs_string>* faces);
extern void TVPGetFontListFromFreeType(std::vector<ttstr>& list, tjs_uint32 flags, const tTVPFont& font);

static const double GW_PI = 3.14159265358979323846;

//---------------------------------------------------------------------------
GlyphwareFontRasterizer::GlyphwareFontRasterizer()
	: RefCount(0), LastBitmap(nullptr), PixelSize(0), Ascent(0),
	  ChainEmojiMode(TVP_EMOJI_NONE), HasFont(false) {
	AddRef();
}
GlyphwareFontRasterizer::~GlyphwareFontRasterizer() {}
void GlyphwareFontRasterizer::AddRef() { RefCount++; }
void GlyphwareFontRasterizer::Release() {
	RefCount--;
	LastBitmap = nullptr;
	if (RefCount == 0) delete this;
}
//---------------------------------------------------------------------------
void GlyphwareFontRasterizer::RebuildChain(const tTVPFont& font) {
	Chain.clear();
	std::string keyU8;
	TVPUtf16ToUtf8(keyU8, tjs_string(font.Face.c_str()));
	keyU8 = TVPGlyphwareEffectiveKey(keyU8);   // empty -> engine default face chain

	// append the emoji fallback face. Use the configured emoji face name when it
	// resolves (fonts.json / addFont / GDI); otherwise fall back to the embedded
	// mono emoji font so emoji still render as monochrome glyphs rather than
	// tofu when the (larger, non-embedded) color emoji font is unavailable.
	tjs_int emode = TVPResolveEmojiMode(font.EmojiMode);
	if (emode == TVP_EMOJI_MONO || emode == TVP_EMOJI_COLOR) {
		std::string emojiKey;
		const tjs_char* ename = TVPGetEmojiFaceName(emode);
		std::string e;
		if (ename && ename[0]) TVPUtf16ToUtf8(e, tjs_string(ename));
		if (!e.empty() && TVPGlyphwareFontNameAvailable(e))
			emojiKey = e;
		else {
			// 同梱モノクロ絵文字。リソースの置き場はプラットフォームで変わる
			// (resource://./ / file://./resource/) ので直書きしない。
			std::string resbase;
			TVPUtf16ToUtf8(resbase, tjs_string(TVPGetResourcePath().c_str()));
			emojiKey = resbase + "notoemoji-regular.ttf";
		}
		if (!keyU8.empty()) keyU8 += ",";
		keyU8 += emojiKey;
	}
	TVPGlyphwareBuildChain(keyU8, Chain);

	// unresolvable font name -> fall back to the engine default face chain so an
	// unknown Font.face doesn't leave an empty chain (which would throw on draw).
	if (Chain.empty()) {
		std::string def = TVPGlyphwareEffectiveKey("");
		if (!def.empty()) TVPGlyphwareBuildChain(def, Chain);
	}
}
//---------------------------------------------------------------------------
void GlyphwareFontRasterizer::ApplyFont(const tTVPFont& font) {
	CurrentFont = font;
	RebuildChain(font);
	ChainEmojiMode = TVPResolveEmojiMode(font.EmojiMode);

	// 可変軸の適用: Font.variations (+ Font.weight) の実効座標を、連鎖の各 face が
	// 持つ同名軸にだけ適用する (private face の LRU 経由。共有 face は不変)。
	// Font.defaultUseVarStyle 有効時は bold/italic を軸へ自動マッピングし、
	// 軸で表現できたぶんの合成スタイルを無効化する。
	SynthBold = (font.Flags & TVP_TF_BOLD) != 0;
	SynthItalic = (font.Flags & TVP_TF_ITALIC) != 0;
	{
		std::vector<tTVPFontAxisCoord> coords;
		TVPFontGetEffectiveVarCoords(font, coords);
		TVPGlyphwareAutoStyleCoords(Chain.empty() ? nullptr : Chain[0],
		                            coords, SynthBold, SynthItalic);
		TVPGlyphwareApplyVariationsToChain(Chain, coords);
	}

	PixelSize = font.Height < 0 ? -font.Height : font.Height;
	if (PixelSize <= 0) PixelSize = 1;
	Ascent = 0;
	for (auto& f : Chain) if (f) f->setPixelSize(PixelSize);
	if (!Chain.empty() && Chain[0]) {
		// baseline (ascent) must match the FreeType rasterizer's
		// tFreeTypeFace::GetAscent() = ascender * ppem / unitsPerEm (truncating
		// integer division). lineMetrics().ascent comes from FreeType's
		// size->metrics.ascender which is FT_PIX_CEIL'ed, so using it directly
		// draws glyphs up to 1px lower than the FreeType path.
		glyphware::LineMetrics lm = Chain[0]->lineMetrics();
		if (lm.unitsPerEm > 0 && lm.ppemY > 0)
			Ascent = static_cast<tjs_int>(lm.ascenderUnits) * static_cast<tjs_int>(lm.ppemY)
			         / static_cast<tjs_int>(lm.unitsPerEm);
		else
			Ascent = static_cast<tjs_int>(std::lround(lm.ascent));  // bitmap-only faces etc.
	}
	HasFont = true;
	LastBitmap = nullptr;
}
//---------------------------------------------------------------------------
void GlyphwareFontRasterizer::ApplyFont(tTVPNativeBaseBitmap* bmp, bool force) {
	if (bmp != LastBitmap || force) {
		ApplyFont(bmp->GetFont());
		LastBitmap = bmp;
	}
}
//---------------------------------------------------------------------------
bool GlyphwareFontRasterizer::ResolveGlyph(tjs_uint32 codepoint, int& faceIdx,
                                           tjs_uint32& gid, bool preferLast) const {
	const int n = static_cast<int>(Chain.size());
	for (int k = 0; k < n; ++k) {
		// preferLast (VS16): scan fallback/emoji faces first, then earlier ones.
		int i = preferLast ? (n - 1 - k) : k;
		if (!Chain[i]) continue;
		tjs_uint32 g = Chain[i]->glyphIndex(static_cast<char32_t>(codepoint));
		if (g) { faceIdx = i; gid = g; return true; }
	}
	return false;
}
//---------------------------------------------------------------------------
void GlyphwareFontRasterizer::GetTextExtent(tjs_uint32 ch, tjs_int& w, tjs_int& h) {
	w = h = 0;
	int fi; tjs_uint32 gid;
	if (ResolveGlyph(ch, fi, gid)) {
		Chain[fi]->setPixelSize(PixelSize);
		glyphware::GlyphMetrics gm;
		// pass bold/italic so getTextWidth (which drives this) matches the drawn
		// advance (GetBitmap emboldens); otherwise measured != rendered width.
		if (Chain[fi]->glyphMetrics(gid, gm, SynthBold, SynthItalic)) {
			w = static_cast<tjs_int>(std::lround(gm.advanceX));
			h = static_cast<tjs_int>(std::lround(gm.advanceY));
		}
	}
}
//---------------------------------------------------------------------------
tjs_int GlyphwareFontRasterizer::GetAscentHeight() { return Ascent; }
//---------------------------------------------------------------------------
tTVPCharacterData* GlyphwareFontRasterizer::GetBitmap(
    const tTVPFontAndCharacterData& font, tjs_int aofsx, tjs_int aofsy) {
	// ensure the chain matches the requested font (the draw pipeline usually
	// ApplyFont()s first, but be safe on a direct cache-miss call).
	if (!HasFont || CurrentFont != font.Font) ApplyFont(font.Font);
	if (Chain.empty()) TVPThrowExceptionMessage(TVPFontRasterizeError);

	const tTVPFont& F = font.Font;

	// per-character emoji presentation (VS15/VS16), matching the FreeType path:
	//   VS16 -> prefer the emoji fallback face (COLOR if base has no emoji mode)
	//   VS15 -> force the base/text glyph (no emoji fallback)
	tjs_int baseMode = TVPResolveEmojiMode(F.EmojiMode);
	tjs_int effMode = baseMode;
	bool preferEmoji = false;
	if (font.EmojiPresentation == TVP_EMOJI_PRESENTATION_EMOJI) {
		effMode = (baseMode == TVP_EMOJI_NONE) ? TVP_EMOJI_COLOR : baseMode;
		preferEmoji = true;
	} else if (font.EmojiPresentation == TVP_EMOJI_PRESENTATION_TEXT) {
		effMode = TVP_EMOJI_NONE;
	}
	bool reapplied = false;
	if (effMode != ChainEmojiMode) {
		tTVPFont f = F;
		f.EmojiMode = effMode;
		ApplyFont(f);   // rebuild the chain for the effective mode
		reapplied = true;
	}

	int faceIdx = 0; tjs_uint32 gid = 0;
	ResolveGlyph(font.Character, faceIdx, gid, preferEmoji);   // miss -> primary, .notdef
	std::shared_ptr<glyphware::Face> face = Chain[faceIdx];    // hold: chain may rebuild on restore
	face->setPixelSize(PixelSize);
	const tjs_int baseline = Ascent;

	// 合成 bold/italic は ApplyFont が計算した実効値 (defaultUseVarStyle で
	// 軸へマッピングされたぶんは落ちている) を使う。
	const bool bold = SynthBold;
	const bool italic = SynthItalic;
	const bool underline = (F.Flags & TVP_TF_UNDERLINE) != 0;
	const bool strikeout = (F.Flags & TVP_TF_STRIKEOUT) != 0;
	// request color bitmaps only in the color-emoji mode (matching the classic
	// path's FT_LOAD_COLOR); otherwise text uses outlines (FT_LOAD_NO_BITMAP) so
	// embedded MONO strikes aren't shown.
	const bool wantColor = (effMode == TVP_EMOJI_COLOR);

	glyphware::GlyphMetrics gm;
	int cx = 0;
	if (face->glyphMetrics(gid, gm, bold, italic)) cx = static_cast<int>(std::lround(gm.advanceX));

	glyphware::GlyphBitmap gb;
	bool haveBmp = face->glyphBitmap(gid, wantColor, gb, bold, italic) &&
	               gb.buffer && gb.width > 0 && gb.rows > 0;

	tGlyphMetrics metrics;
	metrics.CellIncX = cx;
	metrics.CellIncY = 0;

	tTVPCharacterData* data;
	if (haveBmp && gb.format == glyphware::BitmapFormat::BGRA) {
		// color glyph (emoji). glyphware returns premultiplied BGRA; the engine's
		// TVPAlphaBlend expects non-premultiplied, so un-premultiply the copy.
		data = new tTVPCharacterData(gb.buffer, gb.pitch / 4, gb.left,
		                             Ascent - gb.top, gb.width, gb.rows, metrics, true);
		tjs_uint8* dp = data->GetData();
		const tjs_int cpitch = data->Pitch;
		for (tjs_uint yy = 0; yy < data->BlackBoxY; ++yy) {
			tjs_uint8* p = dp + cpitch * yy;
			for (tjs_uint xx = 0; xx < data->BlackBoxX; ++xx) {
				tjs_uint a = p[3];
				if (a == 0) { p[0] = p[1] = p[2] = 0; }
				else if (a < 255) {
					p[0] = static_cast<tjs_uint8>((p[0] * 255 + (a >> 1)) / a);
					p[1] = static_cast<tjs_uint8>((p[1] * 255 + (a >> 1)) / a);
					p[2] = static_cast<tjs_uint8>((p[2] * 255 + (a >> 1)) / a);
				}
				p += 4;
			}
		}
		// Gray stays default (do NOT set 256): the blend dispatch checks Gray==256
		// before the FullColored branch.
	} else if (haveBmp) {
		// grayscale coverage (8-bit, 0..255)
		data = new tTVPCharacterData(gb.buffer, gb.pitch, gb.left,
		                             Ascent - gb.top, gb.width, gb.rows, metrics);
		data->Gray = 256;
	} else {
		// empty glyph (e.g. space): advance only, no pixels
		data = new tTVPCharacterData(static_cast<const tjs_uint8*>(nullptr), 0,
		                             0, 0, 0, 0, metrics);
		data->Pitch = 0;
		data->Gray = 256;
	}

	// bake underline / strikeout into the glyph cell (gray only — matches the
	// FreeType path; color glyphs skip it). Positions are baseline-relative from
	// glyphware line metrics; AddHorizontalLine needs the pre-angle CellIncX.
	if ((underline || strikeout) && !data->FullColored) {
		glyphware::LineMetrics lm = face->lineMetrics();
		if (underline) {
			int pos = baseline + static_cast<int>(std::lround(lm.underlineOffset));
			int th = static_cast<int>(std::lround(lm.underlineThickness));
			if (th < 1) th = 1;
			if (pos >= 0) data->AddHorizontalLine(pos, th, 255);
		}
		if (strikeout) {
			int pos = baseline + static_cast<int>(std::lround(lm.strikeoutOffset));
			int th = static_cast<int>(std::lround(lm.strikeoutThickness));
			if (th < 1) th = 1;
			if (pos >= 0) data->AddHorizontalLine(pos, th, 255);
		}
	}

	// restore the chain to the requested font (data is an independent copy).
	if (reapplied) ApplyFont(F);

	// angle: rotate the advance only (glyph pixels stay upright — the classic
	// FreeType path does the same; glyphware's setTransform rotation is reserved
	// for drawShapedText).
	if (F.Angle == 0) {
		data->Metrics.CellIncX = cx;
		data->Metrics.CellIncY = 0;
	} else if (F.Angle == 2700) {
		data->Metrics.CellIncX = 0;
		data->Metrics.CellIncY = cx;
	} else {
		double a = F.Angle * (GW_PI / 1800.0);
		data->Metrics.CellIncX = static_cast<tjs_int>(std::cos(a) * cx);
		data->Metrics.CellIncY = static_cast<tjs_int>(-std::sin(a) * cx);
	}

	data->Antialiased = font.Antialiased;
	data->Blured = font.Blured;
	data->BlurWidth = font.BlurWidth;
	data->BlurLevel = font.BlurLevel;
	if (font.Blured && !data->FullColored) data->Blur();
	return data;
}
//---------------------------------------------------------------------------
void GlyphwareFontRasterizer::GetGlyphDrawRect(const ttstr& text, tTVPRect& area) {
	area.left = area.top = area.right = area.bottom = 0;
	if (Chain.empty()) return;
	for (auto& f : Chain) if (f) f->setPixelSize(PixelSize);
	const tjs_int baseline = Ascent;

	tjs_int penx = 0;
	bool any = false;
	const tjs_char* p = text.c_str();
	tjs_uint len = text.length();
	for (tjs_uint i = 0; i < len; ++i) {
		tjs_uint32 ch = static_cast<tjs_uint32>(static_cast<tjs_uint16>(p[i]));
		if (ch >= 0xD800 && ch <= 0xDBFF && (i + 1) < len) {
			tjs_uint32 lo = static_cast<tjs_uint32>(static_cast<tjs_uint16>(p[i + 1]));
			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				ch = 0x10000u + ((ch - 0xD800u) << 10) + (lo - 0xDC00u);
				++i;
			}
		}
		int fi; tjs_uint32 gid;
		if (!ResolveGlyph(ch, fi, gid)) continue;
		glyphware::GlyphMetrics gm;
		if (!Chain[fi]->glyphMetrics(gid, gm, SynthBold, SynthItalic)) continue;
		tTVPRect rt;
		rt.left = penx + static_cast<tjs_int>(std::lround(gm.bearingX));
		rt.top = baseline - static_cast<tjs_int>(std::lround(gm.bearingY));
		rt.right = rt.left + static_cast<tjs_int>(std::lround(gm.width));
		rt.bottom = rt.top + static_cast<tjs_int>(std::lround(gm.height));
		if (!any) { area = rt; any = true; } else area.do_union(rt);
		penx += static_cast<tjs_int>(std::lround(gm.advanceX));
	}
}
//---------------------------------------------------------------------------
bool GlyphwareFontRasterizer::AddFont(const ttstr& storage, std::vector<tjs_string>* faces) {
	// font registration stays shared with the FreeType/FontSystem registry so
	// name resolution (fonts.json / addFont) works for glyphware too.
	return TVPAddFontToFreeType(storage, faces);
}
//---------------------------------------------------------------------------
void GlyphwareFontRasterizer::GetFontList(std::vector<ttstr>& list, tjs_uint32 flags,
                                          const tTVPFont& font) {
#ifdef __WINVER__
	TVPGetFontList(list, flags, font);
#endif
	TVPGetFontListFromFreeType(list, flags, font);
}
//---------------------------------------------------------------------------

#endif // KRKRZ_USE_GLYPHWARE
