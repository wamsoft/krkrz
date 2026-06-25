//---------------------------------------------------------------------------
// Elements ベース UserConfig (SDL3 実装、 elements_modal ベース)
//
// `-userconf` 起動時に専用 SDL_Window を立ち上げ、 Elements UI で各オプションを
// 編集して .cfu ファイルに保存する。
//
// 実装の柱:
//   - tTVPCommandOptionList を読み込んで User=true の項目を picojson で
//     dynamic に JSON 化し、 elements_modal::run_modal でモーダル実行
//   - VT_Select は selection_menu (ドロップダウン) で値選択
//   - VT_String は MVP では disabled な label のみ
//   - 結果の result.values から変更箇所を抽出し .cfu に書き出し
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ElementsUserConfig.h"
#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "BinaryStream.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "Application.h"
#include "ReadOptionDesc.h"
#include "ElementsDialogManager.h"   // EnsureRuntimeInitialized 共有

#include <elements_modal/modal.h>

#include <picojson/picojson.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

bool s_userconfig_executed_flag = false;

//---------------------------------------------------------------------------
// 文字列変換補助
//---------------------------------------------------------------------------
std::string ToUtf8(const tjs_string& s)
{
	std::string out;
	TVPUtf16ToUtf8(out, s);
	return out;
}

tjs_string Utf8ToTjsStr(const std::string& s)
{
	tjs_string out;
	TVPUtf8ToUtf16(out, s);
	return out;
}

//---------------------------------------------------------------------------
// .cfu ファイル (WINVER ConfigFormUnit::SaveSetting と互換フォーマット)
//---------------------------------------------------------------------------
constexpr const char* kCfuWarnings =
"; ============================================================================\r\n"
"; *DO NOT EDIT* this file unless you are understanding what you are doing.\r\n"
"; FYI:\r\n"
";  Each line consists of NAME=\"VALUE\" pair, VALUE is a series of\r\n"
";  \\xNN, where NN is hexadecimal representation of UNICODE codepoint.\r\n"
"; ============================================================================\r\n";

std::string EncodeCfuValue(const tjs_string& value)
{
	if (value.empty()) return "\"\"";
	std::string ret = "\"";
	for (tjs_char ch : value) {
		char tmp[16];
		TJS_nsprintf(tmp, "\\x%X", static_cast<unsigned>(ch));
		ret += tmp;
	}
	return ret + "\"";
}

// exe フルパスから basename を抜き出し、 末尾拡張子を ".cfu" に置換した
// ファイル名を返す。 FilePathUtil.h (Win32 専用) の
// ChangeFileExt(ExtractFileName(...), ".cfu") 相当を素の string 処理で実装。
tjs_string DeriveCfuFileNameFromExe(const tjs_string& exepath)
{
	auto last_sep = exepath.find_last_of(TJS_W("/\\"));
	tjs_string name = (last_sep == tjs_string::npos)
	                  ? exepath
	                  : exepath.substr(last_sep + 1);
	auto last_dot = name.find_last_of(TJS_W('.'));
	if (last_dot != tjs_string::npos) name = name.substr(0, last_dot);
	return name + TJS_W(".cfu");
}

tjs_string ResolveCfuFilePath()
{
	tjs_string datapath = Application->LogPath();
	tjs_string exename  = Application->ExePath();
	tjs_string filename = DeriveCfuFileNameFromExe(exename);
	if (!datapath.empty() && datapath.back() != TJS_W('/') && datapath.back() != TJS_W('\\')) {
		datapath += TJS_W("/");
	}
	return datapath + filename;
}

bool WriteCfuFile(const std::vector<std::pair<tjs_string, tjs_string>>& entries)
{
	tjs_string path = ResolveCfuFilePath();
	if (path.empty()) {
		TVPAddImportantLog(TJS_W("ElementsUserConfig: cannot resolve .cfu path"));
		return false;
	}
	iTJSBinaryStream* stream = nullptr;
	try {
		stream = TVPCreateBinaryStreamForWrite(ttstr(path.c_str()), TJS_W(""));
	} catch (...) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsUserConfig: failed to open: ")) + ttstr(path.c_str()));
		return false;
	}
	if (!stream) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsUserConfig: failed to open: ")) + ttstr(path.c_str()));
		return false;
	}
	try {
		stream->Write(kCfuWarnings, static_cast<tjs_uint>(strlen(kCfuWarnings)));
		for (const auto& kv : entries) {
			std::string name_utf8 = ToUtf8(kv.first);
			std::string line = name_utf8 + "=" + EncodeCfuValue(kv.second) + "\r\n";
			stream->Write(line.c_str(), static_cast<tjs_uint>(line.length()));
		}
	} catch (...) {
		delete stream;
		throw;
	}
	delete stream;
	TVPAddImportantLog(ttstr(TJS_W("ElementsUserConfig: saved to: ")) + ttstr(path.c_str()));
	return true;
}

//---------------------------------------------------------------------------
// picojson 構築ヘルパ (object/array リテラル相当の短縮形)
//---------------------------------------------------------------------------
using picojson::value;
using picojson::object;
using picojson::array;

value Num(double d) { return value(d); }
value Str(std::string s) { return value(s); }
value Obj(object o) { return value(o); }
value Arr(array a) { return value(a); }

value Padding(double l, double t, double r, double b)
{
	return Arr({Num(l), Num(t), Num(r), Num(b)});
}

value BgRGBA(int r, int g, int b, int a)
{
	return Arr({Num(r), Num(g), Num(b), Num(a)});
}

value Label(const std::string& text, double size = 1.0)
{
	object o{
		{"type", Str("label")},
		{"text", Str(text)}
	};
	if (size != 1.0) o["size"] = Num(size);
	return Obj(std::move(o));
}

value TopMargin(double v, value child)
{
	return Obj({
		{"type", Str("top_margin")},
		{"value", Num(v)},
		{"child", std::move(child)}
	});
}

value LeftMargin(double v, value child)
{
	return Obj({
		{"type", Str("left_margin")},
		{"value", Num(v)},
		{"child", std::move(child)}
	});
}

value AlignCenter(value child)
{
	return Obj({{"type", Str("align_center")}, {"child", std::move(child)}});
}

value AlignLeft(value child)
{
	return Obj({{"type", Str("align_left")}, {"child", std::move(child)}});
}

value AlignRight(value child)
{
	return Obj({{"type", Str("align_right")}, {"child", std::move(child)}});
}

value HSize(double w, value child)
{
	return Obj({
		{"type", Str("hsize")}, {"width", Num(w)}, {"child", std::move(child)}
	});
}

value HMinSize(double w, value child)
{
	return Obj({
		{"type", Str("hmin_size")}, {"width", Num(w)}, {"child", std::move(child)}
	});
}

value VMinSize(double h, value child)
{
	return Obj({
		{"type", Str("vmin_size")}, {"height", Num(h)}, {"child", std::move(child)}
	});
}

value Margin(value padding, value child)
{
	return Obj({
		{"type", Str("margin")},
		{"padding", std::move(padding)},
		{"child", std::move(child)}
	});
}

value Scroller(value child)
{
	return Obj({{"type", Str("scroller")}, {"child", std::move(child)}});
}

value VTile(array children)
{
	return Obj({
		{"type", Str("vtile")}, {"children", Arr(std::move(children))}
	});
}

value HTile(array children)
{
	return Obj({
		{"type", Str("htile")}, {"children", Arr(std::move(children))}
	});
}

value Button(const std::string& id, const std::string& text,
             bool initial_focus = false, bool close_on_click = false)
{
	object o{
		{"type", Str("button")}, {"id", Str(id)}, {"text", Str(text)}
	};
	if (initial_focus)  o["initial_focus"]  = value(true);
	if (close_on_click) o["close_on_click"] = value(true);
	return value(std::move(o));
}

//! 入力設定 (input ブロック)。 arrow 2D navigation 有効化 +
//! LB/RB を cancel/save にショートカット。 force=true は input_box
//! 編集中でも反応させるため (リスト編集中の save 押下を許容)。
value InputBlock()
{
	return Obj({
		{"arrow_focus_nav", value(true)},
		{"dpad_mode",        Str("both")},
		{"left_stick_mode",  Str("focus")},
		{"shortcuts", Arr({
			Obj({{"pad", Str("lb")}, {"target", Str("cancel")}}),
			Obj({{"pad", Str("rb")}, {"target", Str("save")},
			     {"force", value(true)}})
		})}
	});
}

value SelectionMenu(const std::string& id, const array& options, int selected)
{
	return Obj({
		{"type", Str("selection_menu")},
		{"id", Str(id)},
		{"options", Arr(options)},
		{"selected", Num(selected)}
	});
}

//---------------------------------------------------------------------------
// Option を id でアクセスするためのマップ
//---------------------------------------------------------------------------
struct OptionRef
{
	const tTVPCommandOption* opt;
	size_t default_index;   // VT_Select の場合
};

size_t FindDefaultIndex(const tTVPCommandOption& opt)
{
	for (size_t i = 0; i < opt.Values.size(); ++i) {
		if (opt.Values[i].IsDefault) return i;
	}
	return 0;
}

//---------------------------------------------------------------------------
// オプション一覧 JSON を構築
//
// id_map[id] = OptionRef を埋めながら、 各 User=true option の行を生成。
// JSON top-level に size/background を持ち、 content にスクロール領域 + 保存/
// キャンセルボタンを配置。
//---------------------------------------------------------------------------
std::string BuildOptionsJson(const tTVPCommandOptionList& options,
                             std::map<std::string, OptionRef>& id_map)
{
	array rows;
	int counter = 0;

	for (const auto& cat : options.Categories) {
		bool has_user = false;
		for (const auto& opt : cat.Options) if (opt.User) { has_user = true; break; }
		if (!has_user) continue;

		// カテゴリ見出し (font-size 1.2 の左寄せ label)
		rows.push_back(TopMargin(10, AlignLeft(Label(ToUtf8(cat.Name), 1.2))));

		for (const auto& opt : cat.Options) {
			if (!opt.User) continue;

			std::string id = "opt_" + std::to_string(counter++);
			OptionRef ref{ &opt, FindDefaultIndex(opt) };
			id_map[id] = ref;

			value right_widget;
			if (opt.Type == tTVPCommandOption::VT_Select && !opt.Values.empty()) {
				array option_labels;
				for (const auto& v : opt.Values) {
					option_labels.push_back(Str(ToUtf8(v.Description)));
				}
				int sel = static_cast<int>(ref.default_index);
				if (sel < 0 || static_cast<size_t>(sel) >= opt.Values.size()) sel = 0;
				right_widget = SelectionMenu(id, option_labels, sel);
			} else if (opt.Type == tTVPCommandOption::VT_String) {
				// MVP: 編集 UI 未実装 (Phase 7d 候補)
				right_widget = Label("(string opt - editing TBD)");
			} else {
				right_widget = Label("(unsupported)");
			}

			// hmin_size を 280→250 に。 view extent 600 logical 内に 2 列 +
			// scroller の右マージン (スクロールバー幅 ~15) を確保するため。
			rows.push_back(TopMargin(4, HTile({
				HMinSize(250, Label(ToUtf8(opt.Caption))),
				HMinSize(250, std::move(right_widget))
			})));
		}
	}

	// 行リストが空なら fallback メッセージ
	if (rows.empty()) {
		rows.push_back(AlignCenter(Label("(no user options)")));
	}

	// scroller の minimum 高さ。 view extent 高さ - (title + footer + margin)
	// を超えないよう抑える (= 初期表示で content が view 内に収まる)。
	// 内側 right padding は scroller の縦スクロールバー幅分 (~15) を確保。
	value scroll_area = VMinSize(500, Scroller(Margin(
		Padding(0, 0, 20, 0),
		VTile(std::move(rows))
	)));

	value footer = AlignRight(HTile({
		HSize(120, Button("save", "Save",
		                  /*initial_focus=*/true, /*close_on_click=*/true)),
		LeftMargin(20, HSize(120, Button("cancel", "Cancel",
		                                 /*initial_focus=*/false,
		                                 /*close_on_click=*/true)))
	}));

	value content = Margin(Padding(20, 20, 20, 20), VTile({
		AlignCenter(Label("Kirikiri Z UserConfig", 1.4)),
		TopMargin(15, std::move(scroll_area)),
		TopMargin(15, std::move(footer))
	}));

	object root{
		{"size",       Arr({Num(1200), Num(720)})},
		{"background", BgRGBA(35, 35, 37, 255)},
		{"input",      InputBlock()},
		{"content",    std::move(content)}
	};
	return value(root).serialize();
}

//---------------------------------------------------------------------------
// オプションが読めなかったときの fallback JSON
//---------------------------------------------------------------------------
std::string BuildFallbackJson()
{
	value content = Margin(Padding(30, 30, 30, 30), VTile({
		AlignCenter(Label("Kirikiri Z UserConfig")),
		TopMargin(15, AlignCenter(Label("optiondesc.json not found - dummy mode"))),
		TopMargin(30, AlignRight(HTile({
			HSize(120, Button("save", "Save", /*initial_focus=*/true)),
			LeftMargin(20, HSize(120, Button("cancel", "Cancel")))
		})))
	}));

	object root{
		{"size",       Arr({Num(600), Num(300)})},
		{"background", BgRGBA(35, 35, 37, 255)},
		{"input",      InputBlock()},
		{"content",    std::move(content)}
	};
	return value(root).serialize();
}

//---------------------------------------------------------------------------
// run_modal の結果から .cfu 書き出し用の entries を構築
//---------------------------------------------------------------------------
std::vector<std::pair<tjs_string, tjs_string>> CollectChangedEntries(
	const elements_modal::result& result,
	const std::map<std::string, OptionRef>& id_map)
{
	std::vector<std::pair<tjs_string, tjs_string>> entries;
	for (const auto& kv : result.values) {
		auto it = id_map.find(kv.first);
		if (it == id_map.end()) continue;
		const auto* opt = it->second.opt;
		if (!opt || opt->Type != tTVPCommandOption::VT_Select) continue;

		// payload は選択された Description (utf-8)。 Values 内で一致するものを探す
		if (!std::holds_alternative<std::string>(kv.second)) continue;
		const std::string& selected = std::get<std::string>(kv.second);

		for (size_t i = 0; i < opt->Values.size(); ++i) {
			std::string desc_utf8 = ToUtf8(opt->Values[i].Description);
			if (selected != desc_utf8) continue;
			if (i == it->second.default_index) break;   // default のまま → 出力しない
			entries.emplace_back(opt->Name, opt->Values[i].Value);
			break;
		}
	}
	return entries;
}

//---------------------------------------------------------------------------
// run_modal でメイン UI を表示
//---------------------------------------------------------------------------
bool RunUserConfigUi()
{
	// ランタイム (ThorVG + Storages loader + フォント) を ElementsDialogManager
	// 経由で初期化。 内部で TVPInstallElementsResourceLoader + elements_modal::init
	// + 既定 dir からの font register が走る (idempotent)。
	tTVPElementsDialogManager::Instance().EnsureRuntimeInitialized();

	// オプション取得 → JSON 構築
	std::unique_ptr<tTVPCommandOptionList> options(TVPGetEngineCommandDesc());
	std::map<std::string, OptionRef> id_map;
	std::string json;
	if (options && !options->Categories.empty()) {
		json = BuildOptionsJson(*options, id_map);
	} else {
		json = BuildFallbackJson();
	}

	// モーダル設定。 pixel_scale=2.0 で view extent = window/2 logical。
	// 旧 UserConfig は 1200x720 (= view 600x360 logical) だが、 オプション数が
	// 多く scroller が 500 logical 必要なので、 縦を 1400 pixel (700 logical)
	// に広めにとる。 ユーザは resize で更に広げられる。
	elements_modal::config cfg;
	cfg.title_utf8   = "Kirikiri Z UserConfig";
	cfg.width        = 1200;
	cfg.height       = 1400;
	cfg.pixel_scale  = 2.0f;
	cfg.parent       = nullptr;

	elements_modal::result result;
	if (!elements_modal::run_modal(json, cfg, result)) {
		TVPAddImportantLog(TJS_W("ElementsUserConfig: run_modal failed"));
		return false;
	}

	// 結果処理
	if (result.action == "save") {
		auto entries = CollectChangedEntries(result, id_map);
		if (entries.empty()) {
			TVPAddLog(TJS_W("ElementsUserConfig: no changed values; saving header only"));
		}
		WriteCfuFile(entries);
	} else {
		// cancel または Esc/×閉じ
		TVPAddLog(TJS_W("ElementsUserConfig: cancelled (no save)"));
	}
	return true;
}

} // anonymous

//---------------------------------------------------------------------------
// 公開 API
//---------------------------------------------------------------------------
bool TVPExecuteElementsUserConfig()
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-userconf"), &val)) {
		return false;
	}
	TVPAddImportantLog(TJS_W("ElementsUserConfig: -userconf detected, entering UI"));
	bool ok = RunUserConfigUi();
	if (!ok) {
		TVPAddImportantLog(TJS_W("ElementsUserConfig: UI run failed"));
	}
	return true;
}

void TVPSetUserConfigExitFlag(bool b)
{
	s_userconfig_executed_flag = b;
}

bool TVPGetUserConfigExitFlag()
{
	return s_userconfig_executed_flag;
}
