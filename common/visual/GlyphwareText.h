// GlyphwareText — draw text into an engine bitmap via the glyphware font engine
// (layout + BiDi + shaping + fallback). These are the shaped-text entries
// (Layer.drawShapedText and friends); they coexist with the classic
// FreeType/GDI drawText path.
#pragma once
#ifdef KRKRZ_USE_GLYPHWARE

#include "tjsCommHead.h"

#include <memory>
#include <vector>

class tTVPBaseBitmap;
struct tTVPFont;
namespace glyphware { class Face; }

// Text style for the shaped-text entries, resolved from an engine font
// (Layer.font or a standalone Font object): `fontKey` comes from Font.face
// (a fonts.json declared name / storage path / installed font name; may be
// comma-separated for a fallback chain), `size` from Font.height (px), the
// styling flags from Font.bold etc., `angle` from Font.angle.
struct tTVPShapedTextStyle
{
	ttstr fontKey;
	tjs_int size = 0;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;
	tjs_int angle = 0;   // 1/10 deg, CCW (Font.angle). Ignored by DrawTextArea.
	// variable-font axes (Font.weight / Font.variations). Applied to the faces
	// of the fallback chain that actually have the same-named axes; `weight`
	// merges in as wght unless `variations` already names wght.
	tjs_int weight = -1;      // 100-900, -1 = unspecified
	ttstr variations;         // normalized "wdth=87.5,wght=700"
};

// Fill `out` from an engine font descriptor.
void TVPShapedTextStyleFromFont(tTVPShapedTextStyle& out, const tTVPFont& font);

// Font resolution shared by every glyphware text entry: `fontKey` -> fallback
// chain (first covering face per codepoint wins), with the variable-font axes
// applied to each face that has them. `bold`/`italic` come back as the
// SYNTHETIC styling still to be applied — defaultUseVarStyle maps them onto the
// axes instead, and drops them here when it does. All faces are left at
// `style.size` pixels. false when the style names no usable font.
struct tTVPShapedFontChain {
	std::vector<std::shared_ptr<glyphware::Face>> chain;
	bool bold = false;
	bool italic = false;
};
bool TVPGlyphwareResolveChain(tTVPShapedFontChain& out, const tTVPShapedTextStyle& style);

// Draw `text` (UTF-16, single line) into `dest` (32bpp BGRA) with its baseline
// origin at (x, y) and color 0xAARRGGBB (alpha 0 is treated as opaque).
// `base`: 0=auto, 1=LTR, 2=RTL. `count` >= 0 limits drawing to the first
// `count` clusters in logical (reading) order — a cluster is one draw-time
// unit: a ligature, a base + combining marks, an emoji ZWJ sequence (the same
// unit TVPGlyphwareTextCount counts). The full line is shaped first, so a
// typewriter-style reveal keeps contextual forms (e.g. Arabic) stable.
// Returns the advance width (px) of the drawn portion, or -1 on failure.
int TVPGlyphwareDrawText(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
                         const ttstr& text, tjs_uint32 color,
                         const tTVPShapedTextStyle& style, tjs_int base = 0,
                         tjs_int count = -1);

// Text measurement (parallel to GetTextExtent / GetGlyphDrawRect). All values
// in pixels, relative to a baseline origin at (0,0): `left/top/right/bottom` is
// the tight ink bounding box (top is negative = above the baseline); `advance`
// is the pen advance width; `ascent`/`descent` are the primary face's line
// metrics; `count` is the draw-time cluster count (see TVPGlyphwareTextCount).
struct tTVPGlyphwareMetrics {
	int advance = 0;
	int left = 0, top = 0, right = 0, bottom = 0;
	int ascent = 0, descent = 0;
	int count = 0;
};

// Measure `text` with the given style. Returns false on failure.
bool TVPGlyphwareMeasureText(tTVPGlyphwareMetrics& out, const ttstr& text,
                             const tTVPShapedTextStyle& style, tjs_int base = 0);

// Count the draw-time clusters of `text` — the unit the draw entries' `count`
// parameter limits. Newlines split the text into lines and are themselves not
// counted. Returns -1 on failure.
int TVPGlyphwareTextCount(const ttstr& text, const tTVPShapedTextStyle& style,
                          tjs_int base = 0);

// Result of TVPGlyphwareDrawTextArea: consumed height (px, from the rect top
// to the bottom of the last drawn line), number of lines drawn (wrapped or
// explicit), and number of clusters actually drawn.
struct tTVPShapedTextAreaResult {
	int height = 0;
	int lines = 0;
	int count = 0;
};

// Draw `text` into the rect (x, y, width, height) with simple line wrapping:
// '\n' ('\r\n'/'\r') breaks explicitly; space-separated scripts wrap word-wise;
// CJK wraps per character with simple kinsoku (line-start/line-end prohibition)
// handling. `align`: 0=left, 1=center, 2=right. `lineSpacing` adds px between
// lines (may be negative). `count` >= 0 limits the total drawn clusters across
// lines; wrapping is computed for the FULL text first, so a typewriter reveal
// does not reflow. Lines that would cross the rect bottom are not drawn, and
// drawing is clipped to the rect. style.angle is ignored.
// Returns false on failure (bad style / no usable font).
bool TVPGlyphwareDrawTextArea(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
                              tjs_int width, tjs_int height,
                              const ttstr& text, tjs_uint32 color,
                              const tTVPShapedTextStyle& style, tjs_int base,
                              tjs_int count, tjs_int lineSpacing, tjs_int align,
                              tTVPShapedTextAreaResult& out);

#endif // KRKRZ_USE_GLYPHWARE
