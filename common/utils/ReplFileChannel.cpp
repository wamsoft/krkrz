//---------------------------------------------------------------------------
// REPL ファイル監視コマンドチャネル実装 (エージェント駆動用)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ReplFileChannel.h"
#include "ReplMainQueue.h"
#include "REPL.h"             // tTVPReplThread::ProcessLine / LineSink (ドットコマンド)
#include "ReplModal.h"        // TVPSetReplModalChannelDir
#include "SysInitIntf.h"      // TVPGetCommandLine
#include "DebugIntf.h"        // TVPPrettyPrint, TVPAddLog
#include "CharacterSet.h"     // TVPUtf16ToUtf8 / TVPUtf8ToUtf16

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

//---------------------------------------------------------------------------
namespace {

// ttstr (utf16) → std::filesystem::path (プラットフォーム適合)。
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

// JSON 文字列エスケープ (utf-8 のまま、 制御文字と "/\ をエスケープ)。
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

// cmd ファイルの中身を 1 行として扱えるよう前後の空白・改行を落とす
// (エディタや Write が付ける末尾改行でドットコマンド判定を外さないため)。
std::string TrimLine(const std::string& s)
{
	size_t b = 0, e = s.size();
	auto isws = [](char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	};
	while (b < e && isws(s[b])) ++b;
	while (e > b && isws(s[e - 1])) --e;
	return s.substr(b, e - b);
}

std::string TtstrToUtf8Std(const ttstr& s)
{
	std::string out;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(out, ts);
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

} // anonymous

//---------------------------------------------------------------------------
std::string tTVPReplFileChannel::GetDirFromCommandLine()
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-replfile"), &val)) return std::string();
	ttstr s(val);
	if (s == TJS_W("no") || s == TJS_W("off") ||
	    s == TJS_W("false") || s == TJS_W("0") || s.IsEmpty())
		return std::string();
	return TtstrToUtf8Std(s);
}

bool tTVPReplFileChannel::ShouldStart()
{
	return !GetDirFromCommandLine().empty();
}

tTVPReplFileChannel::tTVPReplFileChannel()
	: tTVPThread("ReplFileChannel")
{
	dir_utf8_ = GetDirFromCommandLine();
	StartThread();
}

tTVPReplFileChannel::~tTVPReplFileChannel()
{
	Terminate();
	WaitFor();
}

//---------------------------------------------------------------------------
void tTVPReplFileChannel::Execute()
{
	ttstr dir_tt;
	{
		tjs_string ts;
		TVPUtf8ToUtf16(ts, dir_utf8_);
		dir_tt = ttstr(ts.c_str());
	}
	fs::path dir = TtstrToPath(dir_tt);

	std::error_code ec;
	fs::create_directories(dir, ec);

	const fs::path cmd      = dir / "cmd";
	const fs::path resp     = dir / "resp";
	const fs::path resp_tmp = dir / "resp.tmp";

	TVPAddImportantLog(ttstr(TJS_W("ReplFileChannel: watching ")) + dir_tt);

	// モーダル応答チャネル (confirm / ファイル選択 等) の監視 dir を登録。
	TVPSetReplModalChannelDir(dir_tt);

	while (!GetTerminated()) {
		// 未読の resp が残っている間は次コマンドを処理しない (lockstep)。
		bool has_cmd  = fs::exists(cmd, ec);
		bool has_resp = fs::exists(resp, ec);

		if (has_cmd && !has_resp) {
			std::string script_utf8;
			if (ReadWholeFile(cmd, script_utf8)) {
				fs::remove(cmd, ec);

				bool ok = true;
				std::string result_utf8, error_utf8;

				// 先頭が '.' ならドットコマンド。 console / web と同じ
				// ProcessLine へ通し、 出力行を改行で連結して result に返す
				// (.watch / .mem / .cap 等をエージェントからも叩けるように)。
				// 通常の TJS はこれまでどおり共有キューへ直接出す — 応答の
				// result は「評価結果 1 個の pretty print」という契約なので、
				// 全部を ProcessLine 経由にはしない。
				std::string line = TrimLine(script_utf8);
				if (!line.empty() && line[0] == '.') {
					std::string out, err;
					tTVPReplThread::LineSink sink;
					sink.emit = [&out, &err](int level, const std::string& s) {
						std::string& dst =
							(level == tTVPReplThread::LL_ERROR) ? err : out;
						if (!dst.empty()) dst += "\n";
						dst += s;
					};
					std::string multiline;   // ドットコマンドは継続入力を使わない
					tTVPReplThread::ProcessLine(line, multiline, sink);
					if (GetTerminated()) break;
					result_utf8 = out;
					error_utf8 = err;
					ok = err.empty();
				} else {
					// utf-8 → utf-16 して共有キューでメイン実行。
					tjs_string script_u16;
					TVPUtf8ToUtf16(script_u16, script_utf8);

					tTJSVariant result;
					ttstr error;
					ok = TVPReplMainQueue::Submit(ttstr(script_u16.c_str()), result, error);

					if (GetTerminated()) break;

					if (ok) {
						ttstr pp = TVPPrettyPrint(result, 4, false);
						result_utf8 = TtstrToUtf8Std(pp);
					} else {
						error_utf8 = TtstrToUtf8Std(error);
					}
				}
				std::string json = "{\"ok\":";
				json += ok ? "true" : "false";
				json += ",\"result\":\"" + JsonEscape(result_utf8) + "\"";
				json += ",\"error\":\"" + JsonEscape(error_utf8) + "\"}";

				// resp.tmp に書いて resp に rename (原子的に見せる)。
				WriteWholeFile(resp_tmp, json);
				fs::remove(resp, ec);
				fs::rename(resp_tmp, resp, ec);
				if (ec) {
					// rename 失敗時は直接書き込みにフォールバック。
					WriteWholeFile(resp, json);
					ec.clear();
				}
			} else {
				fs::remove(cmd, ec);
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	// モーダル応答チャネルを解除 (待機中の要求があれば中断され既定へ戻る)。
	TVPSetReplModalChannelDir(ttstr());

	TVPAddImportantLog(TJS_W("ReplFileChannel: stopped"));
}
