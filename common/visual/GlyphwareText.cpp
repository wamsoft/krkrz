#include "tjsCommHead.h"
#include "GlyphwareText.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include "GlyphwareHost.h"      // resolve / effective key / build chain
#include "FontVariations.h"     // 可変軸の実効座標
#include "LayerBitmapIntf.h"    // tTVPBaseBitmap
#include "CharacterSet.h"       // TVPUtf16ToUtf8
#include "tvpfontstruc.h"       // tTVPFont / TVP_TF_*
#include "glyphware/glyphware.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

//---------------------------------------------------------------------------
void TVPShapedTextStyleFromFont(tTVPShapedTextStyle& out, const tTVPFont& font)
{
	out.fontKey = font.Face;
	out.size = font.Height < 0 ? -font.Height : font.Height;
	out.bold = (font.Flags & TVP_TF_BOLD) != 0;
	out.italic = (font.Flags & TVP_TF_ITALIC) != 0;
	out.underline = (font.Flags & TVP_TF_UNDERLINE) != 0;
	out.strikeout = (font.Flags & TVP_TF_STRIKEOUT) != 0;
	out.angle = font.Angle;
	out.weight = font.Weight;
	out.variations = font.Variations;
}
//---------------------------------------------------------------------------

namespace {

//---------------------------------------------------------------------------
// shaping context: resolved fallback chain + direction + pixel size
//---------------------------------------------------------------------------
struct ShapeContext {
	std::vector<std::shared_ptr<glyphware::Face>> chain;
	glyphware::BaseDirection dir = glyphware::BaseDirection::Auto;
	int size = 0;
	// 実効の合成 bold/italic (defaultUseVarStyle で可変軸へマッピングされた
	// ぶんは落ちる)。描画は style.bold/italic ではなくこちらを使う。
	bool bold = false;
	bool italic = false;
};

bool PrepareContext(ShapeContext& ctx, const tTVPShapedTextStyle& style,
                    tjs_int base)
{
	if (style.size <= 0) return false;
	std::string keyU8;
	TVPUtf16ToUtf8(keyU8, tjs_string(style.fontKey.c_str()));
	keyU8 = TVPGlyphwareEffectiveKey(keyU8);
	if (keyU8.empty()) return false;

	// fontKey may list several font paths/names, comma-separated: the fallback
	// chain (first covering face per codepoint wins). e.g. "meiryo.ttc,seguiemj.ttf".
	TVPGlyphwareBuildChain(keyU8, ctx.chain);
	if (ctx.chain.empty()) return false;

	// 可変軸 (style.weight / style.variations) を連鎖の各 face が持つ同名軸へ
	// 適用する (private face の LRU 経由)。defaultUseVarStyle 有効時は
	// bold/italic を軸へ自動マッピングし、合成スタイルを落とす。
	ctx.bold = style.bold;
	ctx.italic = style.italic;
	{
		std::vector<tTVPFontAxisCoord> coords;
		TVPFontGetEffectiveVarCoords(style.weight, style.variations, coords);
		TVPGlyphwareAutoStyleCoords(ctx.chain[0], coords, ctx.bold, ctx.italic);
		TVPGlyphwareApplyVariationsToChain(ctx.chain, coords);
	}

	ctx.dir = base == 1 ? glyphware::BaseDirection::LTR
	        : base == 2 ? glyphware::BaseDirection::RTL
	                    : glyphware::BaseDirection::Auto;
	ctx.size = style.size;
	return true;
}

std::string ToUtf8(const tjs_char* s, size_t len)
{
	std::string u8;
	TVPUtf16ToUtf8(u8, tjs_string(s, len));
	return u8;
}

glyphware::LineLayout Layout(ShapeContext& ctx, const std::string& u8)
{
	return glyphware::layoutLine(u8, ctx.dir, ctx.chain, ctx.size);
}

//---------------------------------------------------------------------------
// glyph blitting
//---------------------------------------------------------------------------
struct ClipRect { int l = 0, t = 0, r = 0, b = 0; };   // r/b exclusive

ClipRect FullClip(const tTVPBaseBitmap* bmp)
{
	return ClipRect{ 0, 0, static_cast<int>(bmp->GetWidth()),
	                       static_cast<int>(bmp->GetHeight()) };
}

struct BlitResult {
	bool any = false;
	float penMin = 0.f, penMax = 0.f;   // pen-x range of drawn glyphs (line local)
	float advSum = 0.f;                 // advance total of drawn glyphs
};

// Blit one laid-out line with its baseline origin at (x, y). Rotation (cph/sph,
// y-up CCW) applies to glyph placement; the outline rotation itself must have
// been set on the faces by the caller (Face::setTransform). A `count` reveal is
// applied upstream by glyphware::limitClusters / layoutBlock, so every glyph
// still in `ll` is drawn.
void BlitLine(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
              const glyphware::LineLayout& ll,
              bool bold, bool italic,
              unsigned cr, unsigned cg, unsigned cb, unsigned ca,
              bool rotated, double cph, double sph,
              const ClipRect& clip, BlitResult& res)
{
	for (const glyphware::PositionedGlyph& g : ll.glyphs) {
		glyphware::GlyphBitmap gb;
		// Ask for color; text glyphs come back Gray, emoji (COLR/CBDT) come back
		// BGRA (premultiplied) and keep their own colors.
		if (!g.face->glyphBitmap(g.gid, true, gb, bold, italic) || !gb.buffer) {
			continue;
		}
		{
			const float pen = g.x + g.xOffset;
			if (!res.any) { res.penMin = pen; res.penMax = pen + g.advance; }
			else {
				if (pen < res.penMin) res.penMin = pen;
				if (pen + g.advance > res.penMax) res.penMax = pen + g.advance;
			}
			res.any = true;
			res.advSum += g.advance;
		}
		int ox, oy;
		if (!rotated) {
			ox = x + static_cast<int>(std::lround(g.x + g.xOffset)) + gb.left;
			oy = y - gb.top - static_cast<int>(std::lround(g.yOffset));
		} else {
			// pen origin along the baseline, rotated about (x,y). gb.left/top are
			// the already-rotated bitmap's bearings from that origin.
			const double penX = g.x + g.xOffset;
			const double penY = g.yOffset;   // usually 0
			ox = static_cast<int>(std::lround(x + penX * cph + penY * sph)) + gb.left;
			oy = static_cast<int>(std::lround(y - penX * sph + penY * cph)) - gb.top;
		}

		if (gb.format == glyphware::BitmapFormat::BGRA) {
			for (int r = 0; r < gb.rows; ++r) {
				const int py = oy + r;
				if (py < clip.t || py >= clip.b) continue;
				tjs_uint8* line = static_cast<tjs_uint8*>(dest->GetScanLineForWrite(py));
				const tjs_uint8* srow = gb.buffer + static_cast<ptrdiff_t>(r) * gb.pitch;
				for (int c = 0; c < gb.width; ++c) {
					const int px = ox + c;
					if (px < clip.l || px >= clip.r) continue;
					const tjs_uint8* s = srow + static_cast<ptrdiff_t>(c) * 4;  // B,G,R,A premult
					const unsigned sa = s[3];
					if (!sa) continue;
					tjs_uint8* d = line + static_cast<ptrdiff_t>(px) * 4;
					d[0] = static_cast<tjs_uint8>(s[0] + d[0] * (255 - sa) / 255);
					d[1] = static_cast<tjs_uint8>(s[1] + d[1] * (255 - sa) / 255);
					d[2] = static_cast<tjs_uint8>(s[2] + d[2] * (255 - sa) / 255);
					const unsigned na = sa + d[3] * (255 - sa) / 255;
					d[3] = static_cast<tjs_uint8>(na > 255 ? 255 : na);
				}
			}
		} else if (gb.format == glyphware::BitmapFormat::Gray) {
			for (int r = 0; r < gb.rows; ++r) {
				const int py = oy + r;
				if (py < clip.t || py >= clip.b) continue;
				tjs_uint8* line = static_cast<tjs_uint8*>(dest->GetScanLineForWrite(py));
				const tjs_uint8* srow = gb.buffer + static_cast<ptrdiff_t>(r) * gb.pitch;
				for (int c = 0; c < gb.width; ++c) {
					const int px = ox + c;
					if (px < clip.l || px >= clip.r) continue;
					const unsigned cov = srow[c];
					if (!cov) continue;
					const unsigned a = cov * ca / 255;
					tjs_uint8* d = line + static_cast<ptrdiff_t>(px) * 4;  // B,G,R,A
					d[0] = static_cast<tjs_uint8>((cb * a + d[0] * (255 - a)) / 255);
					d[1] = static_cast<tjs_uint8>((cg * a + d[1] * (255 - a)) / 255);
					d[2] = static_cast<tjs_uint8>((cr * a + d[2] * (255 - a)) / 255);
					const unsigned na = a + d[3] * (255 - a) / 255;
					d[3] = static_cast<tjs_uint8>(na > 255 ? 255 : na);
				}
			}
		}
	}
}

// Solid horizontal band (underline / strikeout), clipped.
void BlitBand(tTVPBaseBitmap* dest, const ClipRect& clip,
              int xStart, int xEnd, int yTop, int thick,
              unsigned cr, unsigned cg, unsigned cb, unsigned ca)
{
	if (xStart < clip.l) xStart = clip.l;
	if (xEnd > clip.r) xEnd = clip.r;
	if (xStart >= xEnd) return;
	for (int r = 0; r < thick; ++r) {
		const int py = yTop + r;
		if (py < clip.t || py >= clip.b) continue;
		tjs_uint8* line = static_cast<tjs_uint8*>(dest->GetScanLineForWrite(py));
		for (int px = xStart; px < xEnd; ++px) {
			tjs_uint8* d = line + static_cast<ptrdiff_t>(px) * 4;
			d[0] = static_cast<tjs_uint8>((cb * ca + d[0] * (255 - ca)) / 255);
			d[1] = static_cast<tjs_uint8>((cg * ca + d[1] * (255 - ca)) / 255);
			d[2] = static_cast<tjs_uint8>((cr * ca + d[2] * (255 - ca)) / 255);
			const unsigned na = ca + d[3] * (255 - ca) / 255;
			d[3] = static_cast<tjs_uint8>(na > 255 ? 255 : na);
		}
	}
}

// underline / strikeout decoration lines over the drawn pen range of a line.
void BlitDecorations(tTVPBaseBitmap* dest, const ClipRect& clip,
                     ShapeContext& ctx, tjs_int x, tjs_int y,
                     const tTVPShapedTextStyle& style, const BlitResult& res,
                     unsigned cr, unsigned cg, unsigned cb, unsigned ca)
{
	if (!(style.underline || style.strikeout) || !res.any) return;
	glyphware::LineMetrics lm = ctx.chain[0]->lineMetrics();
	const int xs = x + static_cast<int>(std::floor(res.penMin));
	const int xe = x + static_cast<int>(std::ceil(res.penMax));
	if (style.underline) {
		BlitBand(dest, clip, xs, xe,
		         y + static_cast<int>(std::lround(lm.underlineOffset)),
		         std::max(1, static_cast<int>(std::lround(lm.underlineThickness))),
		         cr, cg, cb, ca);
	}
	if (style.strikeout) {
		BlitBand(dest, clip, xs, xe,
		         y + static_cast<int>(std::lround(lm.strikeoutOffset)),
		         std::max(1, static_cast<int>(std::lround(lm.strikeoutThickness))),
		         cr, cg, cb, ca);
	}
}

void SplitColor(tjs_uint32 color, unsigned& cr, unsigned& cg, unsigned& cb,
                unsigned& ca)
{
	cr = (color >> 16) & 0xff;
	cg = (color >> 8) & 0xff;
	cb = color & 0xff;
	ca = (color >> 24) & 0xff;
	if (ca == 0) ca = 255;   // classic drawText passes 24bit RGB (alpha 0 = opaque)
}

} // anonymous namespace

//---------------------------------------------------------------------------
int TVPGlyphwareDrawText(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
                         const ttstr& text, tjs_uint32 color,
                         const tTVPShapedTextStyle& style, tjs_int base,
                         tjs_int count)
{
	if (!dest) return -1;
	ShapeContext ctx;
	if (!PrepareContext(ctx, style, base)) return -1;
	const std::string utf8 = ToUtf8(text.c_str(), text.GetLen());
	if (utf8.empty()) return -1;

	glyphware::LineLayout ll = Layout(ctx, utf8);
	const float fullWidth = ll.width;

	// `count` reveal: drop the glyphs past the limit (the full line was shaped
	// first, so contextual forms stay stable as the reveal advances).
	const int totalClusters = glyphware::lineClusterCount(ll);
	const bool limited = glyphware::limitClusters(ll, count) < totalClusters;

	unsigned cr, cg, cb, ca;
	SplitColor(color, cr, cg, cb, ca);

	// Rotation: angle is 1/10 degree, counterclockwise (Font.angle). We rotate the
	// glyph outlines via FreeType's transform (crisp), and rotate each glyph's pen
	// origin about (x,y). FreeType coords are y-up; device y is down, so the CCW
	// matrix is applied with a negated y on placement.
	const bool rotated = style.angle != 0;
	const double phi = style.angle * (3.14159265358979323846 / 1800.0);
	const double cph = std::cos(phi);
	const double sph = std::sin(phi);
	if (rotated) {
		for (auto& f : ctx.chain)
			if (f) f->setTransform(cph, -sph, sph, cph);   // y-up CCW by phi
	}

	BlitResult res;
	const ClipRect clip = FullClip(dest);
	BlitLine(dest, x, y, ll, ctx.bold, ctx.italic, cr, cg, cb, ca,
	         rotated, cph, sph, clip, res);

	// decoration lines over the drawn portion
	// (rotated decoration is not drawn — rare combined with a non-zero angle.)
	if (!rotated)
		BlitDecorations(dest, clip, ctx, x, y, style, res, cr, cg, cb, ca);

	if (rotated) {
		for (auto& f : ctx.chain)
			if (f) f->clearTransform();   // don't leak the transform to later draws
	}
	return static_cast<int>(std::lround(limited ? res.advSum : fullWidth));
}
//---------------------------------------------------------------------------
bool TVPGlyphwareMeasureText(tTVPGlyphwareMetrics& out, const ttstr& text,
                             const tTVPShapedTextStyle& style, tjs_int base)
{
	ShapeContext ctx;
	if (!PrepareContext(ctx, style, base)) return false;

	// establish pixel size for line metrics
	for (auto& f : ctx.chain) if (f) f->setPixelSize(ctx.size);
	glyphware::LineMetrics lm = ctx.chain[0]->lineMetrics();
	out = tTVPGlyphwareMetrics{};
	out.ascent = static_cast<int>(std::lround(lm.ascent));
	out.descent = static_cast<int>(std::lround(lm.descent));

	const std::string utf8 = ToUtf8(text.c_str(), text.GetLen());
	if (utf8.empty()) return true;

	glyphware::LineLayout ll = Layout(ctx, utf8);
	out.advance = static_cast<int>(std::lround(ll.width));
	{
		const int c = TVPGlyphwareTextCount(text, style, base);
		out.count = c < 0 ? 0 : c;
	}

	bool any = false;
	float minX = 0, minY = 0, maxX = 0, maxY = 0;
	for (const glyphware::PositionedGlyph& g : ll.glyphs) {
		glyphware::GlyphMetrics gm;
		if (!g.face->glyphMetrics(g.gid, gm)) continue;
		const float l = g.x + g.xOffset + gm.bearingX;
		const float r = l + gm.width;
		const float t = -(g.yOffset) - gm.bearingY;  // baseline=0, y-down
		const float b = t + gm.height;
		if (!any) { minX = l; maxX = r; minY = t; maxY = b; any = true; }
		else {
			if (l < minX) minX = l;
			if (r > maxX) maxX = r;
			if (t < minY) minY = t;
			if (b > maxY) maxY = b;
		}
	}
	if (any) {
		out.left = static_cast<int>(std::floor(minX));
		out.top = static_cast<int>(std::floor(minY));
		out.right = static_cast<int>(std::ceil(maxX));
		out.bottom = static_cast<int>(std::ceil(maxY));
	}
	return true;
}
//---------------------------------------------------------------------------
int TVPGlyphwareTextCount(const ttstr& text, const tTVPShapedTextStyle& style,
                          tjs_int base)
{
	ShapeContext ctx;
	if (!PrepareContext(ctx, style, base)) return -1;
	return glyphware::countClusters(ToUtf8(text.c_str(), text.GetLen()),
	                                ctx.dir, ctx.chain, ctx.size);
}
//---------------------------------------------------------------------------
bool TVPGlyphwareDrawTextArea(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
                              tjs_int width, tjs_int height,
                              const ttstr& text, tjs_uint32 color,
                              const tTVPShapedTextStyle& style, tjs_int base,
                              tjs_int count, tjs_int lineSpacing, tjs_int align,
                              tTVPShapedTextAreaResult& out)
{
	out = tTVPShapedTextAreaResult{};
	if (!dest) return false;
	ShapeContext ctx;
	if (!PrepareContext(ctx, style, base)) return false;
	if (width <= 0 || height <= 0) return true;   // nothing to draw, not an error

	// wrapping / 禁則 / alignment / the count reveal all live in glyphware, so
	// the Elements text widgets break lines exactly the same way
	glyphware::BlockOptions bo;
	bo.width = static_cast<float>(width);
	bo.height = static_cast<float>(height);
	bo.lineSpacing = static_cast<float>(lineSpacing);
	bo.align = align == 1 ? glyphware::Align::Center
	         : align == 2 ? glyphware::Align::Right
	                      : glyphware::Align::Left;
	bo.count = count;
	glyphware::BlockLayout block = glyphware::layoutBlock(
		ToUtf8(text.c_str(), text.GetLen()), ctx.dir, ctx.chain, ctx.size, bo);

	unsigned cr, cg, cb, ca;
	SplitColor(color, cr, cg, cb, ca);

	ClipRect clip = FullClip(dest);
	if (x > clip.l) clip.l = x;
	if (y > clip.t) clip.t = y;
	if (x + width < clip.r) clip.r = x + width;
	if (y + height < clip.b) clip.b = y + height;

	for (const glyphware::BlockLine& bl : block.lines) {
		if (bl.layout.glyphs.empty()) continue;
		const tjs_int lx = x + static_cast<tjs_int>(bl.x);
		const tjs_int baseline = y + static_cast<tjs_int>(bl.y);
		BlitResult res;
		BlitLine(dest, lx, baseline, bl.layout, ctx.bold, ctx.italic,
		         cr, cg, cb, ca, false, 1.0, 0.0, clip, res);
		BlitDecorations(dest, clip, ctx, lx, baseline, style, res,
		                cr, cg, cb, ca);
	}

	out.count = block.drawnClusters;
	out.lines = block.lineCount;
	out.height = static_cast<int>(block.height);
	return true;
}
//---------------------------------------------------------------------------

#endif // KRKRZ_USE_GLYPHWARE
