//---------------------------------------------------------------------------
// REPL ソケットコマンドチャネル実装 (Android / Linux、adb 駆動用)
//
// tTVPReplFileChannel の socket 版。abstract namespace の Unix domain socket で
// 1 行 = 1 コマンド、応答は 1 行 JSON。ReplMainQueue でメインスレッド実行する。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ReplSocketChannel.h"
#include "ReplMainQueue.h"
#include "SysInitIntf.h"      // TVPGetCommandLine
#include "DebugIntf.h"        // TVPPrettyPrint, TVPAddImportantLog
#include "CharacterSet.h"     // TVPUtf16ToUtf8 / TVPUtf8ToUtf16

#include <string>
#include <cstdlib>            // getenv (Linux/Android の環境変数フォールバック用)

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <cstddef>           // offsetof
#endif

//---------------------------------------------------------------------------
namespace {

std::string TtstrToUtf8Std(const ttstr& s)
{
	std::string out;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(out, ts);
	return out;
}

ttstr Utf8ToTtstr(const std::string& s)
{
	tjs_string ts;
	TVPUtf8ToUtf16(ts, s);
	return ttstr(ts.c_str());
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

// { "ok":bool, "result":"...", "error":"..." } を組み立てる。
std::string MakeJson(bool ok, const std::string& result_utf8, const std::string& error_utf8)
{
	std::string json = "{\"ok\":";
	json += ok ? "true" : "false";
	json += ",\"result\":\"" + JsonEscape(result_utf8) + "\"";
	json += ",\"error\":\"" + JsonEscape(error_utf8) + "\"}";
	return json;
}

// TJS スクリプト (UTF-8, 改行可) を共有キューでメイン実行し、結果 JSON を返す。
std::string EvalToJson(const std::string& script_utf8, int depth, bool compact)
{
	tjs_string script_u16;
	TVPUtf8ToUtf16(script_u16, script_utf8);

	tTJSVariant result;
	ttstr error;
	bool ok = TVPReplMainQueue::Submit(ttstr(script_u16.c_str()), result, error);

	if (ok) {
		return MakeJson(true, TtstrToUtf8Std(TVPPrettyPrint(result, depth, compact)), "");
	}
	return MakeJson(false, "", TtstrToUtf8Std(error));
}

// 本家 console REPL に合わせた dot コマンドのヘルプ (socket で対応する範囲)。
// 改行は本物の '\n' (1 文字)。JsonEscape が JSON の "\n" に変換し、クライアントの
// json.loads で本物の改行へ戻る。'\\' は表示上のバックスラッシュ 1 文字。
const char* kSocketHelp =
	".help              このヘルプ\n"
	".depth [N]         結果表示の展開深さを表示/設定\n"
	".compact [on|off]  結果表示のコンパクト表示を表示/切替\n"
	"exit / quit        (クライアント側で解釈: 接続を閉じる)\n"
	"行末 \\             継続行 (改行を保ったまま複数行を 1 コマンドとして送る)";

} // anonymous

//---------------------------------------------------------------------------
std::string tTVPReplSocketChannel::GetNameFromConfig()
{
	// 1. -replsocket=<name> (CLI がある環境)
	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-replsocket"), &val)) {
		ttstr s(val);
		if (!(s == TJS_W("no") || s == TJS_W("off") ||
		      s == TJS_W("false") || s == TJS_W("0") || s.IsEmpty()))
			return TtstrToUtf8Std(s);
	}
	// 2. 環境変数 KRKRZ_REPL_SOCKET (Android は CLI 引数が無いのでこちら)。
	// この経路は abstract unix socket 同様 Linux/Android 専用。一部プラットフォームは
	// getenv 自体を持たない (std/global どちらにも無い) ため、非 Linux では
	// 環境変数フォールバックをコンパイルしない。ShouldStart() も非 Linux では
	// 常に false を返すので機能欠落は無い。
#if defined(__linux__)
	const char* env = getenv("KRKRZ_REPL_SOCKET");
	if (env && env[0]) {
		std::string s(env);
		if (s != "no" && s != "off" && s != "false" && s != "0")
			return s;
	}
#endif
	return std::string();
}

bool tTVPReplSocketChannel::ShouldStart()
{
#if defined(__linux__)
	return !GetNameFromConfig().empty();
#else
	return false;   // abstract unix socket は Linux (Android 含む) 専用
#endif
}

tTVPReplSocketChannel::tTVPReplSocketChannel()
	: tTVPThread("ReplSocketChannel"), listen_fd_(-1), pp_depth_(4), pp_compact_(false)
{
	name_ = GetNameFromConfig();
	StartThread();
}

//---------------------------------------------------------------------------
// 1 コマンドを処理: 先頭 '.' の dot コマンド (allowDot 時) は channel 側で解釈し、
// それ以外は TJS として ReplMainQueue で実行する。本家 console REPL の
// .help / .depth / .compact に相当。応答はいずれも {ok,result,error} JSON。
std::string tTVPReplSocketChannel::ProcessCommand(const std::string& script, bool allowDot)
{
	if (allowDot && !script.empty() && script[0] == '.') {
		// 空白でトークン分割
		std::string cmd, arg;
		{
			size_t sp = script.find_first_of(" \t");
			if (sp == std::string::npos) { cmd = script; }
			else {
				cmd = script.substr(0, sp);
				size_t a = script.find_first_not_of(" \t", sp);
				if (a != std::string::npos) arg = script.substr(a);
			}
		}
		if (cmd == ".help") {
			return MakeJson(true, kSocketHelp, "");
		}
		if (cmd == ".depth") {
			if (!arg.empty()) {
				int n = std::atoi(arg.c_str());
				if (n >= 0) pp_depth_ = n;
			}
			return MakeJson(true, std::string("depth = ") + std::to_string(pp_depth_), "");
		}
		if (cmd == ".compact") {
			if (arg == "on" || arg == "true" || arg == "1")       pp_compact_ = true;
			else if (arg == "off" || arg == "false" || arg == "0") pp_compact_ = false;
			else if (arg.empty())                                  pp_compact_ = !pp_compact_;
			return MakeJson(true, std::string("compact = ") + (pp_compact_ ? "on" : "off"), "");
		}
		return MakeJson(false, "", std::string("unknown command: ") + cmd);
	}
	return EvalToJson(script, pp_depth_, pp_compact_);
}

tTVPReplSocketChannel::~tTVPReplSocketChannel()
{
	Terminate();
#if defined(__linux__)
	// accept/recv の poll timeout (200ms) で GetTerminated を拾うので通常はそれで
	// 抜けるが、listen fd を落として即時に起こす。
	if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
#endif
	WaitFor();
}

//---------------------------------------------------------------------------
#if defined(__linux__)

namespace {

bool WriteAll(int fd, const std::string& data)
{
	size_t off = 0;
	while (off < data.size()) {
		ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
		if (n <= 0) return false;
		off += (size_t)n;
	}
	return true;
}

} // anonymous

void tTVPReplSocketChannel::Execute()
{
	listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd_ < 0) {
		TVPAddImportantLog(TJS_W("ReplSocketChannel: socket() failed"));
		return;
	}

	struct sockaddr_un addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	// abstract namespace: sun_path[0] = '\0' の後ろに名前。
	size_t nlen = name_.size();
	if (nlen > sizeof(addr.sun_path) - 1) nlen = sizeof(addr.sun_path) - 1;
	addr.sun_path[0] = '\0';
	std::memcpy(addr.sun_path + 1, name_.data(), nlen);
	socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nlen);

	if (::bind(listen_fd_, (struct sockaddr*)&addr, alen) < 0) {
		TVPAddImportantLog(ttstr(TJS_W("ReplSocketChannel: bind failed @")) + Utf8ToTtstr(name_));
		::close(listen_fd_); listen_fd_ = -1;
		return;
	}
	if (::listen(listen_fd_, 1) < 0) {
		TVPAddImportantLog(TJS_W("ReplSocketChannel: listen failed"));
		::close(listen_fd_); listen_fd_ = -1;
		return;
	}

	TVPAddImportantLog(ttstr(TJS_W("ReplSocketChannel: listening on @")) + Utf8ToTtstr(name_));

	while (!GetTerminated()) {
		struct pollfd pfd; pfd.fd = listen_fd_; pfd.events = POLLIN; pfd.revents = 0;
		int pr = ::poll(&pfd, 1, 200);
		if (pr <= 0) continue;                       // timeout / EINTR → GetTerminated 再確認
		if (pfd.revents & (POLLHUP | POLLERR)) break;

		int cfd = ::accept(listen_fd_, nullptr, nullptr);
		if (cfd < 0) continue;

		// --- 1 クライアントを行単位で応対 ---
		std::string buf;
		std::string accum;   // 継続行 (行末 '\') の蓄積
		char tmp[4096];
		bool alive = true;
		while (alive && !GetTerminated()) {
			// buf 内の完成行を処理
			size_t nl;
			while ((nl = buf.find('\n')) != std::string::npos) {
				std::string line = buf.substr(0, nl);
				buf.erase(0, nl + 1);
				if (!line.empty() && line.back() == '\r') line.pop_back();

				// 行末 '\' は継続 (改行を保ったまま accum に積む → 複数行 1 コマンド)。
				bool cont = (!line.empty() && line.back() == '\\');
				if (cont) line.pop_back();
				accum += line;
				if (cont) { accum += '\n'; continue; }

				std::string script = accum;
				accum.clear();
				if (script.empty()) continue;

				// 単一行のときだけ dot コマンド (.help/.depth/.compact) を許可。
				bool allowDot = (script.find('\n') == std::string::npos);
				std::string resp = ProcessCommand(script, allowDot);
				if (GetTerminated()) { alive = false; break; }
				resp += '\n';
				if (!WriteAll(cfd, resp)) { alive = false; break; }
			}
			if (!alive) break;

			struct pollfd cp; cp.fd = cfd; cp.events = POLLIN; cp.revents = 0;
			int cr = ::poll(&cp, 1, 200);
			if (cr < 0) break;
			if (cr == 0) continue;                   // timeout
			if (cp.revents & (POLLHUP | POLLERR)) break;
			ssize_t r = ::recv(cfd, tmp, sizeof(tmp), 0);
			if (r <= 0) break;                       // 切断
			buf.append(tmp, (size_t)r);
		}
		::close(cfd);
	}

	::close(listen_fd_); listen_fd_ = -1;
	TVPAddImportantLog(TJS_W("ReplSocketChannel: stopped"));
}

#else  // !__linux__

void tTVPReplSocketChannel::Execute() { /* Linux 以外では起動しない */ }

#endif
