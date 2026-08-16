//---------------------------------------------------------------------------
// Elements の block text バックエンド (cycfi::elements::block_text_backend) を
// glyphware で実装する。
//
// これで Elements の text_area ウィジェットと本体 Layer.drawShapedTextArea が
// **同じ折返しロジック** (glyphware::layoutBlock: 段落分割・単語/文字単位の
// 折返し・行頭行末禁則・整列・クラスタ count 制限) を通る。以前は Elements 側
// が ThorVG/cycfi の素朴な幅貪欲 wrap で、禁則も count も無かった。
//
// 描画そのものは Elements 側 (ThorVG 経由) のまま。ここが決めるのは
// 「どこで改行するか」「どこまで見せるか」だけなので、行内のグリフ配置は
// 従来どおり ThorVG が行う。
//
// フォント鍵: Elements の font::file() は register_font に渡したホストキー
// (= krkrz の storage パス) なので、glyphware の名前解決へそのまま渡せる。
// フォールバック連鎖は theme の families 並び (Latin → CJK → Emoji) を
// Elements の font_map で鍵へ引き直して復元する (= ThorVG の per-codepoint
// fallback と同じ優先順)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#if defined(KRKRZ_USE_GLYPHWARE)

#include "StoragesResourceLoader.h"   // TVPGetElementsDefaultFontFamily
#include "GlyphwareHost.h"            // EffectiveKey / BuildChain
#include "CharacterSet.h"             // TVPUtf16ToUtf8
#include "glyphware/Layout.h"

#include <elements/support/block_text.hpp>
#include <elements/support/font.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using cycfi::elements::block_text_request;
using cycfi::elements::block_text_result;
using cycfi::elements::block_text_line;

// comma 区切り families 文字列を分割 (前後空白は落とす)
void SplitFamilies(const std::string& s, std::vector<std::string>& out)
{
	std::string::size_type pos = 0;
	while (pos <= s.size()) {
		std::string::size_type comma = s.find(',', pos);
		std::string tok = s.substr(pos, comma == std::string::npos
			? std::string::npos : comma - pos);
		const auto b = tok.find_first_not_of(" \t");
		const auto e = tok.find_last_not_of(" \t");
		if (b != std::string::npos) out.push_back(tok.substr(b, e - b + 1));
		if (comma == std::string::npos) break;
		pos = comma + 1;
	}
}

// widget のフォント鍵 → glyphware 連鎖鍵 (comma 区切り)。
// 先頭は widget 自身の鍵、続いて theme families を鍵へ引き直したもの。
std::string BuildChainKey(const block_text_request& req)
{
	std::vector<std::string> keys;
	auto push = [&keys](const std::string& k) {
		if (k.empty()) return;
		if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
	};
	push(req.font_key);

	std::string families;
	{
		ttstr f = TVPGetElementsDefaultFontFamily();
		if (!f.IsEmpty()) TVPUtf16ToUtf8(families, tjs_string(f.c_str()));
	}
	std::vector<std::string> fams;
	SplitFamilies(families, fams);
	for (const std::string& fam : fams) {
		// font_descr は families を string_view で持つので fam を生かしたまま
		// font を作る (ここで font_map から実ファイル鍵へ解決される)。
		cycfi::elements::font_descr d{ std::string_view(fam) };
		cycfi::elements::font fnt{ d };
		push(fnt.file());
	}

	std::string chain;
	for (const std::string& k : keys) {
		if (!chain.empty()) chain += ',';
		chain += k;
	}
	return chain;
}

bool PrepareChain(const block_text_request& req,
                  std::vector<std::shared_ptr<glyphware::Face>>& chain)
{
	if (req.size <= 0) return false;
	std::string key = TVPGlyphwareEffectiveKey(BuildChainKey(req));
	if (key.empty()) return false;
	TVPGlyphwareBuildChain(key, chain);
	return !chain.empty();
}

glyphware::BaseDirection ToBaseDir(int base)
{
	return base == block_text_request::dir_ltr ? glyphware::BaseDirection::LTR
	     : base == block_text_request::dir_rtl ? glyphware::BaseDirection::RTL
	                                           : glyphware::BaseDirection::Auto;
}

class tTVPGlyphwareBlockTextBackend : public cycfi::elements::block_text_backend
{
public:
	bool layout(const block_text_request& req, block_text_result& out) override
	{
		out = block_text_result{};
		std::vector<std::shared_ptr<glyphware::Face>> chain;
		if (!PrepareChain(req, chain)) return false;

		const int px = static_cast<int>(req.size + 0.5f);
		glyphware::BlockOptions bo;
		bo.width = req.width;
		bo.height = req.height;
		bo.lineSpacing = req.line_spacing;
		bo.align = req.align == block_text_request::align_center
		             ? glyphware::Align::Center
		         : req.align == block_text_request::align_right
		             ? glyphware::Align::Right
		             : glyphware::Align::Left;
		bo.count = req.count;

		glyphware::BlockLayout block = glyphware::layoutBlock(
			std::string_view(req.text.data(), req.text.size()),
			ToBaseDir(req.base), chain, px, bo);

		out.width = block.width;
		out.height = block.height;
		out.line_height = block.lineHeight;
		out.ascent = block.ascent;
		out.descent = block.descent;
		out.drawn_clusters = block.drawnClusters;
		out.total_clusters = block.totalClusters;
		out.lines.reserve(block.lines.size());
		for (const glyphware::BlockLine& bl : block.lines) {
			block_text_line line;
			line.start = bl.byteStart;
			line.end = bl.byteEnd;
			line.reveal_end = bl.revealEnd;
			line.x = bl.x;
			line.y = bl.y;
			line.width = bl.layout.width;
			line.clusters = bl.clusters;
			line.total_clusters = bl.totalClusters;
			out.lines.push_back(line);
		}
		return true;
	}

	int count_clusters(const block_text_request& req) override
	{
		std::vector<std::shared_ptr<glyphware::Face>> chain;
		if (!PrepareChain(req, chain)) return 0;
		return glyphware::countClusters(
			std::string_view(req.text.data(), req.text.size()),
			ToBaseDir(req.base), chain, static_cast<int>(req.size + 0.5f));
	}

};

tTVPGlyphwareBlockTextBackend gBackend;

} // namespace

//---------------------------------------------------------------------------
void TVPInstallElementsBlockTextBackend()
{
	cycfi::elements::set_block_text_backend(&gBackend);
}
//---------------------------------------------------------------------------

#else   // !KRKRZ_USE_GLYPHWARE

// glyphware 無しビルドでは Elements 内蔵の折返しがそのまま使われる。
void TVPInstallElementsBlockTextBackend() {}

#endif // KRKRZ_USE_GLYPHWARE
