//---------------------------------------------------------------------------
// VerticalLineBreak.cpp — 組版アイテムの組み立てと Greedy 行分割
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "VerticalLineBreak.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include "glyphware/Vertical.h"

#include <algorithm>
#include <cmath>

namespace TVPVertical {

namespace {

//---------------------------------------------------------------------------
// 禁則とボディ
//---------------------------------------------------------------------------

// 2 つのクラスタの間で改行できるか。ここで false になる位置には Penalty(∞) を
// 置き、続く Glue がブレーク点にならないようにする。
bool canBreakBetween(CharClass before, CharClass after)
{
	// 行末禁則: 始め括弧類・前置省略記号の直後では切らない
	if (TVPIsLineEndProhibited(before)) return false;
	// 行頭禁則: 終わり括弧類・句読点・中点類・小書きの仮名等の直前では切らない
	if (TVPIsLineStartProhibited(after)) return false;

	// 分離禁止文字 (—— …… 等) は 2 つ並びを割らない
	if (before == CharClass::Inseparable && after == CharClass::Inseparable) return false;

	// 欧文単語の内部と連数字は割らない。欧文の切れ目は空白 (Glue) だけ
	if (TVPIsWestern(before) && TVPIsWestern(after)) return false;
	// 省略記号は続く/先立つ数字と離さない
	if (before == CharClass::PrefixAbbr && TVPIsWestern(after)) return false;
	if (TVPIsWestern(before) && after == CharClass::PostfixAbbr) return false;

	return true;
}

// 詰めた仮想ボディの中でのグリフ位置。シェイパーが返す送り (通常 1em) と、
// 詰めたあとのボディ幅の差を、字面の寄りに応じて前後へ振り分ける。
float bodyGlyphOffset(CharClass cls, float shapedAdvance, float bodyWidth)
{
	const float slack = shapedAdvance - bodyWidth;
	if (slack <= 0.f) return 0.f;
	switch (TVPGetBodyAlign(cls)) {
	case BodyAlign::End:    return -slack;
	case BodyAlign::Center: return -slack * 0.5f;
	case BodyAlign::Start:
	case BodyAlign::Full:
	default:                return 0.f;
	}
}

//---------------------------------------------------------------------------
// 行分割の共通部
//---------------------------------------------------------------------------

constexpr float kInfinity = std::numeric_limits<float>::infinity();

// アイテム i が合法なブレーク点か
bool isLegalBreak(const std::vector<LineItem>& items, std::size_t i)
{
	const LineItem& it = items[i];
	if (it.isPenalty()) return it.penalty < TVP_VERT_INFINITE_PENALTY;
	if (it.isGlue()) {
		// グルーの直前が Box のときだけ切れる (禁則は直前に Penalty(∞) を
		// 挟むことでこの条件を崩して表現している)
		return i > 0 && items[i - 1].isBox();
	}
	return false;
}

// ブレーク点 b の次の行の開始位置 (捨てられるアイテムを読み飛ばす)
std::uint32_t nextLineStart(const std::vector<LineItem>& items, std::size_t b)
{
	std::size_t i = items[b].isPenalty() ? b + 1 : b;
	while (i < items.size() && !items[i].isBox()) ++i;
	return static_cast<std::uint32_t>(i);
}

// 各アイテムまでの累積 (幅・伸び・縮み)
struct Prefix {
	std::vector<float> width, stretch, shrink;

	explicit Prefix(const std::vector<LineItem>& items) {
		const std::size_t n = items.size();
		width.resize(n + 1, 0.f);
		stretch.resize(n + 1, 0.f);
		shrink.resize(n + 1, 0.f);
		for (std::size_t i = 0; i < n; ++i) {
			const LineItem& it = items[i];
			float w = 0.f, st = 0.f, sh = 0.f;
			if (it.isBox()) {
				w = it.width;
			} else if (it.isGlue()) {
				w = it.natural; st = it.stretch; sh = it.shrink;
			}
			width[i + 1] = width[i] + w;
			stretch[i + 1] = stretch[i] + st;
			shrink[i + 1] = shrink[i] + sh;
		}
	}
};

// 行 [start, breakAt) の自然幅 (ブレーク点が Penalty ならその幅を足す)
float lineWidth(const std::vector<LineItem>& items, const Prefix& pre,
                std::uint32_t start, std::uint32_t breakAt)
{
	float w = pre.width[breakAt] - pre.width[start];
	if (items[breakAt].isPenalty()) w += items[breakAt].width;
	return w;
}

// グルーの調整比。目標より短ければ正 (伸ばす)、長ければ負 (縮める)。
float adjustRatio(float natural, float stretch, float shrink, float target)
{
	const float diff = target - natural;
	if (std::fabs(diff) < 1e-4f) return 0.f;
	if (diff > 0.f) return (stretch > 0.f) ? diff / stretch : kInfinity;
	return (shrink > 0.f) ? diff / shrink : -kInfinity;
}

} // namespace

//---------------------------------------------------------------------------
std::vector<LineItem> TVPBuildLineItems(const glyphware::VerticalLineLayout& shaped,
                                        float em,
                                        const VerticalSpacingOptions& opts)
{
	std::vector<LineItem> items;
	if (shaped.clusters.empty() || em <= 0.f) return items;
	items.reserve(shaped.clusters.size() * 3 + 3);

	const float letterSpacing = opts.letterSpacing * em;
	const std::uint32_t clusterCount = static_cast<std::uint32_t>(shaped.clusters.size());

	// --- 仮想ボディ幅 ---
	std::vector<float> bodyWidths(clusterCount, 0.f);
	std::vector<CharClass> classes(clusterCount, CharClass::Unknown);
	for (std::uint32_t ci = 0; ci < clusterCount; ++ci) {
		const glyphware::VerticalCluster& cl = shaped.clusters[ci];
		const CharClass cls = TVPGetCharClass(cl.lead);
		classes[ci] = cls;

		float bodyEm = opts.punctuationSpacing ? TVPGetBodyWidth(cls) : 0.f;
		if (!opts.punctuationSpacing && TVPIsJapanese(cls)) bodyEm = 1.f;
		float w = (bodyEm > 0.f) ? bodyEm * em : cl.advance;
		if (bodyEm > 0.f && cl.advance > 0.f) w = std::min(w, cl.advance);
		bodyWidths[ci] = w;
	}

	// --- 本体 ---
	bool prevWasBox = false;
	CharClass prevClass = CharClass::Unknown;
	float prevBoxWidth = 0.f;

	for (std::uint32_t ci = 0; ci < clusterCount; ++ci) {
		const glyphware::VerticalCluster& cluster = shaped.clusters[ci];
		const CharClass cls = classes[ci];

		// 欧文間隔は Box ではなく Glue にする (そこが唯一の欧文の切れ目)
		if (cls == CharClass::Space) {
			const float w = cluster.advance;
			items.push_back(LineItem::glue(w, w * 0.5f, w / 3.f, cluster.byteStart));
			prevWasBox = false;
			prevClass = cls;
			continue;
		}

		const float boxWidth = bodyWidths[ci];

		// --- クラスタ間のアキと禁則 ---
		if (prevWasBox) {
			const bool latinBoundary =
				(TVPIsJapanese(prevClass) && TVPIsWestern(cls)) ||
				(TVPIsWestern(prevClass) && TVPIsJapanese(cls));
			GlueSpec spec = TVPGetSpacing(prevClass, cls);
			if (latinBoundary ? !opts.latinGap : !opts.punctuationSpacing) spec = GlueSpec{};

			const float natural = spec.natural * em + letterSpacing;
			const float stretch = spec.stretch * em;
			const float shrink = spec.shrink * em;

			const bool breakable = canBreakBetween(prevClass, cls);

			// ぶら下げ: 句読点の直後は「幅が負の Penalty」で切る。ブレークすると
			// 句読点 1 文字分が行長から引かれる = 版面外へ出る。Penalty を挟むと
			// 直後の Glue はブレーク点でなくなるので (TeX の「Glue の直前が Box の
			// ときだけ切れる」規則)、ここでの切り方はぶら下げに一本化される。
			if (breakable && opts.hangingPunctuation && TVPIsHangable(prevClass)) {
				items.push_back(LineItem::penaltyItem(0.f, -prevBoxWidth, cluster.byteStart));
			} else if (!breakable) {
				items.push_back(LineItem::penaltyItem(TVP_VERT_INFINITE_PENALTY, 0.f,
				                                      cluster.byteStart));
			}

			if (breakable || natural != 0.f || stretch != 0.f || shrink != 0.f) {
				items.push_back(LineItem::glue(natural, stretch, shrink, cluster.byteStart));
			}
		}

		LineItem box = LineItem::box(boxWidth, ci, cluster.byteStart);
		box.glyphOffset = bodyGlyphOffset(cls, cluster.advance, boxWidth);
		items.push_back(box);

		prevWasBox = true;
		prevClass = cls;
		prevBoxWidth = boxWidth;
	}

	// --- 段落の終端 (TeX と同じ形) ---
	// 無限に伸びる Glue で最終行を伸ばさないようにし、強制ブレークで閉じる
	const std::size_t tail = shaped.clusters.back().byteEnd;
	items.push_back(LineItem::penaltyItem(TVP_VERT_INFINITE_PENALTY, 0.f, tail));
	items.push_back(LineItem::glue(0.f, 1.0e6f, 0.f, tail));
	items.push_back(LineItem::penaltyItem(TVP_VERT_FORCED_BREAK_PENALTY, 0.f, tail));

	return items;
}

//---------------------------------------------------------------------------
std::vector<BreakLine> TVPBreakLines(const std::vector<LineItem>& items,
                                     float lineLength,
                                     const LineBreakOptions& opts)
{
	std::vector<BreakLine> lines;
	if (items.empty() || lineLength <= 0.f) return lines;

	const Prefix pre(items);
	const std::uint32_t n = static_cast<std::uint32_t>(items.size());

	std::uint32_t start = 0;
	while (start < n && !items[start].isBox()) ++start;

	while (start < n) {
		std::uint32_t chosen = n;
		bool forced = false;

		for (std::uint32_t i = start; i < n; ++i) {
			if (!isLegalBreak(items, i) || i <= start) continue;
			const float w = lineWidth(items, pre, start, i);
			const float sh = pre.shrink[i] - pre.shrink[start];
			if (w - sh <= lineLength) {
				chosen = i;
				if (items[i].isForcedBreak()) { forced = true; break; }
			} else if (chosen != n) {
				break;   // これ以上は入らない
			} else {
				// 1 つも入らない場合は溢れを承知でここで切る
				chosen = i;
				if (items[i].isForcedBreak()) forced = true;
				break;
			}
		}

		if (chosen == n) {
			// ブレーク点が見つからない (末尾の段落終端が無い場合など)
			chosen = n - 1;
			forced = true;
		}

		BreakLine line;
		line.itemStart = start;
		line.itemEnd = chosen;
		line.naturalWidth = lineWidth(items, pre, start, chosen);
		const float st = pre.stretch[chosen] - pre.stretch[start];
		const float sh = pre.shrink[chosen] - pre.shrink[start];
		{
			float r = adjustRatio(line.naturalWidth, st, sh, lineLength);
			if (!std::isfinite(r)) r = 0.f;
			// 揃えない指定でも「縮み」は掛ける。ブレーク候補は縮めれば入る
			// ところまで許しているので、伸ばさないだけにすると行が溢れる。
			if (!opts.justify && r > 0.f) r = 0.f;
			line.ratio = std::clamp(r, -1.f, 1.f);
		}
		line.width = line.naturalWidth +
		             (line.ratio >= 0.f ? line.ratio * st : line.ratio * sh);
		lines.push_back(line);

		if (forced && chosen >= n - 1) break;

		const std::uint32_t next = nextLineStart(items, chosen);
		if (next <= start) break;   // 前進しない場合の保険
		start = next;
	}

	return lines;
}

} // namespace TVPVertical

#endif // KRKRZ_USE_GLYPHWARE
