//---------------------------------------------------------------------------
// メッセージ / オプション記述資材の言語選択 (詳細は MsgLanguage.h)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "MsgLanguage.h"
#include "SysInitIntf.h"   // TVPGetCommandLine
#include "SystemIntf.h"    // TVPGetSystemLanguage

namespace {

// ASCII 前提の小文字化 (言語タグ用)
std::string ToLowerAscii(const tjs_string &in)
{
	std::string ret;
	ret.reserve(in.size());
	for (tjs_char c : in) {
		if (c >= TJS_W('A') && c <= TJS_W('Z')) c = c - TJS_W('A') + TJS_W('a');
		if (c < 0x80) ret.push_back(static_cast<char>(c));
	}
	return ret;
}

bool StartsWith(const std::string &s, const char *prefix)
{
	return s.rfind(prefix, 0) == 0;
}

bool Contains(const std::string &s, const char *sub)
{
	return s.find(sub) != std::string::npos;
}

} // anonymous

std::vector<std::string> TVPGetMessageResourceSuffixesForTag(const tjs_string &tag)
{
	const std::string t = ToLowerAscii(tag);

	// 日本語 (または不明) → 基底資材のみ
	if (t.empty() || StartsWith(t, "ja"))
		return { "" };

	// 繁体字中国語: zh-Hant / zh-TW / zh-HK / zh-MO / 短縮形 cht
	if (t == "cht" ||
	    (StartsWith(t, "zh") && (Contains(t, "hant") || Contains(t, "-tw") ||
	                             Contains(t, "-hk") || Contains(t, "-mo"))))
		return { "-cht", "-chs", "-en", "" };

	// 簡体字中国語 (その他の zh) / 短縮形 chs
	if (t == "chs" || StartsWith(t, "zh"))
		return { "-chs", "-en", "" };

	// それ以外は英語資材を優先
	return { "-en", "" };
}

tjs_string TVPGetEffectiveMessageLanguageTag()
{
	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-language"), &val)) {
		ttstr s(val);
		if (!s.IsEmpty()) return s.AsStdString();
	}
	return TVPGetSystemLanguage().AsStdString();
}

std::vector<std::string> TVPGetMessageResourceSuffixes()
{
	return TVPGetMessageResourceSuffixesForTag(TVPGetEffectiveMessageLanguageTag());
}
