//---------------------------------------------------------------------------
// -display= 起動オプション (起動するディスプレイの指定) 共通部
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "DisplaySelect.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"

//---------------------------------------------------------------------------
namespace {
//! ASCII 範囲のみ小文字化した複製を返す (名前に非 ASCII が混じっていても安全)
tjs_string ASCIIToLower(const tjs_string &s)
{
	tjs_string r(s);
	for (auto &c : r) {
		if (c >= TJS_W('A') && c <= TJS_W('Z')) c = (tjs_char)(c - TJS_W('A') + TJS_W('a'));
	}
	return r;
}
//! 全て数字なら 1 origin の番号を返す。数字でなければ -1
tjs_int ParseNumber(const tjs_string &s)
{
	if (s.empty()) return -1;
	tjs_int v = 0;
	for (auto c : s) {
		if (c < TJS_W('0') || c > TJS_W('9')) return -1;
		v = v * 10 + (tjs_int)(c - TJS_W('0'));
		if (v > 1000) return -1; // 明らかに範囲外
	}
	return v;
}
//! 部分一致 (ASCII は大小無視)
bool Contains(const tjs_string &haystack, const tjs_string &needle)
{
	if (needle.empty()) return false;
	return ASCIIToLower(haystack).find(ASCIIToLower(needle)) != tjs_string::npos;
}
} // anonymous namespace
//---------------------------------------------------------------------------
bool TVPGetStartupDisplayOption(tjs_string &opt)
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-display"), &val)) return false;
	ttstr s = val;
	if (s.IsEmpty()) return false;
	opt = s.AsStdString();
	return !opt.empty();
}
//---------------------------------------------------------------------------
bool TVPIsDisplayListRequest(const tjs_string &opt)
{
	tjs_string l = ASCIIToLower(opt);
	return l == TJS_W("list") || l == TJS_W("?") || l == TJS_W("help");
}
//---------------------------------------------------------------------------
void TVPLogDisplayList(const std::vector<tTVPDisplayEntry> &list)
{
	TVPAddImportantLog(ttstr(TJS_W("(info) available displays (-display=<number|name>):")));
	for (const auto &e : list) {
		ttstr line(TJS_W("(info)   -display="));
		line += ttstr((tjs_int)e.index);
		line += ttstr(TJS_W(" : "));
		line += ttstr(e.name.empty() ? e.device : e.name);
		if (!e.device.empty() && !e.name.empty()) {
			line += ttstr(TJS_W(" ["));
			line += ttstr(e.device);
			line += ttstr(TJS_W("]"));
		}
		line += ttstr(TJS_W(" "));
		line += ttstr((tjs_int)e.width);
		line += ttstr(TJS_W("x"));
		line += ttstr((tjs_int)e.height);
		line += ttstr(TJS_W(" at ("));
		line += ttstr((tjs_int)e.left);
		line += ttstr(TJS_W(","));
		line += ttstr((tjs_int)e.top);
		line += ttstr(TJS_W(")"));
		if (e.primary) line += ttstr(TJS_W(" (primary)"));
		TVPAddImportantLog(line);
	}
}
//---------------------------------------------------------------------------
tjs_int TVPMatchDisplay(const tjs_string &opt, const std::vector<tTVPDisplayEntry> &list)
{
	if (list.empty() || opt.empty()) return -1;

	// 番号指定 (1 origin)
	tjs_int no = ParseNumber(opt);
	if (no > 0) {
		for (tjs_uint i = 0; i < (tjs_uint)list.size(); i++) {
			if (list[i].index == no) return (tjs_int)i;
		}
		return -1;
	}

	if (ASCIIToLower(opt) == TJS_W("primary")) {
		for (tjs_uint i = 0; i < (tjs_uint)list.size(); i++) {
			if (list[i].primary) return (tjs_int)i;
		}
		return -1;
	}

	// 名前 / デバイス名の部分一致。完全一致を優先する
	for (tjs_uint i = 0; i < (tjs_uint)list.size(); i++) {
		if (ASCIIToLower(list[i].name) == ASCIIToLower(opt) ||
			ASCIIToLower(list[i].device) == ASCIIToLower(opt)) return (tjs_int)i;
	}
	for (tjs_uint i = 0; i < (tjs_uint)list.size(); i++) {
		if (Contains(list[i].name, opt) || Contains(list[i].device, opt)) return (tjs_int)i;
	}
	return -1;
}
//---------------------------------------------------------------------------
