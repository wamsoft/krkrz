//---------------------------------------------------------------------------
// REPL モーダル応答チャネル 実装
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ReplModal.h"
#include "SysInitIntf.h"     // TVPReplActive / TVPGetCommandLineInt
#include "CharacterSet.h"    // TVPUtf16ToUtf8 / TVPUtf8ToUtf16
#include "MsgIntf.h"         // TVPThrowExceptionMessage

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

namespace fs = std::filesystem;

//---------------------------------------------------------------------------
namespace {

std::mutex g_dir_mtx;
ttstr      g_dir; //!< ReplFileChannel の監視ディレクトリ (空 = 無効)

ttstr GetDir()
{
	std::lock_guard<std::mutex> lk(g_dir_mtx);
	return g_dir;
}

// ttstr (utf16) → std::filesystem::path
fs::path TtstrToPath(const ttstr& s)
{
#ifdef _WIN32
	return fs::path(reinterpret_cast<const wchar_t*>(s.c_str()));
#else
	std::string utf8;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(utf8, ts);
	return fs::path(utf8);
#endif
}

std::string TtstrToUtf8Std(const ttstr& s)
{
	std::string out;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(out, ts);
	return out;
}

// JSON 文字列エスケープ (utf-8 のまま)
std::string JsonEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 16);
	for (unsigned char c : s) {
		switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20) {
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out += (char)c;
				}
		}
	}
	return out;
}

bool ReadWholeFile(const fs::path& p, std::string& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign((std::istreambuf_iterator<char>(f)),
	           std::istreambuf_iterator<char>());
	return true;
}

bool WriteWholeFile(const fs::path& p, const std::string& data)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	if (!f) return false;
	f.write(data.data(), (std::streamsize)data.size());
	return (bool)f;
}

// modal 応答待ちのタイムアウト(ms)。-replmodaltimeout=<秒> で調整、0=無限(=旧挙動)、
// 既定 30 秒。応答者(エージェント)が modalresp を書かないケースでの永久ブロックを防ぐ。
long GetModalTimeoutMs()
{
	static long cached = -1; // -1 = 未取得
	if (cached < 0) {
		int sec = TVPGetCommandLineInt(TJS_W("-replmodaltimeout"), 30);
		if (sec < 0) sec = 0;
		cached = (long)sec * 1000;
	}
	return cached;
}

// 末尾の改行/空白を除去
ttstr TrimTrailing(const ttstr &s)
{
	tjs_string t(s.c_str());
	while (!t.empty()) {
		tjs_char c = t.back();
		if (c == TJS_W('\n') || c == TJS_W('\r') || c == TJS_W(' ') || c == TJS_W('\t'))
			t.pop_back();
		else
			break;
	}
	return ttstr(t.c_str());
}

} // anonymous

//---------------------------------------------------------------------------
void TVPSetReplModalChannelDir(const ttstr &dir)
{
	std::lock_guard<std::mutex> lk(g_dir_mtx);
	g_dir = dir;
}
//---------------------------------------------------------------------------
bool TVPReplModalActive()
{
	if (!TVPReplActive) return false;
	return !GetDir().IsEmpty();
}
//---------------------------------------------------------------------------
bool TVPReplRequestModal(const ttstr &requestJson, ttstr &responseOut)
{
	if (!TVPReplModalActive()) return false;

	ttstr dir_tt = GetDir();
	if (dir_tt.IsEmpty()) return false;

	fs::path dir       = TtstrToPath(dir_tt);
	fs::path modal     = dir / "modal";
	fs::path modal_tmp = dir / "modal.tmp";
	fs::path modalresp = dir / "modalresp";

	std::error_code ec;
	// 古い要求/応答を掃除
	fs::remove(modal, ec);
	fs::remove(modalresp, ec);

	// 要求を書き込む (tmp→rename で原子的に見せる)
	std::string json = TtstrToUtf8Std(requestJson);
	WriteWholeFile(modal_tmp, json);
	fs::remove(modal, ec);
	if (!fs::exists(modal_tmp, ec) || !(fs::rename(modal_tmp, modal, ec), !ec)) {
		WriteWholeFile(modal, json); // フォールバック
		fs::remove(modal_tmp, ec);
	}

	// 応答を待つ (メインスレッドはここでブロック。応答は外部エージェントが書く)
	// 応答者がいないケースの永久ブロックを防ぐため timeout_ms で打ち切り、
	// catch 可能な例外を投げる (0=無限で旧挙動)。
	const long timeout_ms = GetModalTimeoutMs();
	const auto  start      = std::chrono::steady_clock::now();
	for (;;) {
		// チャネルが停止 (dir クリア) されたら中断して既定へ
		if (GetDir().IsEmpty()) {
			fs::remove(modal, ec);
			return false;
		}
		if (fs::exists(modalresp, ec)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 書込完了待ち
			std::string resp_u8;
			ReadWholeFile(modalresp, resp_u8);
			fs::remove(modalresp, ec);
			fs::remove(modal, ec);
			tjs_string resp16;
			TVPUtf8ToUtf16(resp16, resp_u8);
			responseOut = TrimTrailing(ttstr(resp16.c_str()));
			return true;
		}
		if (timeout_ms > 0) {
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			if (elapsed >= timeout_ms) {
				fs::remove(modal, ec);
				// REPL 中に modal 応答(modalresp)が時間内に来なかった。
				// 呼び出し元(inputString/confirm/select)へ TJS 例外として伝播する。
				TVPThrowExceptionMessage(TVPREPLModalTimeoutNoModalrespWasWrittenWithinTheTimeoutRepl);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}
//---------------------------------------------------------------------------
bool TVPReplConfirm(const ttstr &text, const ttstr &caption, bool &answer)
{
	if (!TVPReplModalActive()) return false;

	std::string req = "{\"type\":\"confirm\",\"caption\":\"" +
		JsonEscape(TtstrToUtf8Std(caption)) + "\",\"text\":\"" +
		JsonEscape(TtstrToUtf8Std(text)) + "\"}";

	tjs_string req16;
	TVPUtf8ToUtf16(req16, req);
	ttstr resp;
	if (!TVPReplRequestModal(ttstr(req16.c_str()), resp)) return false;

	// 応答文字列を yes/no 判定
	tjs_string r(resp.c_str());
	for (auto &c : r) if (c >= TJS_W('A') && c <= TJS_W('Z')) c = c - TJS_W('A') + TJS_W('a');
	answer = (r == TJS_W("yes") || r == TJS_W("y") || r == TJS_W("1") ||
	          r == TJS_W("true") || r == TJS_W("ok"));
	return true;
}
//---------------------------------------------------------------------------
bool TVPReplSelectPath(bool isDir, const ttstr &name, const ttstr &title,
	bool save, ttstr &pathOut, bool &selected)
{
	if (!TVPReplModalActive()) return false;

	std::string req = std::string("{\"type\":\"") +
		(isDir ? "selectDirectory" : "selectFile") +
		"\",\"name\":\"" + JsonEscape(TtstrToUtf8Std(name)) +
		"\",\"title\":\"" + JsonEscape(TtstrToUtf8Std(title)) +
		"\",\"save\":" + (save ? "true" : "false") + "}";

	tjs_string req16;
	TVPUtf8ToUtf16(req16, req);
	ttstr resp;
	if (!TVPReplRequestModal(ttstr(req16.c_str()), resp)) return false;

	pathOut  = resp;
	selected = !resp.IsEmpty(); // 空応答 = キャンセル
	return true;
}
//---------------------------------------------------------------------------
int TVPReplTrySelect(iTJSDispatch2 *params, bool isDir)
{
	if (!TVPReplModalActive()) return -1;

	ttstr name, title;
	bool save = false;
	tTJSVariant v;
	if (params) {
		if (TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("name"), 0, &v, params))
			&& v.Type() == tvtString) name = v;
		if (TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("title"), 0, &v, params))
			&& v.Type() != tvtVoid) title = v;
		if (TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("save"), 0, &v, params)))
			save = v.operator bool();
	}

	ttstr path;
	bool selected = false;
	if (!TVPReplSelectPath(isDir, name, title, save, path, selected)) return -1;

	if (selected && params) {
		tTJSVariant nv(path);
		params->PropSet(TJS_MEMBERENSURE, TJS_W("name"), 0, &nv, params);
	}
	return selected ? 1 : 0;
}
//---------------------------------------------------------------------------
bool TVPReplInputString(const ttstr &caption, const ttstr &prompt, const ttstr &def,
	ttstr &result, bool &cancelled)
{
	// 応答は 1 行目が "ok"/"cancel"、以降が入力値。
	if (!TVPReplModalActive()) return false;

	std::string req = "{\"type\":\"inputString\",\"caption\":\"" +
		JsonEscape(TtstrToUtf8Std(caption)) + "\",\"prompt\":\"" +
		JsonEscape(TtstrToUtf8Std(prompt)) + "\",\"default\":\"" +
		JsonEscape(TtstrToUtf8Std(def)) + "\"}";

	tjs_string req16;
	TVPUtf8ToUtf16(req16, req);
	ttstr resp;
	if (!TVPReplRequestModal(ttstr(req16.c_str()), resp)) return false;

	// "cancel" 単独 = キャンセル。"ok\n<値>" = 入力確定。
	tjs_string r(resp.c_str());
	if (r == TJS_W("cancel")) { cancelled = true; result = ttstr(); return true; }
	tjs_string::size_type nl = r.find(TJS_W('\n'));
	if (r.compare(0, 3, TJS_W("ok\n")) == 0 || r == TJS_W("ok")) {
		cancelled = false;
		result = (nl != tjs_string::npos) ? ttstr(r.substr(nl + 1).c_str()) : ttstr();
	} else {
		// 後方互換: 全体を入力値として扱う
		cancelled = false;
		result = resp;
	}
	return true;
}
//---------------------------------------------------------------------------
