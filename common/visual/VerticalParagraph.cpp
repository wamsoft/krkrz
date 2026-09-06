//---------------------------------------------------------------------------
// VerticalParagraph.cpp — 縦組みの段落レイアウト
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "VerticalParagraph.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include <algorithm>

namespace TVPVertical {

namespace {

// 1 段落 (改行を含まない) を組んで result へ足す
void layoutOneParagraph(std::string_view paragraph,
                        const std::vector<std::shared_ptr<glyphware::Face>>& chain,
                        int pixelSize, float lineLength,
                        const VerticalParagraphOptions& opts,
                        VerticalParagraphResult& result)
{
	const float em = static_cast<float>(pixelSize);
	const float halfEm = em * 0.5f;

	if (paragraph.empty()) {
		// 空行もそのまま 1 列分を占める
		VerticalLine line;
		line.extentLeft = -halfEm;
		line.extentRight = halfEm;
		result.lines.push_back(std::move(line));
		return;
	}

	const glyphware::VerticalLineLayout shaped =
		glyphware::layoutVerticalLine(paragraph, chain, pixelSize, opts.orientation);
	if (shaped.clusters.empty()) return;

	const std::vector<LineItem> items = TVPBuildLineItems(shaped, em, opts.spacing);
	const std::vector<BreakLine> breaks = TVPBreakLines(items, lineLength, opts.lineBreak);

	for (const BreakLine& br : breaks) {
		VerticalLine line;
		line.length = br.width;
		line.extentLeft = -halfEm;
		line.extentRight = halfEm;
		if (br.itemEnd < items.size() && items[br.itemEnd].isPenalty() &&
			items[br.itemEnd].width < 0.f) {
			line.hanging = true;
			line.hangWidth = -items[br.itemEnd].width;
		}

		float v = 0.f;
		for (std::uint32_t i = br.itemStart; i < br.itemEnd; ++i) {
			const LineItem& item = items[i];

			if (item.isGlue()) {
				v += item.natural +
				     (br.ratio >= 0.f ? br.ratio * item.stretch : br.ratio * item.shrink);
				continue;
			}
			if (!item.isBox()) continue;   // Penalty はブレークしたときだけ効く

			const glyphware::VerticalCluster& cluster = shaped.clusters[item.clusterIndex];

			// クラスタはベタ組み位置で組んであるので、その差分だけずらす
			const float delta = (v + item.glyphOffset) - cluster.origin;
			for (std::uint32_t g = 0; g < cluster.glyphCount; ++g) {
				const glyphware::VerticalGlyph& src = shaped.glyphs[cluster.glyphStart + g];
				PlacedGlyph pg;
				pg.face = src.face;
				pg.gid = src.gid;
				pg.u = src.u;
				pg.v = src.v + delta;
				pg.rotation = src.rotation;
				pg.cluster = result.totalClusters;
				line.glyphs.push_back(pg);
			}
			if (!cluster.upright) {
				// 横倒しラン (欧文) は 1em より広く/狭くなりうる
				line.extentLeft = std::min(line.extentLeft, shaped.extentLeft);
				line.extentRight = std::max(line.extentRight, shaped.extentRight);
			}

			// 描画単位 (Box) の通し番号。欧文間隔は Glue なので数えない —
			// count リビールの単位をここに揃える
			++result.totalClusters;
			++line.clusters;
			v += item.width;
		}

		result.maxLineLength = std::max(result.maxLineLength, line.length);
		result.lines.push_back(std::move(line));
	}
}

} // namespace

//---------------------------------------------------------------------------
VerticalParagraphResult TVPLayoutVerticalParagraph(
	std::string_view utf8,
	const std::vector<std::shared_ptr<glyphware::Face>>& chain,
	int pixelSize, float lineLength,
	const VerticalParagraphOptions& opts)
{
	VerticalParagraphResult result;
	if (pixelSize <= 0 || lineLength <= 0.f || chain.empty()) return result;
	result.lineAdvance = static_cast<float>(pixelSize) + opts.lineGap;
	if (utf8.empty()) return result;

	// 改行で段落へ分ける (シェイパーは改行文字を扱わない)
	std::size_t pos = 0;
	while (pos <= utf8.size()) {
		std::size_t end = utf8.size();
		std::size_t next = utf8.size() + 1;   // ループ終了を表す
		for (std::size_t i = pos; i < utf8.size(); ++i) {
			if (utf8[i] == '\n') { end = i; next = i + 1; break; }
			if (utf8[i] == '\r') {
				end = i;
				next = (i + 1 < utf8.size() && utf8[i + 1] == '\n') ? i + 2 : i + 1;
				break;
			}
		}
		layoutOneParagraph(utf8.substr(pos, end - pos), chain, pixelSize, lineLength,
		                   opts, result);
		if (next > utf8.size()) break;
		pos = next;
	}

	return result;
}

} // namespace TVPVertical

#endif // KRKRZ_USE_GLYPHWARE
