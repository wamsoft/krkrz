//---------------------------------------------------------------------------
// UserConfig オプション記述ローダ (ファイルベース版: SDL3 / LIB / その他)
//
// JSON パーサ・マージは common/msg/ReadOptionDescUtil.cpp に共通化済み。
// このファイルは:
//   - Engine: resource/optiondesc.json から読む
//   - Plugin: 各 .dll/.so に隣接する `<basename>.options.json` から読む
// 既存 plugin に対しては JSON ファイル無し = nullptr 返しで縮退。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ReadOptionDesc.h"
#include "MsgLanguage.h"
#include "DebugIntf.h"
#include "Application.h"
#include "CharacterSet.h"
#include "StorageIntf.h"

#include <vector>
#include <string>
#include <filesystem>
#include <memory>

namespace {

// ファイルをそのまま読む (<= 1MB 想定)。不在は空返し
// (TVPReadStream は不在ファイルで例外を投げるため、先に存在確認する)
std::vector<char> ReadFileBytes(const tjs_string& path)
{
	std::vector<char> ret;
	if (!TVPIsExistentStorage(ttstr(path.c_str()))) return ret;
	tjs_uint64 flen = 0;
	auto buf = TVPReadStream(path.c_str(), &flen);
	if (!buf || flen == 0) return ret;
	ret.assign(reinterpret_cast<const char*>(buf.get()),
	           reinterpret_cast<const char*>(buf.get()) + flen);
	return ret;
}

// プラグインパスから sidecar JSON パスを導出。
// 例: "C:/foo/myplugin.dll" → "C:/foo/myplugin.options.json"
tjs_string DeriveSidecarJsonPath(const tjs_char* plugin_name)
{
	if (!plugin_name) return tjs_string();
	tjs_string in(plugin_name);
	// 末尾の拡張子のみ剥がす (パス区切りを跨がない)
	auto last_sep = in.find_last_of(TJS_W("/\\"));
	auto last_dot = in.find_last_of(TJS_W('.'));
	tjs_string base = (last_dot != tjs_string::npos &&
	                   (last_sep == tjs_string::npos || last_dot > last_sep))
	                  ? in.substr(0, last_dot)
	                  : in;
	return base + TJS_W(".options.json");
}

} // anonymous

tTVPCommandOptionList* TVPGetEngineCommandDesc()
{
	if (!Application) return nullptr;
	// 言語 suffix 候補 (-language= / OS 言語) を優先順に試す。
	// 例: en-US → optiondesc-en.json → optiondesc.json
	tjs_string path;
	for (const std::string &sfx : TVPGetMessageResourceSuffixes()) {
		tjs_string wsfx(sfx.begin(), sfx.end());
		path = Application->ResourcePath() + TJS_W("optiondesc") + wsfx + TJS_W(".json");
		auto bytes = ReadFileBytes(path);
		if (bytes.empty()) continue;
		return TVPParseCommandDescJson(bytes.data(), bytes.size());
	}
	TVPAddImportantLog(ttstr(TJS_W("UserConfig: optiondesc.json not found at: "))
		+ ttstr(path.c_str()));
	return nullptr;
}

tTVPCommandOptionList* TVPGetPluginCommandDesc(const tjs_char* name)
{
	if (!name) return nullptr;
	tjs_string path = DeriveSidecarJsonPath(name);
	auto bytes = ReadFileBytes(path);
	if (bytes.empty()) return nullptr; // sidecar 無しは正常 (plugin 任意)
	return TVPParseCommandDescJson(bytes.data(), bytes.size());
}

//---------------------------------------------------------------------------
// 全プラグインの option desc を dest へマージする (設定画面 -userconf 用)。
// exe のフォルダとプラグインフォルダにある `<basename>.options.json` を読む。
//---------------------------------------------------------------------------
void TVPMergeAllPluginCommandDesc(tTVPCommandOptionList& dest)
{
	if (!Application) return;
	std::vector<tjs_string> dirs;
	dirs.push_back(Application->AppPath());
	if (!Application->PluginPath().empty() && Application->PluginPath() != Application->AppPath())
		dirs.push_back(Application->PluginPath());
	static const tjs_char suffix[] = TJS_W(".options.json");
	const size_t slen = sizeof(suffix) / sizeof(suffix[0]) - 1;
	for (const tjs_string& dir : dirs) {
		std::error_code ec;
#ifdef _WIN32
		std::filesystem::path dirpath(std::wstring(dir.begin(), dir.end()));
#else
		std::string dir8;
		TVPUtf16ToUtf8(dir8, dir);
		std::filesystem::path dirpath(dir8);
#endif
		for (std::filesystem::directory_iterator it(dirpath, ec), end; !ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec)) continue;
			tjs_string fn;
#ifdef _WIN32
			std::wstring wfn = it->path().filename().wstring();
			fn.assign(wfn.begin(), wfn.end());
#else
			TVPUtf8ToUtf16(fn, it->path().filename().string());
#endif
			if (fn.size() <= slen || fn.compare(fn.size() - slen, slen, suffix) != 0) continue;
			auto bytes = ReadFileBytes(dir + fn);
			if (bytes.empty()) continue;
			std::unique_ptr<tTVPCommandOptionList> options(TVPParseCommandDescJson(bytes.data(), bytes.size()));
			if (options) TVPMargeCommandDesc(dest, *options);
		}
	}
}
