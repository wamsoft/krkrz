//---------------------------------------------------------------------------
// VerticalLineBreak — Box / Glue / Penalty 列と、そこからの行分割
//
// 組版対象を TeX と同じ 3 種のアイテム列で表す。寸法はピクセル (em ではない。
// アキ量表の em 値にフォントサイズを掛けたもの)。
//
// 合法なブレーク点は TeX と同じ規則で決まる。
//
//  - Penalty アイテムで、penalty が TVP_VERT_INFINITE_PENALTY 未満のところ
//  - Glue アイテムで、直前が Box のところ
//
// 禁則は「Glue の直前に Penalty(∞) を挟む」ことで表す。これで 2 番目の条件
// (直前が Box) が崩れ、その位置では切れなくなる。ぶら下げは「幅が負の
// Penalty」で表す (ブレークするとその幅が行長から引かれる = 版面外へ出る)。
//
// 行が確定するとグルーの調整比 ratio が決まり、これを各グルーの伸び/縮みに
// 掛けることで追い込み・追い出し・両端揃えがまとめて実現される。
//---------------------------------------------------------------------------
#ifndef __VERTICAL_LINE_BREAK_H__
#define __VERTICAL_LINE_BREAK_H__

#include "VerticalCharClass.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace glyphware { struct VerticalLineLayout; }

namespace TVPVertical {

// これ以上のペナルティはブレーク禁止
constexpr float TVP_VERT_INFINITE_PENALTY = 10000.f;
// これ以下のペナルティは強制ブレーク
constexpr float TVP_VERT_FORCED_BREAK_PENALTY = -10000.f;

enum class ItemType : std::uint8_t {
	Box,        // 固定幅。グリフ (クラスタ) 1 個
	Glue,       // 伸縮するアキ
	Penalty,    // 分割の可否とコスト
};

struct LineItem {
	ItemType type = ItemType::Box;

	// Box: ボディ幅 / Penalty: ブレークしたときにその位置に現れる幅
	// (ぶら下げは負値)
	float width = 0.f;

	// Glue の伸縮 (ピクセル)
	float natural = 0.f;
	float stretch = 0.f;
	float shrink = 0.f;

	// Penalty のコスト
	float penalty = 0.f;

	// Box: 仮想ボディを詰めたときのグリフの描画オフセット
	// (始め括弧のように字面がボディ後半に寄る約物で負値になる)
	float glyphOffset = 0.f;

	// Box が指すクラスタ (Box 以外では kNoCluster)
	std::uint32_t clusterIndex = 0xFFFFFFFFu;

	// 元テキストでの位置 (UTF-8 バイトオフセット)
	std::size_t byteIndex = 0;

	static constexpr std::uint32_t kNoCluster = 0xFFFFFFFFu;

	static LineItem box(float w, std::uint32_t cluster, std::size_t byteIndex) {
		LineItem it;
		it.type = ItemType::Box;
		it.width = w;
		it.clusterIndex = cluster;
		it.byteIndex = byteIndex;
		return it;
	}
	static LineItem glue(float natural, float stretch, float shrink, std::size_t byteIndex) {
		LineItem it;
		it.type = ItemType::Glue;
		it.natural = natural;
		it.stretch = stretch;
		it.shrink = shrink;
		it.byteIndex = byteIndex;
		return it;
	}
	static LineItem penaltyItem(float cost, float width, std::size_t byteIndex) {
		LineItem it;
		it.type = ItemType::Penalty;
		it.penalty = cost;
		it.width = width;
		it.byteIndex = byteIndex;
		return it;
	}

	bool isBox() const { return type == ItemType::Box; }
	bool isGlue() const { return type == ItemType::Glue; }
	bool isPenalty() const { return type == ItemType::Penalty; }
	bool isForcedBreak() const {
		return type == ItemType::Penalty && penalty <= TVP_VERT_FORCED_BREAK_PENALTY;
	}
};

//---------------------------------------------------------------------------
// 組版オプション
//---------------------------------------------------------------------------
struct VerticalSpacingOptions {
	// 約物の詰め (JLReq のアキ量表を適用する)。false ならベタ組み
	bool punctuationSpacing = true;
	// 行末に来た句読点を版面外へ出す
	bool hangingPunctuation = false;
	// 和欧間のアキを入れる
	bool latinGap = true;
	// 字間 (em 単位)。クラスタ間の Glue として扱う
	float letterSpacing = 0.f;
};

// シェイピング結果 → Box / Glue / Penalty 列。
// 末尾には段落終端 (無限に伸びる Glue + 強制ブレークの Penalty) を付ける。
// `em` は本文のフォントサイズ (ピクセル)。
std::vector<LineItem> TVPBuildLineItems(const glyphware::VerticalLineLayout& shaped,
                                        float em,
                                        const VerticalSpacingOptions& opts);

//---------------------------------------------------------------------------
// 行分割
//---------------------------------------------------------------------------
struct LineBreakOptions {
	// 行末を揃える (グルーを伸ばして行長へ合わせる)。false でも行が溢れない
	// ように縮めはする
	bool justify = true;
};

struct BreakLine {
	std::uint32_t itemStart = 0;   // 行を構成するアイテムの開始
	std::uint32_t itemEnd = 0;     // 同・終端 (ブレーク位置。この位置は含まない)
	float ratio = 0.f;             // グルーの調整比。>0 で伸ばす、<0 で縮める
	float naturalWidth = 0.f;      // 調整前の行長
	float width = 0.f;             // 調整後の行長
};

// Greedy な行分割 (各ブレーク候補で即断)。画面のリアルタイム描画向け。
// 段全体でデメリットを最小化する Knuth-Plass はここには無い。
std::vector<BreakLine> TVPBreakLines(const std::vector<LineItem>& items,
                                     float lineLength,
                                     const LineBreakOptions& opts);

} // namespace TVPVertical

#endif // __VERTICAL_LINE_BREAK_H__
