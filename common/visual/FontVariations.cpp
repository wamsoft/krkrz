//---------------------------------------------------------------------------
// バリアブルフォント (可変軸) 指定文字列の正規化とパース (詳細は FontVariations.h)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "FontVariations.h"
#include "tvpfontstruc.h"
#include "MsgIntf.h"    // TVPThrowExceptionMessage / TVPInvalidParam

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// Font.defaultUseVarStyle (詳細はヘッダ)。既定 OFF。
bool TVPFontDefaultUseVarStyle = false;

namespace {

// "tag=value" 1 項目。tag は小文字化済み ASCII (1..4 文字)。
struct ParsedVar {
	std::string tag;
	float value;
};

// spec を項目列へ分解する。strict=true なら不正トークンで throw、false なら skip。
void ParseSpec(const ttstr& spec, std::vector<ParsedVar>& out, bool strict)
{
	const tjs_char* p = spec.c_str();
	const tjs_int len = spec.GetLen();
	tjs_int s = 0;
	while (s <= len) {
		// カンマで区切る (最終トークンは終端まで)
		tjs_int e = s;
		while (e < len && p[e] != TJS_W(',')) ++e;
		// trim
		tjs_int b = s, t = e;
		while (b < t && (p[b] == TJS_W(' ') || p[b] == TJS_W('\t'))) ++b;
		while (t > b && (p[t-1] == TJS_W(' ') || p[t-1] == TJS_W('\t'))) --t;
		if (b < t) {
			// tag=value
			tjs_int eq = b;
			while (eq < t && p[eq] != TJS_W('=')) ++eq;
			bool ok = (eq > b) && (eq < t);
			std::string tag;
			float value = 0.f;
			if (ok) {
				// タグ: 1..4 文字の ASCII 可視文字。小文字化。
				tjs_int tagend = eq;
				while (tagend > b && (p[tagend-1] == TJS_W(' ') || p[tagend-1] == TJS_W('\t'))) --tagend;
				tjs_int taglen = tagend - b;
				if (taglen < 1 || taglen > 4) ok = false;
				for (tjs_int i = b; ok && i < tagend; ++i) {
					tjs_char c = p[i];
					if (c < 0x21 || c > 0x7e || c == TJS_W('=') || c == TJS_W(',')) ok = false;
					else {
						if (c >= TJS_W('A') && c <= TJS_W('Z')) c = c - TJS_W('A') + TJS_W('a');
						tag.push_back(static_cast<char>(c));
					}
				}
				// 値: 10 進数値
				if (ok) {
					std::string num;
					for (tjs_int i = eq + 1; i < t; ++i) {
						tjs_char c = p[i];
						if (c == TJS_W(' ') || c == TJS_W('\t')) continue;
						if (c > 0x7e) { ok = false; break; }
						num.push_back(static_cast<char>(c));
					}
					if (ok && !num.empty()) {
						char* endp = nullptr;
						value = std::strtof(num.c_str(), &endp);
						if (!endp || *endp != '\0') ok = false;
					} else ok = false;
				}
			}
			if (ok) {
				out.push_back({ std::move(tag), value });
			} else if (strict) {
				TVPThrowExceptionMessage(TVPInvalidParam);
			}
		}
		if (e >= len) break;
		s = e + 1;
	}
}

// 量子化: wght = 1 刻み、その他 = 0.5 刻み (軸アニメーションでキャッシュが
// 際限なく増えるのを防ぐ)。
float Quantize(const std::string& tag, float v)
{
	const float step = (tag == "wght") ? 1.0f : 0.5f;
	return std::round(v / step) * step;
}

// 数値の文字列化 (余計な末尾ゼロを付けない: 700 / 87.5)
std::string FormatValue(float v)
{
	char buf[48];
	if (v == std::floor(v) && std::fabs(v) < 1e7f)
		std::snprintf(buf, sizeof(buf), "%.0f", v);
	else
		std::snprintf(buf, sizeof(buf), "%g", v);
	return buf;
}

} // anonymous

//---------------------------------------------------------------------------
ttstr TVPNormalizeFontVariations(const ttstr& spec)
{
	if (spec.IsEmpty()) return ttstr();
	std::vector<ParsedVar> items;
	ParseSpec(spec, items, true /*strict*/);
	// 量子化 → タグ昇順 (stable: 同タグの相対順維持) → 重複は後勝ち
	for (auto& it : items) it.value = Quantize(it.tag, it.value);
	std::stable_sort(items.begin(), items.end(),
		[](const ParsedVar& a, const ParsedVar& b) { return a.tag < b.tag; });
	std::string out;
	for (size_t i = 0; i < items.size(); ++i) {
		if (i + 1 < items.size() && items[i + 1].tag == items[i].tag) continue; // 後勝ち
		if (!out.empty()) out += ",";
		out += items[i].tag;
		out += "=";
		out += FormatValue(items[i].value);
	}
	return ttstr(out.c_str());
}
//---------------------------------------------------------------------------
tjs_uint32 TVPFontVarPackTag(const char* tag, size_t len)
{
	char c[4] = { ' ', ' ', ' ', ' ' };
	for (size_t i = 0; i < 4 && i < len; ++i) c[i] = tag[i];
	return (static_cast<tjs_uint32>(static_cast<unsigned char>(c[0])) << 24) |
	       (static_cast<tjs_uint32>(static_cast<unsigned char>(c[1])) << 16) |
	       (static_cast<tjs_uint32>(static_cast<unsigned char>(c[2])) << 8) |
	        static_cast<tjs_uint32>(static_cast<unsigned char>(c[3]));
}
//---------------------------------------------------------------------------
void TVPParseFontVariations(const ttstr& spec, std::vector<tTVPFontAxisCoord>& out)
{
	if (spec.IsEmpty()) return;
	std::vector<ParsedVar> items;
	ParseSpec(spec, items, false /*lenient*/);
	for (const auto& it : items) {
		tjs_uint32 tag = TVPFontVarPackTag(it.tag.c_str(), it.tag.size());
		// 後勝ち: 既存の同タグを上書き
		bool replaced = false;
		for (auto& o : out) if (o.first == tag) { o.second = it.value; replaced = true; break; }
		if (!replaced) out.emplace_back(tag, it.value);
	}
}
//---------------------------------------------------------------------------
void TVPFontGetEffectiveVarCoords(tjs_int weight, const ttstr& variations,
                                  std::vector<tTVPFontAxisCoord>& out)
{
	TVPParseFontVariations(variations, out);
	if (weight >= 0) {
		const tjs_uint32 wght = TVPFontVarPackTag("wght", 4);
		bool has = false;
		for (const auto& o : out) if (o.first == wght) { has = true; break; }
		if (!has) out.emplace_back(wght, static_cast<float>(weight));
	}
}
//---------------------------------------------------------------------------
void TVPFontGetEffectiveVarCoords(const tTVPFont& font,
                                  std::vector<tTVPFontAxisCoord>& out)
{
	TVPFontGetEffectiveVarCoords(font.Weight, font.Variations, out);
}
