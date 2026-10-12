//---------------------------------------------------------------------------
//!@file エンジン設定 (-userconf) — ブラウザ UI 実装
//
// appserve (external/appserve) でローカル HTTP サーバを立て、埋め込みの画面
// (common/base/userconf_web/) をブラウザのアプリ表示で開く。画面は次の API を使う:
//
//   GET  /api/userconf        項目一覧 (カテゴリ・表示名・説明・選択肢・既定値・今の値)
//   POST /api/userconf/save   {"values":{name:value,...}} を <exe名>.cfu へ書く
//   POST /api/userconf/close  終了する (保存せずに閉じる場合も、保存後も)
//
// ブラウザを閉じた場合も appserve のアイドル監視で終了する。
// -userconfbrowser=app|default|none でブラウザの開き方を選べる (既定 app)。
// 設計: doc/UserConfig.md
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "UserConfig.h"
#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "CharacterSet.h"

namespace {
bool s_userconfig_exit_flag = false;
}

void TVPSetUserConfigExitFlag(bool b) { s_userconfig_exit_flag = b; }
bool TVPGetUserConfigExitFlag() { return s_userconfig_exit_flag; }

#ifndef KRKRZ_USERCONF_WEB

bool TVPExecuteUserConfig() { return false; }

#else

#include "ReadOptionDesc.h"
#include "MsgLanguage.h"

#include <appserve/app.h>
#include <appserve/json.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string U8(const tjs_string& s)
{
	std::string out;
	TVPUtf16ToUtf8(out, s);
	return out;
}

std::filesystem::path ToFsPath(const tjs_string& s)
{
#ifdef _WIN32
	return std::filesystem::path(std::wstring(s.begin(), s.end()));
#else
	return std::filesystem::path(U8(s));
#endif
}

//---------------------------------------------------------------------------
// 設定項目
//---------------------------------------------------------------------------
struct Item {
	tjs_string name;
	bool isSelect = true;
	tjs_int length = 0;
	std::vector<tjs_string> values;   // 選択肢 (isSelect のとき)
	tjs_string def;                   // 既定値
	tjs_string current;               // 画面を開いた時点の値
};

// 選択肢の既定値。IsDefault が無ければ先頭
tjs_string DefaultOf(const tTVPCommandOption& opt)
{
	if (opt.Type == tTVPCommandOption::VT_String) return opt.Value;
	for (const auto& v : opt.Values) if (v.IsDefault) return v.Value;
	return opt.Values.empty() ? tjs_string() : opt.Values.front().Value;
}

// 今の値 (.cf / .cfu / 起動引数を反映したもの)。無ければ既定値
tjs_string CurrentOf(const tTVPCommandOption& opt, const tjs_string& def)
{
	tTJSVariant v;
	tjs_string arg = tjs_string(TJS_W("-")) + opt.Name;
	if (!TVPGetCommandLine(arg.c_str(), &v)) return def;
	tjs_string cur = ttstr(v).AsStdString();
	if (opt.Type == tTVPCommandOption::VT_Select) {
		for (const auto& c : opt.Values) if (c.Value == cur) return cur;
		return def;   // 選択肢に無い値は既定値として見せる
	}
	return cur;
}

// 画面の文言の言語 ("ja" / "en" / "chs" / "cht")。項目の説明文は optiondesc 側で選ばれている
std::string UiLanguage()
{
	std::vector<std::string> sfx = TVPGetMessageResourceSuffixes();
	std::string s = sfx.empty() ? std::string() : sfx.front();
	if (s == "-en") return "en";
	if (s == "-chs") return "chs";
	if (s == "-cht") return "cht";
	return "ja";
}

//---------------------------------------------------------------------------
// .cfu の書き出し (WINVER の旧設定ダイアログと同じ書式)
//---------------------------------------------------------------------------
const char* const kCfuHeader =
"; ============================================================================\r\n"
"; *DO NOT EDIT* this file unless you are understanding what you are doing.\r\n"
"; FYI:\r\n"
";  Each line consists of NAME=\"VALUE\" pair, VALUE is a series of\r\n"
";  \\xNN, where NN is hexadecimal representation of UNICODE codepoint.\r\n"
";  For example, opt=\"\\x61\\x62\\x63\\x3042\\x3044\\x3046\" means that the\r\n"
";  value of options \"opt\" is alphabets a, b, and c followed by Japanese\r\n"
";  Hiraganas A, I, and U.\r\n"
";  DO NOT PUT non-escaped value like opt=\"abc\". This doesn't work and should\r\n"
";  be like opt=\"\\x61\\x62\\x63\".\r\n"
"; ============================================================================\r\n";

std::string EncodeCfuValue(const tjs_string& value)
{
	std::string ret = "\"";
	char tmp[16];
	for (tjs_char ch : value) {
		snprintf(tmp, sizeof(tmp), "\\x%X", static_cast<unsigned>(ch));
		ret += tmp;
	}
	return ret + "\"";
}

// 既存の .cfu のうち、設定画面で扱わない項目の行を残す (手で足した行などを消さない)。
// 扱う項目のうち既存の .cfu にあったものの名前は present に入れる
std::vector<std::string> KeepUnmanagedLines(const std::filesystem::path& path,
                                            const std::set<std::string>& managed,
                                            std::set<std::string>& present)
{
	std::vector<std::string> kept;
	std::ifstream in(path, std::ios::binary);
	if (!in) return kept;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t p = line.find_first_not_of(" \t");
		if (p == std::string::npos || line[p] == ';') continue;
		size_t eq = line.find('=', p);
		if (eq == std::string::npos) continue;
		std::string key = line.substr(p, eq - p);
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
		if (managed.count(key)) { present.insert(key); continue; }
		kept.push_back(line);
	}
	return kept;
}

//---------------------------------------------------------------------------
// appserve モジュール
//---------------------------------------------------------------------------
class UserConfigModule : public appserve::IModule {
public:
	UserConfigModule(std::unique_ptr<tTVPCommandOptionList> options, tjs_string cfuPath)
		: options_(std::move(options)), cfuPath_(std::move(cfuPath)) {}

	const char* name() const override { return "userconf"; }

	void registerApi(appserve::ApiRegistry& reg) override
	{
		app_ = &reg.app();
		reg.route("/api/userconf/save", [this](const appserve::Request& req) { return Save(req); });
		reg.route("/api/userconf/close", [this](const appserve::Request&) {
			app_->requestShutdown(0);
			return appserve::Response::noContent();
		});
		reg.route("/api/userconf", [this](const appserve::Request& req) {
			if (!req.suffix.empty() && req.suffix != "/") return appserve::Response::error(404, "not found");
			return appserve::Response::json(Model());
		});
	}

	void onStart(appserve::App& app) override
	{
		TVPAddImportantLog(ttstr(TJS_W("UserConfig: ")) + ttstr(app.urlWithToken().c_str()));
	}

private:
	std::unique_ptr<tTVPCommandOptionList> options_;
	tjs_string cfuPath_;
	appserve::App* app_ = nullptr;
	std::vector<Item> items_;   // Model() で作る (Save で照合に使う)

	appserve::Json Model()
	{
		items_.clear();
		appserve::Json cats = appserve::Json::array();
		if (options_) {
			for (const auto& cat : options_->Categories) {
				appserve::Json opts = appserve::Json::array();
				for (const auto& opt : cat.Options) {
					if (!opt.User) continue;
					if (opt.Type != tTVPCommandOption::VT_Select && opt.Type != tTVPCommandOption::VT_String) continue;
					if (opt.Type == tTVPCommandOption::VT_Select && opt.Values.empty()) continue;
					Item it;
					it.name = opt.Name;
					it.isSelect = opt.Type == tTVPCommandOption::VT_Select;
					it.length = opt.Length;
					it.def = DefaultOf(opt);
					for (const auto& v : opt.Values) it.values.push_back(v.Value);

					appserve::Json o = appserve::Json::object();
					o.set("name", U8(opt.Name));
					o.set("caption", U8(opt.Caption));
					o.set("description", U8(opt.Description));
					o.set("type", it.isSelect ? "select" : "string");
					if (!it.isSelect) o.set("length", (long long)opt.Length);
					appserve::Json vals = appserve::Json::array();
					for (const auto& v : opt.Values) {
						appserve::Json vo = appserve::Json::object();
						vo.set("value", U8(v.Value));
						vo.set("desc", U8(v.Description));
						vals.push(vo);
					}
					o.set("values", vals);
					o.set("default", U8(it.def));
					it.current = CurrentOf(opt, it.def);
					o.set("current", U8(it.current));
					opts.push(o);
					items_.push_back(std::move(it));
				}
				if (opts.elements().empty()) continue;
				appserve::Json c = appserve::Json::object();
				c.set("name", U8(cat.Name));
				c.set("options", opts);
				cats.push(c);
			}
		}
		appserve::Json root = appserve::Json::object();
		root.set("lang", UiLanguage());
		root.set("file", U8(cfuPath_));
		root.set("categories", cats);
		return root;
	}

	appserve::Response Save(const appserve::Request& req)
	{
		if (req.method != "POST") return appserve::Response::error(405, "POST only");
		if (cfuPath_.empty()) return appserve::Response::error(500, "the settings file location is unknown");
		if (items_.empty()) Model();
		const appserve::Json& body = req.json();
		if (!body.isObj() || !body.has("values") || !body.at("values").isObj())
			return appserve::Response::error(400, "values is required");
		const appserve::Json& values = body.at("values");

		std::set<std::string> managed;
		for (const auto& it : items_) managed.insert(U8(it.name));
		std::filesystem::path path = ToFsPath(cfuPath_);
		std::set<std::string> present;
		std::vector<std::string> kept = KeepUnmanagedLines(path, managed, present);

		std::string lines;
		int written = 0;
		for (const auto& it : items_) {
			std::string name = U8(it.name);
			if (!values.has(name)) continue;
			tjs_string v;
			TVPUtf8ToUtf16(v, values.at(name).asStr());
			if (it.isSelect) {
				bool ok = false;
				for (const auto& c : it.values) if (c == v) { ok = true; break; }
				if (!ok) return appserve::Response::error(400, "invalid value for " + name);
			} else if (it.length > 0 && (tjs_int)v.size() > it.length) {
				v.resize(it.length);
			}
			// 書くのは «この画面で変えた項目» と «もともと .cfu にあった項目» だけ。
			// 変えていない項目は書かず、ゲーム側の .cf や埋め込みの設定が効くようにする
			// (.cf の値をそのまま .cfu へ写すと、後で制作側が .cf を変えても効かなくなる)
			if (v == it.current && !present.count(name)) continue;
			lines += name + "=" + EncodeCfuValue(v) + "\r\n";
			written++;
		}

		std::error_code ec;
		if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) return appserve::Response::error(500, "cannot write " + U8(cfuPath_));
		out << kCfuHeader;
		for (const auto& k : kept) out << k << "\r\n";
		out << lines;
		out.close();
		if (!out) return appserve::Response::error(500, "cannot write " + U8(cfuPath_));

		TVPAddImportantLog(ttstr(TJS_W("UserConfig: saved ")) + ttstr(cfuPath_.c_str()));
		appserve::Json r = appserve::Json::object();
		r.set("ok", true);
		r.set("file", U8(cfuPath_));
		r.set("written", written);
		return appserve::Response::json(r);
	}
};

} // anonymous

//---------------------------------------------------------------------------
bool TVPExecuteUserConfig()
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-userconf"), &val)) return false;

	TVPAddImportantLog(TJS_W("UserConfig: -userconf detected, opening the settings page in a browser"));

	std::unique_ptr<tTVPCommandOptionList> options(TVPGetEngineCommandDesc());
	if (!options) options.reset(new tTVPCommandOptionList());
	TVPMergeAllPluginCommandDesc(*options);

	appserve::App app;
	appserve::Options& o = app.options();
	o.appName = "Kirikiri Z UserConfig";
	o.logLevel = appserve::LogLevel::Warn;
	o.loadPlugins = false;
	// -userconfbrowser=app (既定。Edge / Chrome のアプリ表示) / default (既定のブラウザ) /
	// none (開かない。URL はログに出る。検証や別の端末から開く場合)
	tTJSVariant bv;
	if (TVPGetCommandLine(TJS_W("-userconfbrowser"), &bv)) {
		ttstr b(bv);
		if (b == TJS_W("none")) o.browser = appserve::Options::BrowserMode::None;
		else if (b == TJS_W("default")) o.browser = appserve::Options::BrowserMode::Default;
	}
	app.addModule(std::make_unique<UserConfigModule>(std::move(options), TVPGetUserConfigFilePath()));
	int code = app.run();
	if (code != 0)
		TVPAddImportantLog(ttstr(TJS_W("UserConfig: the settings server ended with code ")) + ttstr((tjs_int)code));
	return true;
}

#endif // KRKRZ_USERCONF_WEB
