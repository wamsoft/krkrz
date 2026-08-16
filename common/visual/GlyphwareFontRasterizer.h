// GlyphwareFontRasterizer — a FontRasterizer backed by the glyphware font
// engine (FreeType + HarfBuzz). It is an opt-in drop-in for the FreeType path:
// selected via Font.rasterizer, it produces per-glyph tTVPCharacterData the
// same way the classic FreeType rasterizer does, so the existing glyph cache,
// blend/shadow machinery and cell-stepping DrawTextMultiple keep working
// unchanged. The GDI rasterizer (WINVER default) is untouched.
//
// Faithful to the FreeType path: glyphs are NOT pixel-rotated for Font.angle
// (the classic FreeType path rotates only the advance); complex-script shaping
// / BiDi are intentionally NOT applied here (drawText is per-codepoint) — use
// drawShapedText for shaped/BiDi text.
#ifndef __GLYPHWARE_FONT_RASTERIZER_H__
#define __GLYPHWARE_FONT_RASTERIZER_H__

#ifdef KRKRZ_USE_GLYPHWARE

#include "tjsCommHead.h"
#include "CharacterData.h"
#include "FontRasterizer.h"

#include <memory>
#include <vector>

namespace glyphware { class Face; }

class GlyphwareFontRasterizer : public FontRasterizer {
	tjs_int RefCount;
	tTVPFont CurrentFont;
	class tTVPNativeBaseBitmap* LastBitmap;
	std::vector<std::shared_ptr<glyphware::Face>> Chain;  // primary + fallbacks
	tjs_int PixelSize;      // current face pixel size (abs(Font.Height))
	tjs_int Ascent;        // primary-face ascent at PixelSize (common baseline)
	tjs_int ChainEmojiMode; // effective emoji mode the current Chain was built for
	bool HasFont;

	void RebuildChain(const tTVPFont& font);
	// codepoint -> (chain face index, glyph id); returns false if none cover it.
	// preferLast tries fallback (emoji) faces first — used for VS16 (U+FE0F).
	bool ResolveGlyph(tjs_uint32 codepoint, int& faceIdx, tjs_uint32& gid,
	                  bool preferLast = false) const;

public:
	GlyphwareFontRasterizer();
	virtual ~GlyphwareFontRasterizer();
	void AddRef();
	void Release();
	void ApplyFont(class tTVPNativeBaseBitmap* bmp, bool force);
	void ApplyFont(const struct tTVPFont& font);
	void GetTextExtent(tjs_uint32 ch, tjs_int& w, tjs_int& h);
	tjs_int GetAscentHeight();
	tTVPCharacterData* GetBitmap(const tTVPFontAndCharacterData& font, tjs_int aofsx, tjs_int aofsy);
	void GetGlyphDrawRect(const ttstr& text, struct tTVPRect& area);
	bool AddFont(const ttstr& storage, std::vector<tjs_string>* faces);
	void GetFontList(std::vector<ttstr>& list, tjs_uint32 flags, const struct tTVPFont& font);
};

#endif // KRKRZ_USE_GLYPHWARE
#endif // __GLYPHWARE_FONT_RASTERIZER_H__
