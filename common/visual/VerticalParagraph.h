//---------------------------------------------------------------------------
// VerticalParagraph — 縦組みの段落レイアウト
//
// 処理の流れ:
//   1. 改行で段落へ分ける
//   2. glyphware::layoutVerticalLine() でクラスタ列を得る (ベタ組み)
//   3. TVPBuildLineItems() で Box / Glue / Penalty 列へ変換 (JLReq のアキと禁則)
//   4. TVPBreakLines() で行を決め、行ごとのグルー調整比を得る
//   5. 調整比に従ってクラスタを配置し直し、行ごとのグリフ列を作る
//
// 座標系は列ローカル。PlacedGlyph::u が縦ベースライン (列の中心線) からの
// 左右、PlacedGlyph::v が行頭からの送り。列そのものの位置は
// TVPVerticalLinePosition() で得る。
//---------------------------------------------------------------------------
#ifndef __VERTICAL_PARAGRAPH_H__
#define __VERTICAL_PARAGRAPH_H__

#ifdef KRKRZ_USE_GLYPHWARE

#include "VerticalLineBreak.h"
#include "glyphware/Vertical.h"

#include <memory>
#include <string_view>
#include <vector>

namespace TVPVertical {

// 配置済みグリフ 1 個
struct PlacedGlyph {
	glyphware::Face* face = nullptr;   // borrowed (chain が所有)
	glyphware::GlyphId gid = 0;
	float u = 0.f;          // 縦ベースラインからの左右 (右が正)
	float v = 0.f;          // 行頭からの送り (下が正)
	float rotation = 0.f;   // ラジアン、y-up CCW 正。横倒しは -90 度
	int cluster = 0;        // 全体を通したクラスタ通し番号 (count リビール用)
};

// 確定した 1 行 (縦組みでは 1 列)
struct VerticalLine {
	std::vector<PlacedGlyph> glyphs;
	float length = 0.f;        // 調整後の行長
	float extentLeft = 0.f;    // 縦ベースラインからの左への張り出し (負値)
	float extentRight = 0.f;   // 同・右への張り出し (正値)
	int clusters = 0;          // この行の描画単位 (Box) 数
	bool hanging = false;      // 行末の約物を版面外へ出した行か
	float hangWidth = 0.f;

	float width() const { return extentRight - extentLeft; }
};

struct VerticalParagraphOptions {
	VerticalSpacingOptions spacing;
	LineBreakOptions lineBreak;
	glyphware::TextOrientation orientation = glyphware::TextOrientation::Mixed;
	// 列の進む向き。既定 (false) は vertical-rl = 右から左
	bool verticalLr = false;
	// 行間 (ピクセル)。行送り = フォントサイズ + lineGap
	float lineGap = 0.f;
};

struct VerticalParagraphResult {
	std::vector<VerticalLine> lines;
	float lineAdvance = 0.f;     // 隣り合う列の縦ベースラインの間隔
	float maxLineLength = 0.f;   // 最長の行長
	int totalClusters = 0;       // 全体の描画単位 (Box) 数。PlacedGlyph::cluster
	                             // はこの通し番号で、count リビールが数える単位

	// 全列を並べたときの幅
	float totalWidth() const { return lineAdvance * static_cast<float>(lines.size()); }
};

// 段落レイアウトを実行する。`utf8` は改行 (\n / \r\n / \r) を含んでよい。
// `lineLength` は 1 行 (1 列) の長さ。
VerticalParagraphResult TVPLayoutVerticalParagraph(
	std::string_view utf8,
	const std::vector<std::shared_ptr<glyphware::Face>>& chain,
	int pixelSize, float lineLength,
	const VerticalParagraphOptions& opts);

// 行の縦ベースラインの X。`originX` は 1 列目の縦ベースラインの X。
// vertical-rl では列が右から左へ進むので X は減っていく。
inline float TVPVerticalLinePosition(const VerticalParagraphResult& result,
                                     std::size_t index, float originX, bool verticalLr)
{
	const float offset = result.lineAdvance * static_cast<float>(index);
	return verticalLr ? originX + offset : originX - offset;
}

} // namespace TVPVertical

#endif // KRKRZ_USE_GLYPHWARE

#endif // __VERTICAL_PARAGRAPH_H__
