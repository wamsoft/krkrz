#include "tjsCommHead.h"
#include "VerticalText.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include "VerticalParagraph.h"
#include "LayerBitmapIntf.h"    // tTVPBaseBitmap
#include "CharacterSet.h"       // TVPUtf16ToUtf8
#include "glyphware/glyphware.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

struct ClipRect { int l = 0, t = 0, r = 0, b = 0; };   // r/b exclusive

// 8bit カバレッジを単色で合成する (BGRA 32bpp 前提)
void BlitGray(tTVPBaseBitmap* dest, const ClipRect& clip,
              const tjs_uint8* buffer, int width, int rows, int pitch,
              int ox, int oy, unsigned cr, unsigned cg, unsigned cb, unsigned ca)
{
	for (int r = 0; r < rows; ++r) {
		const int py = oy + r;
		if (py < clip.t || py >= clip.b) continue;
		tjs_uint8* line = static_cast<tjs_uint8*>(dest->GetScanLineForWrite(py));
		const tjs_uint8* srow = buffer + static_cast<ptrdiff_t>(r) * pitch;
		for (int c = 0; c < width; ++c) {
			const int px = ox + c;
			if (px < clip.l || px >= clip.r) continue;
			const unsigned cov = srow[c];
			if (!cov) continue;
			const unsigned a = cov * ca / 255;
			tjs_uint8* d = line + static_cast<ptrdiff_t>(px) * 4;   // B,G,R,A
			d[0] = static_cast<tjs_uint8>((cb * a + d[0] * (255 - a)) / 255);
			d[1] = static_cast<tjs_uint8>((cg * a + d[1] * (255 - a)) / 255);
			d[2] = static_cast<tjs_uint8>((cr * a + d[2] * (255 - a)) / 255);
			const unsigned na = a + d[3] * (255 - a) / 255;
			d[3] = static_cast<tjs_uint8>(na > 255 ? 255 : na);
		}
	}
}

// カラー絵文字 (premultiplied BGRA) をそのまま合成する
void BlitBGRA(tTVPBaseBitmap* dest, const ClipRect& clip,
              const tjs_uint8* buffer, int width, int rows, int pitch,
              int ox, int oy)
{
	for (int r = 0; r < rows; ++r) {
		const int py = oy + r;
		if (py < clip.t || py >= clip.b) continue;
		tjs_uint8* line = static_cast<tjs_uint8*>(dest->GetScanLineForWrite(py));
		const tjs_uint8* srow = buffer + static_cast<ptrdiff_t>(r) * pitch;
		for (int c = 0; c < width; ++c) {
			const int px = ox + c;
			if (px < clip.l || px >= clip.r) continue;
			const tjs_uint8* s = srow + static_cast<ptrdiff_t>(c) * 4;
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
}

// 1 グリフ描画。ペン原点は (penX, penY) (デバイス座標、y-down)。
//
// 正立グリフは変形が無いので既存の drawShapedText と同じ glyphBitmap 経路を
// 通す (カラー絵文字もそのまま出る)。横倒しグリフだけ回転をアウトラインへ
// 焼き込んでラスタライズする — 1 行の中で正立と横倒しが混ざるので、
// Face::setTransform で face 状態を張り替える方式は使えない。
void DrawGlyph(tTVPBaseBitmap* dest, const ClipRect& clip,
               const TVPVertical::PlacedGlyph& g, float penX, float penY,
               int pixelSize, bool bold, bool italic,
               unsigned cr, unsigned cg, unsigned cb, unsigned ca)
{
	if (!g.face) return;

	if (g.rotation == 0.f) {
		glyphware::GlyphBitmap gb;
		if (!g.face->glyphBitmap(g.gid, true, gb, bold, italic) || !gb.buffer) return;
		const int ox = static_cast<int>(std::lround(penX)) + gb.left;
		const int oy = static_cast<int>(std::lround(penY)) - gb.top;
		if (gb.format == glyphware::BitmapFormat::BGRA)
			BlitBGRA(dest, clip, gb.buffer, gb.width, gb.rows, gb.pitch, ox, oy);
		else
			BlitGray(dest, clip, gb.buffer, gb.width, gb.rows, gb.pitch, ox, oy,
			         cr, cg, cb, ca);
		return;
	}

	float upem = g.face->lineMetrics().unitsPerEm;
	if (upem <= 0.f) upem = 1000.f;
	const float scale = static_cast<float>(pixelSize) / upem;
	const float cs = std::cos(g.rotation);
	const float sn = std::sin(g.rotation);

	// フォントユニット (y-up) → デバイス (y-down) のスケール、回転、ペンへの
	// 平行移動をこの順で 1 つのアフィンへ畳む。回転角は数学慣習 (y-up) の
	// 反時計回りが正なので、y-down 側では共役を取った形になる。
	// 最後に backend が y-up で受けるぶん y 行を反転して渡す。
	glyphware::RenderParams params;
	params.transform.xx = cs * scale;
	params.transform.xy = -sn * scale;
	params.transform.dx = penX;
	params.transform.yx = sn * scale;
	params.transform.yy = cs * scale;
	params.transform.dy = -penY;
	params.bold = bold;
	params.italic = italic;

	glyphware::GlyphMask mask;
	if (!g.face->renderGlyphMask(g.gid, params, mask) || !mask.buffer) return;
	// mask.left / mask.top はペン位置を畳み込んだあとの絶対値 (y-up)
	BlitGray(dest, clip, mask.buffer, mask.width, mask.rows, mask.pitch,
	         mask.left, -mask.top, cr, cg, cb, ca);
}

void SplitColor(tjs_uint32 color, unsigned& cr, unsigned& cg, unsigned& cb, unsigned& ca)
{
	cr = (color >> 16) & 0xff;
	cg = (color >> 8) & 0xff;
	cb = color & 0xff;
	ca = (color >> 24) & 0xff;
	if (ca == 0) ca = 255;   // classic drawText passes 24bit RGB (alpha 0 = opaque)
}

TVPVertical::VerticalParagraphOptions ToParagraphOptions(const tTVPVerticalTextOptions& o)
{
	TVPVertical::VerticalParagraphOptions p;
	p.orientation = o.orientation == 1 ? glyphware::TextOrientation::Upright
	              : o.orientation == 2 ? glyphware::TextOrientation::Sideways
	                                   : glyphware::TextOrientation::Mixed;
	p.verticalLr = o.verticalLr;
	p.lineGap = static_cast<float>(o.lineSpacing);
	p.spacing.punctuationSpacing = o.punctuation;
	p.spacing.latinGap = o.latinGap;
	p.spacing.hangingPunctuation = o.hanging;
	p.spacing.letterSpacing = o.letterSpacing;
	p.lineBreak.justify = o.justify;
	return p;
}

// 組版と、矩形に収まる列数の決定。dest が null なら計測のみ。
bool VerticalTextArea(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
                      tjs_int width, tjs_int height,
                      const ttstr& text, tjs_uint32 color,
                      const tTVPShapedTextStyle& style, tjs_int count,
                      const tTVPVerticalTextOptions& opts,
                      tTVPVerticalTextResult& out)
{
	out = tTVPVerticalTextResult{};

	tTVPShapedFontChain font;
	if (!TVPGlyphwareResolveChain(font, style)) return false;
	if (width <= 0 || height <= 0) return true;   // nothing to draw, not an error

	std::string utf8;
	TVPUtf16ToUtf8(utf8, tjs_string(text.c_str(), text.GetLen()));
	if (utf8.empty()) return true;

	const int size = style.size;
	const TVPVertical::VerticalParagraphResult layout =
		TVPVertical::TVPLayoutVerticalParagraph(utf8, font.chain, size,
		                                        static_cast<float>(height),
		                                        ToParagraphOptions(opts));
	out.totalCount = layout.totalClusters;
	if (layout.lines.empty()) return true;

	// 矩形に収まる列数。n 列は (n-1)*lineAdvance + size を占める
	const float lineAdvance = layout.lineAdvance;
	std::size_t drawnLines = 0;
	for (std::size_t i = 0; i < layout.lines.size(); ++i) {
		if (lineAdvance * static_cast<float>(i) + static_cast<float>(size) >
		    static_cast<float>(width) + 0.5f) {
			break;
		}
		++drawnLines;
	}
	out.lines = static_cast<int>(drawnLines);
	if (drawnLines > 0) {
		out.width = static_cast<int>(std::lround(
			lineAdvance * static_cast<float>(drawnLines - 1) + static_cast<float>(size)));
	}

	int drawableClusters = 0;
	for (std::size_t i = 0; i < drawnLines; ++i) drawableClusters += layout.lines[i].clusters;
	out.count = (count >= 0) ? std::min<int>(count, drawableClusters) : drawableClusters;

	if (!dest || drawnLines == 0) return true;

	unsigned cr, cg, cb, ca;
	SplitColor(color, cr, cg, cb, ca);

	ClipRect clip{ 0, 0, static_cast<int>(dest->GetWidth()),
	                     static_cast<int>(dest->GetHeight()) };
	if (x > clip.l) clip.l = x;
	if (y > clip.t) clip.t = y;
	if (x + width < clip.r) clip.r = x + width;
	if (y + height < clip.b) clip.b = y + height;
	if (clip.l >= clip.r || clip.t >= clip.b) return true;

	// 1 列目の縦ベースライン。vertical-rl は矩形の右端から左へ進む
	const float halfEm = static_cast<float>(size) * 0.5f;
	const float originX = opts.verticalLr
		? static_cast<float>(x) + halfEm
		: static_cast<float>(x + width) - halfEm;
	const float originY = static_cast<float>(y);

	for (std::size_t i = 0; i < drawnLines; ++i) {
		const TVPVertical::VerticalLine& line = layout.lines[i];
		const float cx = TVPVertical::TVPVerticalLinePosition(layout, i, originX,
		                                                      opts.verticalLr);
		for (const TVPVertical::PlacedGlyph& g : line.glyphs) {
			if (count >= 0 && g.cluster >= count) break;   // クラスタは行内で昇順
			DrawGlyph(dest, clip, g, cx + g.u, originY + g.v, size,
			          font.bold, font.italic, cr, cg, cb, ca);
		}
	}

	return true;
}

} // anonymous namespace

//---------------------------------------------------------------------------
bool TVPGlyphwareDrawVerticalTextArea(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
                                      tjs_int width, tjs_int height,
                                      const ttstr& text, tjs_uint32 color,
                                      const tTVPShapedTextStyle& style,
                                      tjs_int count,
                                      const tTVPVerticalTextOptions& opts,
                                      tTVPVerticalTextResult& out)
{
	if (!dest) return false;
	return VerticalTextArea(dest, x, y, width, height, text, color, style, count,
	                        opts, out);
}
//---------------------------------------------------------------------------
bool TVPGlyphwareMeasureVerticalTextArea(tjs_int width, tjs_int height,
                                         const ttstr& text,
                                         const tTVPShapedTextStyle& style,
                                         tjs_int count,
                                         const tTVPVerticalTextOptions& opts,
                                         tTVPVerticalTextResult& out)
{
	return VerticalTextArea(nullptr, 0, 0, width, height, text, 0, style, count,
	                        opts, out);
}
//---------------------------------------------------------------------------

#endif // KRKRZ_USE_GLYPHWARE
