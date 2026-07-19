//---------------------------------------------------------------------------
//!@file wasm (Emscripten) 用 REPL ブリッジ
//
// ブラウザの開発者ツールコンソールを TJS REPL として使うための同期評価
// エントリポイント。devtools のコンソール評価は wasm メインスレッド上で
// フレーム間 (SDL_AppIterate の合間) に実行されるため、他チャネルと違い
// スレッド + ReplMainQueue を介さず直接 TVPExecuteExpression を呼べる。
//
// JS 側 (外枠プロジェクトの pre.js 等) から Module.ccall で呼び出す:
//   Module.ccall('krkrz_repl_eval', 'string', ['string'], ['System.versionString'])
//
// 結果 JSON は ReplFileChannel と同形式:
//   { "ok": bool, "result": "<pretty-printed>", "error": "<msg>" }
//
// 返却ポインタは次回呼び出しまで有効な内部バッファを指す。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#ifdef __EMSCRIPTEN__

#include "ScriptMgnIntf.h"   // TVPExecuteExpression
#include "DebugIntf.h"       // TVPPrettyPrint
#include "CharacterSet.h"    // TVPUtf8ToUtf16 / TVPUtf16ToUtf8

#include <emscripten.h>
#include <string>

namespace {

// JSON 文字列エスケープ (utf-8 のまま、制御文字と "/\ をエスケープ)。
// ReplFileChannel.cpp と同じもの (あちらは無名 namespace 内のため共有不可)。
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

std::string TtstrToUtf8Std(const ttstr& s)
{
	std::string out;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(out, ts);
	return out;
}

} // anonymous

extern "C" EMSCRIPTEN_KEEPALIVE
const char* krkrz_repl_eval(const char* utf8line)
{
	static std::string response;

	tjs_string script_u16;
	std::string script_utf8(utf8line ? utf8line : "");
	TVPUtf8ToUtf16(script_u16, script_utf8);

	tTJSVariant result;
	ttstr error;
	bool ok = false;
	try {
		TVPExecuteExpression(ttstr(script_u16.c_str()), &result);
		ok = true;
	} catch (eTJSScriptError& e) {
		error = ttstr(TJS_W("Error: ")) + e.GetMessage();
	} catch (eTJS& e) {
		error = ttstr(TJS_W("Error: ")) + e.GetMessage();
	} catch (...) {
		error = ttstr(TJS_W("Unknown error occurred"));
	}

	std::string result_utf8, error_utf8;
	if (ok) {
		ttstr pp = TVPPrettyPrint(result, 4, false);
		result_utf8 = TtstrToUtf8Std(pp);
	} else {
		error_utf8 = TtstrToUtf8Std(error);
	}

	response = "{\"ok\":";
	response += ok ? "true" : "false";
	response += ",\"result\":\"" + JsonEscape(result_utf8) + "\"";
	response += ",\"error\":\"" + JsonEscape(error_utf8) + "\"}";
	return response.c_str();
}

#endif // __EMSCRIPTEN__
