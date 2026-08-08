//---------------------------------------------------------------------------
// ブラウザ REPL ビューワー (HTTP + SSE サーバ) 実装
//---------------------------------------------------------------------------
// winsock2.h は windows.h (tjsCommHead 経由) より前に include する必要がある。
#if defined(_WIN32) && defined(KRKRZ_REPL_WEB)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

#include "tjsCommHead.h"
#ifdef KRKRZ_REPL_WEB

#include "ReplWebServer.h"
#include "REPL.h"            // tTVPReplThread::ProcessLine / LineSink / LineLevel
#include "ReplMainQueue.h"   // TVPReplMainQueue::SubmitTask (ハンドラのメインスレッド実行)
#include "SysInitIntf.h"     // TVPGetCommandLine
#include "LogIntf.h"         // TVPLogSetConsoleSink / TVPLogLevel
#include "DebugIntf.h"       // TVPAddImportantLog
#include "CharacterSet.h"    // TVPUtf16ToUtf8 / TVPUtf8ToUtf16
#include "StorageIntf.h"     // TVPCreateStream (静的配信のストレージ読込)
#include "tjsDictionary.h"   // TJSCreateDictionaryObject (ハンドラ req 辞書)

// URL を OS 既定ブラウザで開く (Windows=ShellExecute / SDL=SDL_OpenURL)。
// 宣言はプラットフォーム別 SystemImpl.h にあるが、ここでは前方宣言で参照する。
extern bool TVPShellExecute(const ttstr &target, const ttstr &param);

#include <string>
#include <deque>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cctype>
#include <algorithm>

#ifdef _WIN32
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define closesock ::closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define closesock ::close
#endif

namespace TVPReplWeb {

namespace {

//---------------------------------------------------------------------------
// SSE クライアント 1 本ぶんの送信キュー
struct Client {
	std::mutex               mu;
	std::condition_variable  cv;
	std::deque<std::string>  queue;   // 送信待ちの SSE フレーム ("data: ...\n\n" 済み)
	std::string              channel; // 購読チャネル ("log" = 既定の /events)
	bool                     closed = false;
};

std::mutex                             g_clients_mu;
std::vector<std::shared_ptr<Client>>   g_clients;

//---------------------------------------------------------------------------
// 登録ハンドラ / 静的マウントのレジストリ。
// プレフィックス表 (std::string) は HTTP スレッドがマッチングのため読むので
// g_routes_mu で保護する。TJS クロージャ (tTJSVariant) は参照カウントが
// スレッドセーフでないため g_handler_closures はメインスレッド専用 —
// HTTP スレッドは prefix 文字列だけを持ち回り、実体の解決・呼び出しは
// SubmitTask でメインスレッドに運んでから行う。
std::mutex                                        g_routes_mu;
std::vector<std::string>                          g_handler_prefixes; // utf8
std::vector<std::pair<std::string, std::string>>  g_static_mounts;    // {prefix, storageDir} utf8
std::map<std::string, tTJSVariant>                g_handler_closures; // main thread only

std::mutex                g_ring_mu;
std::deque<std::string>   g_ring;            // 直近ログ (JSON) のバックログ
constexpr size_t          kRingMax = 2000;

std::atomic<bool>         g_running{false};
int                       g_port = 0;
std::string               g_host;            // 実バインドした host (bind 用)
std::string               g_url_host;        // URL 表示用 host (0.0.0.0 は外向き実IPに解決)
sock_t                    g_listen = SOCK_INVALID;
std::thread               g_accept_thread;
std::string               g_multiline;       // 継続入力の蓄積 (単一セッション想定)

//---------------------------------------------------------------------------
// 主要な外向きインタフェースの IPv4 アドレスを文字列で返す。
//
// UDP ソケットをダミー宛先へ connect() すると (実際にはパケットは飛ばない)、
// 経路選択で使われるローカルアドレスが getsockname() で得られる。getifaddrs が
// 無い、端末を持たない一部プラットフォームでも動く手法。inet_ntop 非依存で
// 4 バイトを手動整形。
// 取得できない / loopback しか無い場合は空文字列を返す。
//---------------------------------------------------------------------------
std::string ResolveOutwardIPv4()
{
	std::string result;
	sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (s == SOCK_INVALID) return result;

	sockaddr_in peer;
	memset(&peer, 0, sizeof(peer));
	peer.sin_family      = AF_INET;
	peer.sin_port        = htons(53);
	peer.sin_addr.s_addr = htonl(0x08080808u); // 8.8.8.8 (到達性不要、経路選択のみ)

	if (::connect(s, (sockaddr*)&peer, sizeof(peer)) == 0) {
		sockaddr_in local;
		memset(&local, 0, sizeof(local));
#ifdef _WIN32
		int len = sizeof(local);
#else
		socklen_t len = sizeof(local);
#endif
		if (::getsockname(s, (sockaddr*)&local, &len) == 0) {
			unsigned long a = ntohl(local.sin_addr.s_addr);
			char buf[32];
			snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
				(a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
			result = buf;
		}
	}
	closesock(s);

	// 0.0.0.0 / loopback は接続先として無意味なので無効扱い
	if (result == "0.0.0.0" || result.rfind("127.", 0) == 0) result.clear();
	return result;
}

//---------------------------------------------------------------------------
// -replweb の値を解釈する。書式:
//   -replweb                 → 127.0.0.1:8899
//   -replweb=8080            → 127.0.0.1:8080
//   -replweb=0.0.0.0:8080    → 全インタフェース (外部の開発PCブラウザから接続)
//   -replweb=192.168.1.5:80  → 指定アドレスにバインド
//   -replweb=no/off/false/0  → 無効
// host に "0.0.0.0" / "*" を与えると INADDR_ANY。
// 既定バインド先はプラットフォームで異なる:
//   デスクトップ                        → 127.0.0.1 (ローカル専用、安全側)
//   端末を持たない一部プラットフォーム  → 0.0.0.0   (本体にローカルブラウザが無く、
//                             開発PCのブラウザから LAN 越しに繋ぐため。KRKRZ_REPL_WEB_BIND_ANY)
bool ParseCmd(std::string& host, int& port)
{
#ifdef KRKRZ_REPL_WEB_BIND_ANY
	host = "0.0.0.0";
#else
	host = "127.0.0.1";
#endif
	port = 0;
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-replweb"), &val)) return false;
	ttstr s(val);
	if (s == TJS_W("no") || s == TJS_W("off") || s == TJS_W("false") || s == TJS_W("0"))
		return false;
	if (s.IsEmpty() || s == TJS_W("yes") || s == TJS_W("on") || s == TJS_W("true")) {
		port = 8899;
		return true;
	}
	std::string v;
	{ tjs_string t(s.c_str()); TVPUtf16ToUtf8(v, t); }
	size_t colon = v.rfind(':');
	if (colon != std::string::npos) {
		std::string h = v.substr(0, colon);
		if (!h.empty()) host = h;
		port = atoi(v.c_str() + colon + 1);
	} else {
		port = atoi(v.c_str());
	}
	if (port <= 0 || port >= 65536) port = 8899;
	return true;
}

//---------------------------------------------------------------------------
std::string JsonEscape(const std::string& s)
{
	std::string o;
	o.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
			case '"':  o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n";  break;
			case '\r': o += "\\r";  break;
			case '\t': o += "\\t";  break;
			default:
				if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
				else o += (char)c;
		}
	}
	return o;
}

// ログレベル/疑似レベル → CSS クラス名
const char* ClassForLevel(int level)
{
	switch (level) {
		case TVPLOG_LEVEL_VERBOSE:  return "verbose";
		case TVPLOG_LEVEL_DEBUG:    return "debug";
		case TVPLOG_LEVEL_INFO:     return "info";
		case TVPLOG_LEVEL_WARNING:  return "warn";
		case TVPLOG_LEVEL_ERROR:    return "error";
		case TVPLOG_LEVEL_CRITICAL: return "critical";
		default:                    return "info";
	}
}
const char* ClassForRepl(int lv)
{
	switch (lv) {
		case tTVPReplThread::LL_RESULT: return "result";
		case tTVPReplThread::LL_ERROR:  return "error";
		case tTVPReplThread::LL_HELP:   return "help";
		case tTVPReplThread::LL_ECHO:   return "echo";
		default:                        return "info";
	}
}

//---------------------------------------------------------------------------
// payload (改行含み可) を SSE 1 イベントぶんのフレームへ整形する。
std::string SseFrame(const std::string& payload)
{
	std::string frame;
	size_t start = 0;
	while (true) {
		size_t nl = payload.find('\n', start);
		std::string line = (nl == std::string::npos)
			? payload.substr(start) : payload.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		frame += "data: "; frame += line; frame += "\n";
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	frame += "\n";
	return frame;
}

//---------------------------------------------------------------------------
// channel の購読クライアントへフレームを配信 (thread-safe)
void PushFrameToChannel(const std::string& channel, const std::string& frame)
{
	std::lock_guard<std::mutex> lk(g_clients_mu);
	for (auto& c : g_clients) {
		if (c->channel != channel) continue;
		std::lock_guard<std::mutex> clk(c->mu);
		c->queue.push_back(frame);
		c->cv.notify_one();
	}
}

//---------------------------------------------------------------------------
// ログ 1 行を "log" チャネル (/events) へ配信 + バックログへ格納
void Broadcast(const char* cls, const std::string& text)
{
	std::string json = std::string("{\"cls\":\"") + cls + "\",\"text\":\"" +
		JsonEscape(text) + "\"}";
	{
		std::lock_guard<std::mutex> lk(g_ring_mu);
		g_ring.push_back(json);
		while (g_ring.size() > kRingMax) g_ring.pop_front();
	}
	PushFrameToChannel("log", SseFrame(json));
}

//---------------------------------------------------------------------------
// socket 全量送信
bool SendAll(sock_t s, const char* buf, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		int n = ::send(s, buf + sent, (int)(len - sent), 0);
		if (n <= 0) return false;
		sent += (size_t)n;
	}
	return true;
}
bool SendStr(sock_t s, const std::string& str) { return SendAll(s, str.data(), str.size()); }

//---------------------------------------------------------------------------
// HTTP リクエストのヘッダ部を読み取り、method / path / body を取り出す。
// 戻り: ヘッダを読めたら true。body は Content-Length ぶん読む。
bool ReadRequest(sock_t s, std::string& method, std::string& path, std::string& body)
{
	std::string buf;
	char tmp[2048];
	// ヘッダ終端 (\r\n\r\n) まで読む
	size_t hdr_end = std::string::npos;
	while (true) {
		int n = ::recv(s, tmp, sizeof(tmp), 0);
		if (n <= 0) return false;
		buf.append(tmp, (size_t)n);
		hdr_end = buf.find("\r\n\r\n");
		if (hdr_end != std::string::npos) break;
		if (buf.size() > 65536) return false; // 異常に大きいヘッダは拒否
	}
	// リクエスト行
	size_t line_end = buf.find("\r\n");
	std::string reqline = buf.substr(0, line_end);
	size_t sp1 = reqline.find(' ');
	size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : reqline.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
	method = reqline.substr(0, sp1);
	path   = reqline.substr(sp1 + 1, sp2 - sp1 - 1);

	// Content-Length
	size_t clen = 0;
	{
		std::string head = buf.substr(0, hdr_end);
		for (char& c : head) c = (char)tolower((unsigned char)c);
		size_t p = head.find("content-length:");
		if (p != std::string::npos) clen = (size_t)atoi(head.c_str() + p + 15);
	}
	// body (すでに読めた分 + 残り)
	body = buf.substr(hdr_end + 4);
	while (body.size() < clen) {
		int n = ::recv(s, tmp, sizeof(tmp), 0);
		if (n <= 0) break;
		body.append(tmp, (size_t)n);
	}
	if (body.size() > clen) body.resize(clen);
	return true;
}

//---------------------------------------------------------------------------
extern const char* kHtmlPage; // 末尾に定義

void HandleIndex(sock_t s)
{
	std::string page = kHtmlPage;
	std::string resp =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: " + std::to_string(page.size()) + "\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: close\r\n\r\n";
	SendStr(s, resp);
	SendStr(s, page);
}

// SSE 購読 1 本ぶんの処理。channel="log" (= /events) はログバックログ付き、
// /sub/<channel> はバックログ無し (購読後の broadcast のみ)。
void HandleSse(sock_t s, const std::string& channel, bool withBacklog)
{
	std::string resp =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream; charset=utf-8\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: keep-alive\r\n"
		"X-Accel-Buffering: no\r\n\r\n";
	if (!SendStr(s, resp)) return;

	auto client = std::make_shared<Client>();
	client->channel = channel;
	// バックログを最初に流す (log チャネルのみ)
	if (withBacklog) {
		std::lock_guard<std::mutex> lk(g_ring_mu);
		for (auto& j : g_ring) client->queue.push_back(SseFrame(j));
	}
	{
		std::lock_guard<std::mutex> lk(g_clients_mu);
		g_clients.push_back(client);
	}

	bool alive = true;
	while (alive && g_running.load(std::memory_order_acquire)) {
		std::deque<std::string> batch;
		{
			std::unique_lock<std::mutex> lk(client->mu);
			client->cv.wait_for(lk, std::chrono::seconds(15), [&] {
				return !client->queue.empty() || client->closed ||
				       !g_running.load(std::memory_order_acquire);
			});
			if (client->closed) alive = false;
			batch.swap(client->queue);
		}
		if (batch.empty()) {
			// ハートビート (コメント行) で切断検知
			if (!SendStr(s, ":ping\n\n")) break;
			continue;
		}
		std::string out;
		for (auto& f : batch) out += f;
		if (!SendStr(s, out)) break;
	}
	// 登録解除
	std::lock_guard<std::mutex> lk(g_clients_mu);
	for (size_t i = 0; i < g_clients.size(); ++i) {
		if (g_clients[i] == client) { g_clients.erase(g_clients.begin() + i); break; }
	}
}

void HandleCmd(sock_t s, const std::string& body)
{
	// body = 入力行 (utf8)。末尾改行を除去。
	std::string line = body;
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

	// 入力エコー
	Broadcast(ClassForRepl(tTVPReplThread::LL_ECHO),
		std::string(g_multiline.empty() ? "krkrz> " : "  ...> ") + line);

	// 共有 ProcessLine で評価 (ドットコマンド/複数行/文実行)。出力は SSE へ配信。
	tTVPReplThread::LineSink sink;
	sink.emit = [](int lv, const std::string& t) { Broadcast(ClassForRepl(lv), t); };
	// 履歴はブラウザ側で管理するので addHistory は不要
	tTVPReplThread::ProcessLine(line, g_multiline, sink);

	// 継続入力中かどうかをブラウザへ返す (プロンプト切替用)
	std::string cont = g_multiline.empty() ? "0" : "1";
	std::string resp =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: " + std::to_string(cont.size()) + "\r\n"
		"Connection: close\r\n\r\n" + cont;
	SendStr(s, resp);
}

void Handle404(sock_t s)
{
	std::string resp =
		"HTTP/1.1 404 Not Found\r\n"
		"Content-Length: 0\r\nConnection: close\r\n\r\n";
	SendStr(s, resp);
}

//---------------------------------------------------------------------------
// 登録ハンドラ / 静的マウントへの dispatch
//---------------------------------------------------------------------------

// ハンドラ / 静的配信の応答 (HTTP スレッド ⇄ メインスレッド受け渡し用)
struct WebResp {
	int          status = 500;
	std::string  mime   = "text/plain; charset=utf-8";
	std::string  body;
};

const char* StatusText(int status)
{
	switch (status) {
		case 200: return "OK";
		case 204: return "No Content";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 500: return "Internal Server Error";
		case 503: return "Service Unavailable";
		default:  return "Status";
	}
}

void SendWebResp(sock_t s, const WebResp& r)
{
	std::string resp =
		"HTTP/1.1 " + std::to_string(r.status) + " " + StatusText(r.status) + "\r\n"
		"Content-Type: " + r.mime + "\r\n"
		"Content-Length: " + std::to_string(r.body.size()) + "\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: close\r\n\r\n";
	if (!SendStr(s, resp)) return;
	if (!r.body.empty()) SendStr(s, r.body);
}

// %XX / '+' の URL デコード (path 用。'+' は space にしない — path では literal)
std::string UrlDecode(const std::string& s)
{
	std::string o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size() &&
		    isxdigit((unsigned char)s[i+1]) && isxdigit((unsigned char)s[i+2])) {
			auto hex = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				return (tolower((unsigned char)c) - 'a') + 10;
			};
			o += (char)((hex(s[i+1]) << 4) | hex(s[i+2]));
			i += 2;
		} else {
			o += s[i];
		}
	}
	return o;
}

// 拡張子 → MIME (静的配信用の最小テーブル)
std::string MimeForPath(const std::string& path)
{
	size_t dot = path.rfind('.');
	std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
	for (char& c : ext) c = (char)tolower((unsigned char)c);
	if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
	if (ext == "js"  || ext == "mjs")  return "text/javascript; charset=utf-8";
	if (ext == "css")   return "text/css; charset=utf-8";
	if (ext == "json")  return "application/json; charset=utf-8";
	if (ext == "png")   return "image/png";
	if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
	if (ext == "gif")   return "image/gif";
	if (ext == "svg")   return "image/svg+xml";
	if (ext == "ico")   return "image/x-icon";
	if (ext == "txt")   return "text/plain; charset=utf-8";
	if (ext == "wasm")  return "application/wasm";
	if (ext == "woff2") return "font/woff2";
	if (ext == "glb" || ext == "vrm" || ext == "vrma") return "model/gltf-binary";
	return "application/octet-stream";
}

// ".." セグメント等のパストラバーサルを拒否
bool PathIsUnsafe(const std::string& rel)
{
	if (rel.find('\\') != std::string::npos) return true;
	if (rel.find(':')  != std::string::npos) return true;
	size_t start = 0;
	while (start <= rel.size()) {
		size_t sl = rel.find('/', start);
		std::string seg = (sl == std::string::npos)
			? rel.substr(start) : rel.substr(start, sl - start);
		if (seg == "..") return true;
		if (sl == std::string::npos) break;
		start = sl + 1;
	}
	return false;
}

// ハンドラ戻り値 → WebResp (メインスレッド)
void MarshalResult(const tTJSVariant& result, WebResp& out)
{
	switch (result.Type()) {
	case tvtVoid:
		out.status = 204; out.body.clear();
		return;
	case tvtOctet: {
		out.status = 200; out.mime = "application/octet-stream";
		tTJSVariantOctet* o = result.AsOctetNoAddRef();
		if (o) out.body.assign((const char*)o->GetData(), (size_t)o->GetLength());
		else out.body.clear();
		return; }
	case tvtInteger:
		out.status = (int)(tjs_int)result; out.body.clear();
		return;
	case tvtObject: {
		// %[ status, mime, body(文字列 or octet) ]
		iTJSDispatch2* d = result.AsObjectNoAddRef();
		if (!d) { out.status = 204; out.body.clear(); return; }
		auto get = [&](const tjs_char* k, tTJSVariant& v) -> bool {
			v.Clear();
			return TJS_SUCCEEDED(d->PropGet(0, k, nullptr, &v, d)) &&
			       v.Type() != tvtVoid;
		};
		tTJSVariant v;
		out.status = 200;
		if (get(TJS_W("status"), v)) out.status = (int)(tjs_int)v;
		std::string mime;
		bool mimeSet = false;
		if (get(TJS_W("mime"), v)) {
			ttstr s(v);
			tjs_string w(s.c_str());
			TVPUtf16ToUtf8(mime, w);
			mimeSet = true;
		}
		out.body.clear();
		if (get(TJS_W("body"), v)) {
			if (v.Type() == tvtOctet) {
				tTJSVariantOctet* o = v.AsOctetNoAddRef();
				if (o) out.body.assign((const char*)o->GetData(), (size_t)o->GetLength());
				out.mime = mimeSet ? mime : "application/octet-stream";
			} else {
				ttstr s(v);
				tjs_string w(s.c_str());
				TVPUtf16ToUtf8(out.body, w);
				out.mime = mimeSet ? mime : "application/json; charset=utf-8";
			}
		} else {
			out.mime = mimeSet ? mime : "text/plain; charset=utf-8";
		}
		return; }
	default: {
		// 文字列ほか → utf8 文字列化。既定 MIME は JSON (API 前提の規約)。
		out.status = 200; out.mime = "application/json; charset=utf-8";
		ttstr s(result);
		tjs_string w(s.c_str());
		TVPUtf16ToUtf8(out.body, w);
		return; }
	}
}

// 登録 TJS ハンドラを呼ぶ (メインスレッド。SubmitTask 経由で実行される)
void InvokeHandlerOnMain(const std::string& prefix, const std::string& method,
	const std::string& path, const std::string& query, const std::string& body,
	WebResp& out)
{
	auto it = g_handler_closures.find(prefix);
	if (it == g_handler_closures.end()) {
		out.status = 404; out.body = "no handler";
		return;
	}
	// ハンドラ内で unregister されても安全なようにローカルへ保持
	tTJSVariant handler = it->second;

	iTJSDispatch2* req = TJSCreateDictionaryObject();
	if (!req) { out.status = 500; out.body = "dictionary alloc failed"; return; }
	auto setS = [&](const tjs_char* k, const std::string& utf8) {
		tjs_string w;
		TVPUtf8ToUtf16(w, utf8);
		tTJSVariant vv(ttstr(w.c_str()));
		req->PropSet(TJS_MEMBERENSURE, k, nullptr, &vv, req);
	};
	setS(TJS_W("method"), method);
	setS(TJS_W("path"),   path);
	setS(TJS_W("query"),  query);
	setS(TJS_W("body"),   body);
	if (!body.empty()) {
		// バイナリボディ用に octet も渡す (utf8 変換を経ない生バイト列)
		tTJSVariant vv((const tjs_uint8*)body.data(), (tjs_uint)body.size());
		req->PropSet(TJS_MEMBERENSURE, TJS_W("bytes"), nullptr, &vv, req);
	}
	tTJSVariant reqv(req, req);
	req->Release();

	tTJSVariant result;
	tTJSVariant* args[1] = { &reqv };
	try {
		tjs_error er = handler.AsObjectClosureNoAddRef().FuncCall(
			0, nullptr, nullptr, &result, 1, args, nullptr);
		if (TJS_FAILED(er)) {
			out.status = 500; out.body = "handler call failed";
			return;
		}
	} catch (eTJS& e) {
		out.status = 500;
		{
			tjs_string w(e.GetMessage().c_str());
			TVPUtf16ToUtf8(out.body, w);
		}
		TVPAddImportantLog(ttstr(TJS_W("WebServer: handler error: ")) + e.GetMessage());
		return;
	} catch (...) {
		out.status = 500; out.body = "unknown error in handler";
		return;
	}
	MarshalResult(result, out);
}

// 静的マウントのストレージを読む (メインスレッド。SubmitTask 経由で実行される)
void ReadStorageOnMain(const std::string& utf8name, const std::string& mime, WebResp& out)
{
	tjs_string wname;
	TVPUtf8ToUtf16(wname, utf8name);
	ttstr name(wname.c_str());
	try {
		iTJSBinaryStream* stream = TVPCreateStream(name, TJS_BS_READ);
		if (!stream) { out.status = 404; out.body = "not found"; return; }
		try {
			tjs_uint64 size = stream->GetSize();
			// 開発ツール想定の安全上限 (256MB)
			if (size > (tjs_uint64)256 * 1024 * 1024) {
				out.status = 500; out.body = "file too large";
			} else {
				out.body.resize((size_t)size);
				if (size > 0) stream->Read(&out.body[0], (tjs_uint)size);
				out.status = 200;
				out.mime = mime;
			}
		} catch (...) {
			delete stream;
			throw;
		}
		delete stream;
	} catch (...) {
		// ストレージ未存在は例外で来る
		out.status = 404; out.mime = "text/plain; charset=utf-8"; out.body = "not found";
	}
}

// 登録ハンドラ / 静的マウントへ dispatch (HTTP スレッド)。
// マッチしなければ false (呼び出し側が 404)。
bool DispatchRegistered(sock_t s, const std::string& method, const std::string& p,
	const std::string& query, const std::string& body)
{
	// 最長一致 (ハンドラ / 静的マウント両方から)
	std::string hbest, sbest, sdir;
	{
		std::lock_guard<std::mutex> lk(g_routes_mu);
		for (auto& hp : g_handler_prefixes) {
			if (p.compare(0, hp.size(), hp) == 0 && hp.size() > hbest.size())
				hbest = hp;
		}
		for (auto& m : g_static_mounts) {
			if (p.compare(0, m.first.size(), m.first) == 0 && m.first.size() > sbest.size()) {
				sbest = m.first;
				sdir = m.second;
			}
		}
	}
	if (hbest.empty() && sbest.empty()) return false;

	auto resp = std::make_shared<WebResp>();
	if (hbest.size() >= sbest.size()) {
		// 動的ハンドラ (メインスレッドで実行)
		bool ok = TVPReplMainQueue::SubmitTask([=] {
			InvokeHandlerOnMain(hbest, method, p, query, body, *resp);
		});
		if (!ok) { resp->status = 503; resp->body = "server shutting down"; }
	} else {
		// 静的配信 (GET のみ)
		if (method != "GET") {
			resp->status = 405; resp->body = "method not allowed";
		} else {
			std::string rel = p.substr(sbest.size());
			if (rel.empty() || rel.back() == '/') rel += "index.html";
			if (PathIsUnsafe(rel)) {
				resp->status = 403; resp->body = "forbidden";
			} else {
				std::string name = sdir + rel;
				std::string mime = MimeForPath(rel);
				bool ok = TVPReplMainQueue::SubmitTask([=] {
					ReadStorageOnMain(name, mime, *resp);
				});
				if (!ok) { resp->status = 503; resp->body = "server shutting down"; }
			}
		}
	}
	SendWebResp(s, *resp);
	return true;
}

//---------------------------------------------------------------------------
void HandleConnection(sock_t s)
{
	std::string method, path, body;
	if (ReadRequest(s, method, path, body)) {
		// path のクエリ部を分離 + パスは URL デコード
		size_t q = path.find('?');
		std::string query = (q == std::string::npos) ? std::string() : path.substr(q + 1);
		std::string p = UrlDecode((q == std::string::npos) ? path : path.substr(0, q));
		if (p == "/" || p == "/index.html")      HandleIndex(s);
		else if (p == "/events")                 HandleSse(s, "log", true);   // 長時間保持
		else if (p.rfind("/sub/", 0) == 0 && p.size() > 5)
		                                         HandleSse(s, p.substr(5), false);
		else if (p == "/cmd" && method == "POST") HandleCmd(s, body);
		else if (!DispatchRegistered(s, method, p, query, body))
		                                         Handle404(s);
	}
	closesock(s);
}

//---------------------------------------------------------------------------
void AcceptLoop()
{
	while (g_running.load(std::memory_order_acquire)) {
		sockaddr_in cli;
		socklen_t clen = sizeof(cli);
		sock_t c = ::accept(g_listen, (sockaddr*)&cli, &clen);
		if (c == SOCK_INVALID) {
			if (!g_running.load(std::memory_order_acquire)) break;
			continue;
		}
		std::thread(HandleConnection, c).detach();
	}
}

//---------------------------------------------------------------------------
// ログ sink: エンジンのログを SSE へ流す
bool WebLogSink(TVPLogLevel level, const char* utf8_line)
{
	if (utf8_line) Broadcast(ClassForLevel((int)level), utf8_line);
	// false = 既定コンソール書き出しも継続させる (ブラウザへ流しつつコンソールの
	// ログも止めない)。本体にローカルブラウザが無い一部プラットフォームでは標準の
	// ログ出力が唯一の観測手段であり、また起動直後ブラウザ未接続の間もログを見失わ
	// ないため。
	return false;
}

} // anonymous

//---------------------------------------------------------------------------
bool Wanted() { std::string h; int p; return ParseCmd(h, p) && p > 0; }

bool IsActive() { return g_running.load(std::memory_order_acquire); }

ttstr GetURL()
{
	if (!g_running.load(std::memory_order_acquire)) return ttstr();
	// g_url_host は 0.0.0.0 バインド時に外向き実 IP へ解決済み (Start 参照)。
	tjs_string hw; TVPUtf8ToUtf16(hw, g_url_host);
	return ttstr(TJS_W("http://")) + ttstr(hw.c_str()) +
	       ttstr(TJS_W(":")) + ttstr((tjs_int)g_port) + ttstr(TJS_W("/"));
}

void Start()
{
	if (g_running.load(std::memory_order_acquire)) return;
	std::string host;
	if (!ParseCmd(host, g_port) || g_port <= 0) return;
	g_host = host;
	ttstr hostw; // ログ用 (host は ASCII 想定だが安全のため utf8→utf16 変換)
	{ tjs_string tw; TVPUtf8ToUtf16(tw, host); hostw = ttstr(tw.c_str()); }

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		TVPAddImportantLog(TJS_W("ReplWebServer: WSAStartup failed"));
		return;
	}
#endif

	// URL 表示用ホストを決める。全 IF バインド (0.0.0.0/*) のときは 0.0.0.0 を
	// そのまま見せても接続できないので、実際の外向き IPv4 に解決して見せる。
	// (WSAStartup 後でないと Windows でソケットを作れないためここで解決。)
	if (host == "0.0.0.0" || host == "*") {
		std::string outward = ResolveOutwardIPv4();
		g_url_host = outward.empty() ? host : outward;
	} else {
		g_url_host = host;
	}

	g_listen = ::socket(AF_INET, SOCK_STREAM, 0);
	if (g_listen == SOCK_INVALID) {
		TVPAddImportantLog(TJS_W("ReplWebServer: socket() failed"));
		return;
	}
	int one = 1;
	::setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

	// バインドアドレス解決: "0.0.0.0"/"*"=全IF, それ以外は inet_pton, 失敗時は loopback。
	unsigned long bindaddr;
	bool loopback_only;
	if (host == "0.0.0.0" || host == "*") {
		bindaddr = htonl(INADDR_ANY);
		loopback_only = false;
	} else {
		struct in_addr ia;
		if (inet_pton(AF_INET, host.c_str(), &ia) == 1) {
			bindaddr = ia.s_addr; // network order
			loopback_only = (ia.s_addr == htonl(INADDR_LOOPBACK));
		} else {
			bindaddr = htonl(INADDR_LOOPBACK);
			loopback_only = true;
		}
	}

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((unsigned short)g_port);
	addr.sin_addr.s_addr = bindaddr;
	if (!loopback_only) {
		TVPAddImportantLog(ttstr(TJS_W(
			"ReplWebServer: WARNING binding non-loopback address (")) + hostw +
			ttstr(TJS_W(") — REPL is reachable from the network. Use only on trusted networks/LAN.")));
	}
	if (::bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0 ||
	    ::listen(g_listen, 8) != 0) {
		TVPAddImportantLog(ttstr(TJS_W("ReplWebServer: bind/listen failed on port ")) +
			ttstr((tjs_int)g_port));
		closesock(g_listen);
		g_listen = SOCK_INVALID;
		return;
	}

	g_running.store(true, std::memory_order_release);
	g_accept_thread = std::thread(AcceptLoop);

	// 待受 URL は console log sink を web へ切り替える「前」に出す。
	// 表示 URL は外向き実 IP に解決済みの g_url_host を使う (0.0.0.0 のままだと
	// 接続先が分からないため)。バインド自体は g_host (0.0.0.0=全IF) で行っている。
	// ※ sink 切替後だと URL 行自体が web (まだ誰も接続していない) へ流れて
	//   コンソールに出ず、起動直後に接続先が分からなくなる (これを避ける)。
	ttstr urlhostw; { tjs_string tw; TVPUtf8ToUtf16(tw, g_url_host); urlhostw = ttstr(tw.c_str()); }
	TVPAddImportantLog(ttstr(TJS_W("ReplWebServer: listening on http://")) + urlhostw +
		ttstr(TJS_W(":")) + ttstr((tjs_int)g_port) +
		ttstr(TJS_W("/  (browser REPL viewer)")));

	// 以降のログを SSE ビューワーへも配信する。WebLogSink は false を返すので
	// コンソール既定出力は従来どおり継続する (コンソール ⇄ ブラウザの両方に出る)。
	TVPLogSetConsoleSink(WebLogSink);

	// コンソールが無い (GUI/ダブルクリック起動で URL を出す場所が無い) 場合は、
	// localhost バインド時に限り OS 既定ブラウザで自動的に開く。
	// シェル起動 (コンソールあり) や 0.0.0.0 バインド時はログ表示のみに留める。
#ifdef _WIN32
	bool has_console = (::GetConsoleWindow() != NULL);
#else
	bool has_console = true; // 非 Windows は自動起動しない (ログのみ)
#endif
	if (loopback_only && !has_console) {
		ttstr url = ttstr(TJS_W("http://127.0.0.1:")) + ttstr((tjs_int)g_port) + ttstr(TJS_W("/"));
		TVPShellExecute(url, ttstr());
	}
}

void Stop()
{
	if (!g_running.exchange(false)) return;
	// listen を閉じて accept を解除
	if (g_listen != SOCK_INVALID) { closesock(g_listen); g_listen = SOCK_INVALID; }
	// SSE クライアントを起こして終了させる
	{
		std::lock_guard<std::mutex> lk(g_clients_mu);
		for (auto& c : g_clients) {
			std::lock_guard<std::mutex> clk(c->mu);
			c->closed = true;
			c->cv.notify_all();
		}
	}
	if (g_accept_thread.joinable()) g_accept_thread.join();
	TVPLogSetConsoleSink(nullptr);
#ifdef _WIN32
	WSACleanup();
#endif
}

void LogLine(TVPLogLevel level, const char* utf8_line)
{
	if (utf8_line) Broadcast(ClassForLevel((int)level), utf8_line);
}

//---------------------------------------------------------------------------
// 拡張 API (登録系はメインスレッドから呼ぶこと。ヘッダのコメント参照)
//---------------------------------------------------------------------------

namespace {
std::string TtstrToUtf8(const ttstr& s)
{
	std::string o;
	tjs_string w(s.c_str());
	TVPUtf16ToUtf8(o, w);
	return o;
}
} // anonymous

void RegisterHandler(const ttstr& prefix, const tTJSVariant& handler)
{
	std::string p8 = TtstrToUtf8(prefix);
	if (p8.empty() || p8[0] != '/') return;
	g_handler_closures[p8] = handler;   // main thread only
	std::lock_guard<std::mutex> lk(g_routes_mu);
	if (std::find(g_handler_prefixes.begin(), g_handler_prefixes.end(), p8) ==
	    g_handler_prefixes.end())
		g_handler_prefixes.push_back(p8);
}

bool UnregisterHandler(const ttstr& prefix)
{
	std::string p8 = TtstrToUtf8(prefix);
	bool found = g_handler_closures.erase(p8) > 0;
	std::lock_guard<std::mutex> lk(g_routes_mu);
	auto it = std::find(g_handler_prefixes.begin(), g_handler_prefixes.end(), p8);
	if (it != g_handler_prefixes.end()) g_handler_prefixes.erase(it);
	return found;
}

void RegisterStatic(const ttstr& prefix, const ttstr& storageDir)
{
	std::string p8 = TtstrToUtf8(prefix);
	std::string d8 = TtstrToUtf8(storageDir);
	if (p8.empty() || p8[0] != '/') return;
	std::lock_guard<std::mutex> lk(g_routes_mu);
	for (auto& m : g_static_mounts) {
		if (m.first == p8) { m.second = d8; return; }
	}
	g_static_mounts.emplace_back(p8, d8);
}

bool UnregisterStatic(const ttstr& prefix)
{
	std::string p8 = TtstrToUtf8(prefix);
	std::lock_guard<std::mutex> lk(g_routes_mu);
	for (size_t i = 0; i < g_static_mounts.size(); ++i) {
		if (g_static_mounts[i].first == p8) {
			g_static_mounts.erase(g_static_mounts.begin() + i);
			return true;
		}
	}
	return false;
}

void BroadcastChannel(const ttstr& channel, const ttstr& payload)
{
	PushFrameToChannel(TtstrToUtf8(channel), SseFrame(TtstrToUtf8(payload)));
}

void ClearHandlers()
{
	g_handler_closures.clear();   // main thread only — TJS クロージャの参照を手放す
	std::lock_guard<std::mutex> lk(g_routes_mu);
	g_handler_prefixes.clear();
	g_static_mounts.clear();
}

//---------------------------------------------------------------------------
// 埋め込みビューワー (単一 HTML)。上=ログ / 下=入力。
// ブラウザネイティブで 選択/コピー/貼り付け/スクロール/検索 が効く。
namespace {
const char* kHtmlPage = R"HTMLPAGE(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kirikiri Z REPL</title>
<style>
  :root { color-scheme: dark; }
  html,body { margin:0; height:100%; background:#1e1e1e; color:#d4d4d4;
    font-family: Consolas, "Cascadia Mono", "MS Gothic", monospace; font-size:14px; }
  #app { display:flex; flex-direction:column; height:100vh; }
  #top { padding:4px 8px; background:#252526; border-bottom:1px solid #333;
    display:flex; gap:8px; align-items:center; }
  #top b { color:#4ec9b0; }
  #top input[type=search] { flex:1; background:#1e1e1e; color:#d4d4d4;
    border:1px solid #3c3c3c; padding:3px 6px; border-radius:3px; }
  #log { flex:1; overflow-y:auto; padding:6px 8px; white-space:pre-wrap;
    word-break:break-word; }
  #log .line { }
  .echo    { color:#9cdcfe; }
  .result  { color:#6a9955; }
  .error   { color:#f48771; }
  .help    { color:#4ec9b0; }
  .info    { color:#d4d4d4; }
  .warn    { color:#dcdcaa; }
  .debug   { color:#4fc1ff; }
  .verbose { color:#808080; }
  .critical{ color:#f14c4c; font-weight:bold; }
  .hidden { display:none; }
  #bottom { display:flex; align-items:center; gap:6px; padding:6px 8px;
    background:#252526; border-top:1px solid #333; }
  #prompt { color:#6a9955; font-weight:bold; }
  #cmd { flex:1; background:#1e1e1e; color:#d4d4d4; border:1px solid #3c3c3c;
    padding:5px 8px; border-radius:3px; font:inherit; }
  #status { font-size:12px; color:#808080; }
  button { background:#0e639c; color:#fff; border:0; padding:4px 10px;
    border-radius:3px; cursor:pointer; }
  button:hover { background:#1177bb; }
</style></head>
<body><div id="app">
  <div id="top">
    <b>Kirikiri&nbsp;Z REPL</b>
    <span id="status">connecting...</span>
    <input id="filter" type="search" placeholder="フィルタ (ログを絞り込み)">
    <button id="clear">Clear</button>
  </div>
  <div id="log"></div>
  <div id="bottom">
    <span id="prompt">krkrz&gt;</span>
    <input id="cmd" autocomplete="off" spellcheck="false"
      placeholder="TJS 式/文を入力 (Enter=実行, .help=コマンド一覧, ↑↓=履歴)">
  </div>
</div>
<script>
(function(){
  var log = document.getElementById('log');
  var cmd = document.getElementById('cmd');
  var statusEl = document.getElementById('status');
  var promptEl = document.getElementById('prompt');
  var filterEl = document.getElementById('filter');
  var hist = [], hpos = -1, filterText = '';

  function atBottom(){ return log.scrollHeight - log.scrollTop - log.clientHeight < 40; }
  function applyFilter(el){
    if(!filterText) { el.classList.remove('hidden'); return; }
    el.classList.toggle('hidden', el.textContent.toLowerCase().indexOf(filterText) < 0);
  }
  function append(cls, text){
    var stick = atBottom();
    var d = document.createElement('div');
    d.className = 'line ' + cls;
    d.textContent = text;
    applyFilter(d);
    log.appendChild(d);
    if(stick) log.scrollTop = log.scrollHeight;
  }

  var es = new EventSource('/events');
  es.onopen = function(){ statusEl.textContent = 'connected'; };
  es.onerror = function(){ statusEl.textContent = 'disconnected (retrying)'; };
  es.onmessage = function(e){
    try { var m = JSON.parse(e.data); append(m.cls || 'info', m.text || ''); }
    catch(_) {}
  };

  filterEl.addEventListener('input', function(){
    filterText = filterEl.value.toLowerCase();
    var lines = log.children;
    for(var i=0;i<lines.length;i++) applyFilter(lines[i]);
  });
  document.getElementById('clear').addEventListener('click', function(){
    log.innerHTML=''; cmd.focus();
  });

  function submit(){
    var line = cmd.value;
    cmd.value=''; hpos=-1;
    fetch('/cmd', {method:'POST', body: line})
      .then(function(r){ return r.text(); })
      .then(function(cont){ promptEl.innerHTML = (cont==='1') ? '&nbsp;&nbsp;...&gt;' : 'krkrz&gt;'; })
      .catch(function(){});
    if(line.trim() !== '' && (hist.length===0 || hist[hist.length-1]!==line)) hist.push(line);
  }
  cmd.addEventListener('keydown', function(e){
    if(e.key==='Enter'){ e.preventDefault(); submit(); }
    else if(e.key==='ArrowUp'){ e.preventDefault();
      if(hist.length){ if(hpos<0) hpos=hist.length; if(hpos>0) hpos--; cmd.value=hist[hpos]; } }
    else if(e.key==='ArrowDown'){ e.preventDefault();
      if(hpos>=0){ hpos++; if(hpos>=hist.length){ hpos=-1; cmd.value=''; } else cmd.value=hist[hpos]; } }
  });
  cmd.focus();
})();
</script>
</body></html>
)HTMLPAGE";
} // anonymous

} // namespace TVPReplWeb

#endif // KRKRZ_REPL_WEB
