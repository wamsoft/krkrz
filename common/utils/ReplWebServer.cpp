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
#include "ReplWatch.h"       // 監視式 (/watch + /sub/watch)
#include "TickCount.h"       // TVPGetTickCount (アイドル終了の見張り)
#include "EventIntf.h"       // TVP(Get|Set)SystemEventDisabledState (/state)
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
// 実行ファイルを引数付きで起動する (プログラム実行専用。Edge/Chrome --app 用)。
extern bool TVPExecuteProgram(const ttstr &exe, const ttstr &args);

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
	// 「いますぐ起きて生存確認 (:ping) を送れ」の合図。 閉じる合図 (POST /bye)
	// が来たとき、 ハートビートの周期を待たずに死んだ socket を落とすため。
	bool                     poke = false;
};

std::mutex                             g_clients_mu;
std::vector<std::shared_ptr<Client>>   g_clients;

//---------------------------------------------------------------------------
// ブラウザが閉じたらアプリも終わる (-replwebidle=<秒>)
//
// ブラウザを UI にした構成 (`-replweb` + アプリモード起動) では、 ウィンドウを
// 閉じたのに本体だけ残り続けるのが困る。 **SSE 購読が 1 本も無い状態が <秒>
// 続いたら終了**する見張りを置く。
//
// 既定は無効 (0)。 ブラウザを開かないエージェント駆動や、 `-replweb` を単なる
// API 面として使う構成を勝手に殺さないため。 また **一度でも購読が来てから
// 武装する** ので、 有効にしていてもブラウザを開く前に落ちることはない。
// 判定と終了はメインスレッド (TVPDrainREPL → CheckIdleShutdown) で行う。
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Pad (スクリプトエディタ) の書込許可ディレクトリ (-replwebpad=<dir>)
//
// **既定は書込禁止**。 `-replweb` は 0.0.0.0 バインドもできるし、 UI の [保存]
// はうっかり押せるので、 «書ける場所を明示したときだけ書ける» に倒す。
// 値はストレージパスの接頭辞 (例 `-replwebpad=scenario/`)。 これが空でない
// ときだけ POST /pad/file を受け付け、 かつ path がこの接頭辞で始まること。
//
// なお **これはセキュリティ境界ではない** — `/cmd` で任意の TJS が実行できる
// 時点でプロセスの全権限が開いている。 «事故で資材を上書きしない» ための柵。
//---------------------------------------------------------------------------
std::string               g_pad_write_dir;     // 空 = 書込禁止

constexpr int             kIdleDefaultSec = 5; // -replwebidle 未指定時
int                       g_idle_sec = 0;      // 0 = 無効
std::atomic<bool>         g_ever_subscribed{false};
std::atomic<tjs_uint64>   g_last_client_gone{0};  // 0 = 購読中 (または未武装)

// 閉じる合図 (POST /bye) を受けた印。 ブラウザは pagehide / beforeunload で
// beacon を投げてくるので、 それが来たら猶予を <秒> ではなく下の短い値へ
// 切り替える。 «本当に誰も居ないか» の判定は購読数のままなので、 複数タブの
// 1 枚を閉じただけでは畳まれない (残ったタブの購読が勝つ)。
//
// 合図は **socket が閉じるより先に届く** (beacon は pagehide 中に飛び、 TCP は
// その後で落ちる)。 1 回叩き起こすだけでは «まだ生きている» と判定されるので、
// 合図から kByeProbeMs の間は kByePokeMs ごとに叩き起こし続けて、 死んだ
// socket が落ちるのを待つ。 探り切っても購読が残るなら «別タブが閉じただけ»
// なので合図を取り下げる。
std::atomic<tjs_uint64>   g_bye_at{0};          // 0 = 合図なし
std::atomic<tjs_uint64>   g_bye_last_poke{0};
constexpr tjs_uint64      kByeGraceMs = 2000;   // 購読ゼロ確認後の猶予
constexpr tjs_uint64      kByeProbeMs = 3000;   // 合図から探り続ける時間
constexpr tjs_uint64      kByePokeMs  = 200;    // 探る間隔

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
// 全 SSE クライアントを叩き起こす。 起きた側は送るものが無ければ :ping を
// 出すので、 **死んだ socket がその場で落ちる**。 閉じる合図 (POST /bye) から
// «本当に居なくなったか» をハートビート周期を待たずに確かめるために使う。
void PokeAllClients()
{
	std::lock_guard<std::mutex> lk(g_clients_mu);
	for (auto& c : g_clients) {
		std::lock_guard<std::mutex> clk(c->mu);
		c->poke = true;
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
	// アイドル終了の見張り: 購読が来たので «ブラウザが居る» 状態へ。
	// 新しい購読は «閉じる合図» を取り消す (開き直し / 別タブ)。
	g_ever_subscribed.store(true, std::memory_order_release);
	g_last_client_gone.store(0, std::memory_order_release);
	g_bye_at.store(0, std::memory_order_release);

	// ハートビート間隔。 切断は «次の :ping が失敗して初めて» 分かるので、
	// これが切断検知の遅れの上限になる。 アイドル終了を武装しているときは
	// «ブラウザが閉じてから終了まで» がこのぶん延びてしまうので、 短くする
	// (コメント 1 行なので通信量は無視できる)。
	int hb_sec = 15;
	if (g_idle_sec > 0 && g_idle_sec < hb_sec) hb_sec = g_idle_sec < 1 ? 1 : g_idle_sec;

	bool alive = true;
	while (alive && g_running.load(std::memory_order_acquire)) {
		std::deque<std::string> batch;
		{
			std::unique_lock<std::mutex> lk(client->mu);
			client->cv.wait_for(lk, std::chrono::seconds(hb_sec), [&] {
				return !client->queue.empty() || client->closed || client->poke ||
				       !g_running.load(std::memory_order_acquire);
			});
			if (client->closed) alive = false;
			client->poke = false;
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
	bool none_left = false;
	{
		std::lock_guard<std::mutex> lk(g_clients_mu);
		for (size_t i = 0; i < g_clients.size(); ++i) {
			if (g_clients[i] == client) { g_clients.erase(g_clients.begin() + i); break; }
		}
		none_left = g_clients.empty();
	}
	// 最後の 1 本が抜けた時刻を覚える (残っているなら 0 に戻す — リロードや
	// タブ追加で出入りするので «最後に空になった時刻» だけが意味を持つ)。
	g_last_client_gone.store(none_left ? TVPGetTickCount() : 0,
	                         std::memory_order_release);
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

// application/x-www-form-urlencoded のデコード ('+' は空白)。
std::string UrlDecodeForm(const std::string& s)
{
	std::string t = s;
	for (auto& c : t) if (c == '+') c = ' ';
	return UrlDecode(t);
}

// "a=1&b=2" → map。 値が無いキーは空文字列。
std::map<std::string, std::string> ParseFormParams(const std::string& src)
{
	std::map<std::string, std::string> out;
	size_t i = 0;
	while (i < src.size()) {
		size_t amp = src.find('&', i);
		std::string pair = src.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
		if (!pair.empty()) {
			size_t eq = pair.find('=');
			if (eq == std::string::npos) out[UrlDecodeForm(pair)] = std::string();
			else out[UrlDecodeForm(pair.substr(0, eq))] = UrlDecodeForm(pair.substr(eq + 1));
		}
		if (amp == std::string::npos) break;
		i = amp + 1;
	}
	return out;
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
// Pad (/pad/exec, /pad/file) — 吉里吉里2 の「スクリプトエディタ」窓の相当物
//
// 原典 (utils/win32/PadFormUnit.cpp) は複数行テキスト窓で、 機能は
// **Execute (テキストを実行)** と **Save**。 編集操作 (Undo/Cut/Copy/Paste) は
// ブラウザの textarea が持っているので、 サーバに要るのはこの 2 つだけ。
//
//   POST /pad/exec         body = TJS スクリプト (複数行可)
//                          → {"ok":bool,"result":"...","error":"..."}
//   GET  /pad/file?path=   ストレージから読む (text/plain)
//   POST /pad/file?path=   ストレージへ書く。 **-replwebpad=<dir> 配下のみ**
//
// exec は `/cmd` (1 行 + ドットコマンド) と違い **本文をまるごと 1 回**
// 実行する。 複数行の関数定義やループをそのまま流せる。
//---------------------------------------------------------------------------
void HandlePadExec(sock_t s, const std::string& body)
{
	WebResp r;
	r.mime = "application/json; charset=utf-8";
	std::string script = body;
	// 末尾の改行だけ落とす (先頭の字下げは意味を持つので触らない)
	while (!script.empty() && (script.back() == '\n' || script.back() == '\r'))
		script.pop_back();
	if (script.empty()) {
		r.status = 400;
		r.body = "{\"error\":\"empty script\"}";
		SendWebResp(s, r);
		return;
	}
	tjs_string u16;
	TVPUtf8ToUtf16(u16, script);
	tTJSVariant result;
	ttstr error;
	// Submit は HTTP スレッドから呼んでよい (メインの Drain が処理して起こす)。
	bool ok = TVPReplMainQueue::Submit(ttstr(u16.c_str()), result, error);
	std::string result_utf8, error_utf8;
	if (ok) {
		ttstr pp = TVPPrettyPrint(result, 4, false);
		std::string tmp; TVPUtf16ToUtf8(tmp, pp.AsStdString());
		result_utf8 = tmp;
	} else {
		std::string tmp; TVPUtf16ToUtf8(tmp, error.AsStdString());
		error_utf8 = tmp;
		if (error_utf8.empty()) error_utf8 = "server shutting down";
	}
	r.status = 200;
	r.body = std::string("{\"ok\":") + (ok ? "true" : "false") +
	         ",\"result\":\"" + JsonEscape(result_utf8) + "\"" +
	         ",\"error\":\"" + JsonEscape(error_utf8) + "\"}";
	SendWebResp(s, r);
}

// ストレージへ書く (メインスレッド)。
void WriteStorageOnMain(const std::string& utf8name, const std::string& data,
                        WebResp& out)
{
	tjs_string wname;
	TVPUtf8ToUtf16(wname, utf8name);
	ttstr name(wname.c_str());
	try {
		iTJSBinaryStream* stream = TVPCreateStream(name, TJS_BS_WRITE);
		if (!stream) {
			out.status = 500;
			out.body = "{\"error\":\"cannot open for write\"}";
			return;
		}
		try {
			if (!data.empty())
				stream->Write(data.data(), (tjs_uint)data.size());
		} catch (...) {
			delete stream;
			throw;
		}
		delete stream;
		out.status = 200;
		out.body = "{\"ok\":true}";
	} catch (...) {
		out.status = 500;
		out.body = "{\"error\":\"write failed\"}";
	}
}

void HandlePadFile(sock_t s, const std::string& method,
                   const std::string& query, const std::string& body)
{
	WebResp r;
	r.mime = "application/json; charset=utf-8";
	auto params = ParseFormParams(query);
	auto it = params.find("path");
	std::string path = (it == params.end()) ? std::string() : it->second;

	if (path.empty() || PathIsUnsafe(path)) {
		r.status = 400;
		r.body = "{\"error\":\"bad or missing path\"}";
		SendWebResp(s, r);
		return;
	}

	if (method == "GET") {
		auto resp = std::make_shared<WebResp>();
		std::string name = path;
		bool ran = TVPReplMainQueue::SubmitTask([=] {
			ReadStorageOnMain(name, "text/plain; charset=utf-8", *resp);
		});
		if (!ran) { r.status = 503; r.body = "{\"error\":\"server shutting down\"}"; SendWebResp(s, r); return; }
		SendWebResp(s, *resp);
		return;
	}
	if (method != "POST") {
		r.status = 405;
		r.body = "{\"error\":\"method not allowed\"}";
		SendWebResp(s, r);
		return;
	}

	// --- 書込は許可ディレクトリ配下のみ ---
	if (g_pad_write_dir.empty()) {
		r.status = 403;
		r.body = "{\"error\":\"saving is disabled (start with -replwebpad=<dir> to allow)\"}";
		SendWebResp(s, r);
		return;
	}
	if (path.compare(0, g_pad_write_dir.size(), g_pad_write_dir) != 0) {
		r.status = 403;
		r.body = "{\"error\":\"outside the allowed directory (" +
		         JsonEscape(g_pad_write_dir) + ")\"}";
		SendWebResp(s, r);
		return;
	}

	auto resp = std::make_shared<WebResp>();
	std::string name = path, data = body;
	bool ran = TVPReplMainQueue::SubmitTask([=] {
		WriteStorageOnMain(name, data, *resp);
	});
	if (!ran) { r.status = 503; r.body = "{\"error\":\"server shutting down\"}"; SendWebResp(s, r); return; }
	resp->mime = "application/json; charset=utf-8";
	SendWebResp(s, *resp);
}

//---------------------------------------------------------------------------
// コントローラ (/state) — 吉里吉里2 の「コントローラ」窓の相当物
//
// 原典 (environ/win32/MainFormUnit.cpp) のツールバーは ScriptEditor / Console /
// Watch / Event / Exit の 5 つ。 前 3 つはこの UI ではタブなので、 サーバ側に
// 要るのは **Event (System.eventDisabled) と Exit** だけ。
//
//   GET  /state   { "eventDisabled": bool }
//   POST /state   op=event value=on|off|toggle   → イベント配送の停止/再開
//                 op=exit                        → アプリ終了
//
// 状態は /sub/state へも push する (複数タブの同期 + **ゲーム自身が
// eventDisabled を変えたときの追従**)。 変化の検出は毎フレーム
// PublishStateIfChanged() で行う。
//---------------------------------------------------------------------------
std::string StateJson(bool event_disabled)
{
	return std::string("{\"eventDisabled\":") + (event_disabled ? "true" : "false") + "}";
}

void HandleState(sock_t s, const std::string& method,
                 const std::string& query, const std::string& body)
{
	WebResp r;
	r.mime = "application/json; charset=utf-8";
	auto params = ParseFormParams(body.empty() ? query : body);
	auto param = [&](const char* k) -> std::string {
		auto i = params.find(k);
		return i == params.end() ? std::string() : i->second;
	};

	if (method == "POST") {
		std::string op = param("op");
		if (op == "event") {
			std::string v = param("value");
			bool ran = TVPReplMainQueue::SubmitTask([&v] {
				if (v == "on" || v == "true" || v == "1")
					TVPSetSystemEventDisabledState(true);
				else if (v == "off" || v == "false" || v == "0")
					TVPSetSystemEventDisabledState(false);
				else
					TVPSetSystemEventDisabledState(!TVPGetSystemEventDisabledState());
			});
			if (!ran) {
				r.status = 503;
				r.body = "{\"error\":\"server shutting down\"}";
				SendWebResp(s, r);
				return;
			}
		} else if (op == "exit") {
			// 応答を返してから終了させる (ブラウザ側で fetch が失敗しないように)。
			r.status = 200;
			r.body = StateJson(TVPGetSystemEventDisabledState());
			SendWebResp(s, r);
			TVPReplMainQueue::SubmitTask([] {
				TVPAddImportantLog(TJS_W("ReplWeb: exit requested from browser"));
				TVPTerminateAsync(0);
			});
			return;
		} else {
			r.status = 400;
			r.body = "{\"error\":\"unknown op (event/exit)\"}";
			SendWebResp(s, r);
			return;
		}
	} else if (method != "GET") {
		r.status = 405;
		r.body = "{\"error\":\"method not allowed\"}";
		SendWebResp(s, r);
		return;
	}

	r.status = 200;
	r.body = StateJson(TVPGetSystemEventDisabledState());
	SendWebResp(s, r);
}

//---------------------------------------------------------------------------
// POST /bye — 「このページを閉じます」の合図 (ブラウザの pagehide /
// beforeunload から navigator.sendBeacon で飛んでくる)。
//
// アイドル終了 (-replwebidle) の猶予を <秒> から kByeGraceMs へ前倒しするだけ
// で、 «本当に誰も居ないか» の判定は購読数のまま。 複数タブの 1 枚を閉じても
// 残ったタブの購読が勝つので畳まれない。 合図が届かなくても (beacon は
// ベストエフォート) 従来どおり <秒> 経てば畳まれる — **速くなるだけの経路**。
//
// 合図を受けたら全 SSE クライアントを叩き起こす。 切断は :ping の送信が失敗
// して初めて分かるので、 ここで一度 ping させないとハートビート周期ぶん待つ
// ことになる (それでは前倒しの意味がない)。
//
// 見張りが無効 (既定) でも 204 を返すだけで害は無い。
//---------------------------------------------------------------------------
void HandleBye(sock_t s)
{
	if (g_idle_sec > 0) {
		tjs_uint64 now = TVPGetTickCount();
		g_bye_at.store(now ? now : 1, std::memory_order_release);
		g_bye_last_poke.store(0, std::memory_order_release);
		PokeAllClients();
	}
	WebResp r;
	r.status = 204;
	SendWebResp(s, r);
}

//---------------------------------------------------------------------------
// 監視式 API (/watch) — 吉里吉里2 デバッグ窓「監視式」の Web 側 (P2)
//
// パラメータは **application/x-www-form-urlencoded** で受ける (クエリでも
// body でも可。 body 優先)。 計画 (doc/DebugToolsRevival.md §4.3) は JSON body
// だったが、 本体に JSON パーサを増やさない方針なので form 形式にした。
// curl からも `-d 'op=add&expr=1+2'` で叩けて、 ブラウザからは
// URLSearchParams がそのまま使える。
//
//   GET  /watch            現在の一覧 + 値 (評価しない = ポーリングしても安全)
//   GET  /watch?eval=1     全件評価してから返す (原典の Update ボタン相当)
//   POST /watch  op=add       expr=<式>      式を追加して評価
//                op=rm        id=<id>        削除
//                op=edit      id=<id> expr=<式>  差し替えて評価
//                op=clear                    全消し
//                op=interval  ms=<ms>        自動更新の間隔 (0=毎フレーム / <0=off)
//                op=eval                     全件評価のみ
//
// 応答は成功なら常に現在の状態 (GET と同じ JSON)、 引数不正なら 400 +
// {"error":"..."}。 自動更新の push 先は既存の汎用 SSE /sub/watch。
//---------------------------------------------------------------------------

void HandleWatch(sock_t s, const std::string& method,
                 const std::string& query, const std::string& body)
{
	WebResp r;
	r.mime = "application/json; charset=utf-8";

	auto fail = [&](int status, const std::string& msg) {
		r.status = status;
		r.body = "{\"error\":\"" + JsonEscape(msg) + "\"}";
	};
	auto okState = [&] {
		r.status = 200;
		r.body = TVPReplWatch::ToJson();
	};

	// body が空ならクエリを見る (GET はクエリのみ)。
	auto params = ParseFormParams(body.empty() ? query : body);

	if (method == "GET") {
		// 既定は «評価しない» スナップショット。 GET が任意の TJS を走らせるのは
		// 驚きが大きく、 ポーリングでも安全であってほしいため。
		auto it = params.find("eval");
		if (it != params.end() && it->second != "0" && it->second != "false") {
			if (!TVPReplWatch::EvaluateAllOnMain()) { fail(503, "server shutting down"); SendWebResp(s, r); return; }
		}
		okState();
		SendWebResp(s, r);
		return;
	}
	if (method != "POST") {
		fail(405, "method not allowed");
		SendWebResp(s, r);
		return;
	}

	auto pit = params.find("op");
	std::string op = (pit == params.end()) ? std::string() : pit->second;
	auto param = [&](const char* k) -> std::string {
		auto i = params.find(k);
		return i == params.end() ? std::string() : i->second;
	};
	auto evalAll = [&] { return TVPReplWatch::EvaluateAllOnMain(); };
	auto toTt = [](const std::string& u8) {
		tjs_string w; TVPUtf8ToUtf16(w, u8); return ttstr(w.c_str());
	};

	if (op == "add") {
		std::string expr = param("expr");
		if (expr.empty()) { fail(400, "op=add needs expr"); SendWebResp(s, r); return; }
		TVPReplWatch::Add(toTt(expr));
		if (!evalAll()) { fail(503, "server shutting down"); SendWebResp(s, r); return; }
	} else if (op == "rm") {
		std::string ids = param("id");
		int id = std::atoi(ids.c_str());
		if (id <= 0)                     { fail(400, "op=rm needs id"); SendWebResp(s, r); return; }
		if (!TVPReplWatch::Remove(id))   { fail(404, "no such watch id"); SendWebResp(s, r); return; }
	} else if (op == "edit") {
		std::string ids = param("id"), expr = param("expr");
		int id = std::atoi(ids.c_str());
		if (id <= 0 || expr.empty())     { fail(400, "op=edit needs id and expr"); SendWebResp(s, r); return; }
		if (!TVPReplWatch::Edit(id, toTt(expr))) { fail(404, "no such watch id"); SendWebResp(s, r); return; }
		if (!evalAll()) { fail(503, "server shutting down"); SendWebResp(s, r); return; }
	} else if (op == "clear") {
		TVPReplWatch::Clear();
	} else if (op == "interval") {
		auto mit = params.find("ms");
		if (mit == params.end())         { fail(400, "op=interval needs ms"); SendWebResp(s, r); return; }
		TVPReplWatch::SetInterval(std::atoi(mit->second.c_str()));
	} else if (op == "eval") {
		if (!evalAll()) { fail(503, "server shutting down"); SendWebResp(s, r); return; }
	} else {
		fail(400, "unknown op (add/rm/edit/clear/interval/eval)");
		SendWebResp(s, r);
		return;
	}
	okState();
	SendWebResp(s, r);
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
		else if (p == "/watch")                  HandleWatch(s, method, query, body);
		else if (p == "/bye" && method == "POST") HandleBye(s);
		else if (p == "/state")                  HandleState(s, method, query, body);
		else if (p == "/pad/exec" && method == "POST") HandlePadExec(s, body);
		else if (p == "/pad/file")               HandlePadFile(s, method, query, body);
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

void StartOn(const std::string& host_in, int port)
{
	if (g_running.load(std::memory_order_acquire)) return;
	if (port <= 0 || port >= 65536) return;
	std::string host = host_in.empty() ? std::string("127.0.0.1") : host_in;
	g_port = port;
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

}

//---------------------------------------------------------------------------
// -replweb=<port> を解釈して起動する (従来の起動口)。
// GUI(コンソール無し)起動時は loopback バインドに限り自動でブラウザを開く
// (アプリモード優先 → 不可なら既定ブラウザ)。シェル起動 (コンソールあり) や
// 0.0.0.0 バインド時はログ表示のみに留める。
void Start()
{
	std::string host; int port = 0;
	if (!ParseCmd(host, port) || port <= 0) return;
	StartOn(host, port);
	if (!IsActive()) return;

	// -replwebidle=<秒> : ブラウザ (SSE 購読) が全部消えて <秒> 経ったら終了。
	//
	// **既定は有効** (kIdleDefaultSec)。 ブラウザを UI にした構成で、 ウィンドウ
	// を閉じたのに本体だけ残るのが既定の挙動だと困るため。 «一度でも購読が来て
	// から武装する» ので、 ブラウザを開かないエージェント駆動や API 面としての
	// 利用は影響を受けない (購読ゼロのまま何時間でも生きる)。
	// 明示的に切りたいときは -replwebidle=no / off / 0。
	g_idle_sec = kIdleDefaultSec;
	{
		tTJSVariant iv;
		if (TVPGetCommandLine(TJS_W("-replwebidle"), &iv)) {
			ttstr o(iv);
			if (o == TJS_W("no") || o == TJS_W("off") || o == TJS_W("false") ||
			    o == TJS_W("0")) {
				g_idle_sec = 0;
			} else if (o.IsEmpty() || o == TJS_W("yes") || o == TJS_W("on") ||
			           o == TJS_W("true")) {
				g_idle_sec = kIdleDefaultSec;
			} else {
				g_idle_sec = (int)(tjs_int)iv;
				if (g_idle_sec < 0) g_idle_sec = 0;
			}
		}
	}
	// -replwebpad=<dir> : Pad の [保存] を許すストレージ接頭辞。 未指定なら
	// 書込禁止 (403)。 «書ける場所を明示したときだけ書ける» に倒してある。
	{
		tTJSVariant pv;
		if (TVPGetCommandLine(TJS_W("-replwebpad"), &pv)) {
			ttstr d(pv);
			if (!d.IsEmpty()) {
				tjs_string t(d.c_str());
				TVPUtf16ToUtf8(g_pad_write_dir, t);
				// 接頭辞比較なので、 ディレクトリ指定は末尾 '/' を補って
				// "scenario" が "scenario_old/..." に当たらないようにする。
				if (!g_pad_write_dir.empty() && g_pad_write_dir.back() != '/')
					g_pad_write_dir += '/';
				TVPAddLog(ttstr(TJS_W("ReplWeb: pad save allowed under ")) + d);
			}
		}
	}

	if (g_idle_sec > 0) {
		TVPAddLog(ttstr(TJS_W("ReplWeb: idle shutdown armed (")) +
			ttstr((tjs_int)g_idle_sec) +
			ttstr(TJS_W(" s after the last browser disconnects; -replwebidle=no to disable)")));
	} else {
		TVPAddLog(TJS_W("ReplWeb: idle shutdown disabled (-replwebidle=no)"));
	}

	// --- ブラウザ自動オープン ---
	// 既定は «ループバック束縛 かつ コンソール無し (= GUI 起動)» のときだけ
	// アプリモードで開く。 端末から起動したときに勝手に開かないのは、 端末が
	// あるなら自分で開けるし、 CI / エージェント駆動を邪魔しないため。
	//
	// -replwebopen=<app|tab|no> で明示指定できる:
	//   app       アプリモード (Edge / Chrome の --app。 枠なしウィンドウ)
	//   tab / yes 既定ブラウザの通常ウィンドウ
	//   no / off  開かない (上の自動オープンも抑止)
	// **端末から起動しつつブラウザも開きたい**ときや、 逆に GUI 起動で開かせ
	// たくないときはこれを使う。 -replwebidle と組むとブラウザ = アプリになる。
	bool loopback = (host == "127.0.0.1" || host == "localhost" || host == "::1");
#ifdef _WIN32
	bool has_console = (::GetConsoleWindow() != NULL);
#else
	bool has_console = true; // 非 Windows は自動起動しない (ログのみ)
#endif
	bool open = (loopback && !has_console);
	bool app_mode = true;
	tTJSVariant ov;
	if (TVPGetCommandLine(TJS_W("-replwebopen"), &ov)) {
		ttstr o(ov);
		if (o == TJS_W("no") || o == TJS_W("off") || o == TJS_W("false") || o == TJS_W("0")) {
			open = false;
		} else if (o == TJS_W("tab") || o == TJS_W("window") ||
		           o == TJS_W("browser") || o == TJS_W("yes") ||
		           o == TJS_W("on") || o == TJS_W("true") || o == TJS_W("1")) {
			open = true; app_mode = false;
		} else {   // "app" / 空 (= -replwebopen だけ書いた) はアプリモード
			open = true; app_mode = true;
		}
	}
	if (open) {
		bool ok = OpenBrowser(ttstr(), app_mode);
		TVPAddImportantLog(ttstr(TJS_W("ReplWeb: open browser (")) +
			ttstr(app_mode ? TJS_W("app mode") : TJS_W("tab")) +
			ttstr(TJS_W(") -> ")) + GetURL() +
			ttstr(ok ? TJS_W(" [ok]") : TJS_W(" [failed]")));
	} else {
		TVPAddLog(TJS_W("ReplWeb: browser auto-open skipped "
		                "(use -replwebopen=app|tab to force)"));
	}
}

//---------------------------------------------------------------------------
// URL をブラウザで開く。appMode=true なら Edge→Chrome を --app モードで試し、
// いずれも起動できなければ既定ブラウザ (通常ウィンドウ) へフォールバックする。
// url 空なら稼働中サーバの URL を使う。開けたら true。
bool OpenBrowser(const ttstr& url, bool appMode)
{
	ttstr u = url.IsEmpty() ? GetURL() : url;
	if (u.IsEmpty()) return false;
	if (appMode) {
		ttstr appArg = ttstr(TJS_W("--app=")) + u;
		if (TVPExecuteProgram(ttstr(TJS_W("msedge.exe")), appArg)) return true;
		if (TVPExecuteProgram(ttstr(TJS_W("chrome.exe")), appArg)) return true;
		// アプリモード不可 → 既定ブラウザ (通常ウィンドウ) へフォールバック
	}
	return TVPShellExecute(u, ttstr());
}

void Stop()
{
	if (!g_running.load(std::memory_order_acquire)) return;
	// **終了することを購読者へ知らせてから畳む**。 ブラウザはこれを見て自分の
	// ウィンドウを閉じる (本体だけ終わってページが残るのを防ぐ)。 g_running を
	// 落とす前に流すこと — 落とすと SSE スレッドが送らずに抜けてしまう。
	{
		std::string json = std::string("{\"eventDisabled\":") +
			(TVPGetSystemEventDisabledState() ? "true" : "false") +
			",\"exiting\":true}";
		tjs_string u16;
		TVPUtf8ToUtf16(u16, json);
		BroadcastChannel(ttstr(TJS_W("state")), ttstr(u16.c_str()));
		// 送り切る時間を少しだけ与える (SSE スレッドが起きて send するまで)。
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
	}
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

//---------------------------------------------------------------------------
// アイドル終了の見張り。 メインスレッドから毎フレーム呼ばれる (TVPDrainREPL)。
// «一度でも購読が来た» 後に購読が 0 本になり、 その状態が -replwebidle=<秒>
// 続いたらアプリを終了する。 判定も終了要求もメインスレッドなので、 HTTP
// スレッドから終了を叩くより安全。
//---------------------------------------------------------------------------
void CheckIdleShutdown()
{
	if (g_idle_sec <= 0) return;
	if (!g_running.load(std::memory_order_acquire)) return;
	if (!g_ever_subscribed.load(std::memory_order_acquire)) return;
	tjs_uint64 now  = TVPGetTickCount();
	tjs_uint64 bye  = g_bye_at.load(std::memory_order_acquire);
	tjs_uint64 gone = g_last_client_gone.load(std::memory_order_acquire);

	if (gone == 0) {
		// まだ購読が残っている。 閉じる合図から少しの間は叩き起こして探る
		// (合図は socket が落ちるより先に届くため)。 探り切っても残るなら
		// «別タブが閉じただけ» なので取り下げる。
		if (bye != 0 && now >= bye) {
			if (now - bye > kByeProbeMs) {
				g_bye_at.store(0, std::memory_order_release);
			} else {
				tjs_uint64 last = g_bye_last_poke.load(std::memory_order_acquire);
				if (last == 0 || now - last >= kByePokeMs) {
					g_bye_last_poke.store(now, std::memory_order_release);
					PokeAllClients();
				}
			}
		}
		return;
	}
	if (now < gone) return;  // 念のため (時刻が巻き戻ったら次フレームで測り直す)
	// 閉じる合図が来ているなら短い猶予で畳む。 来ていなければ従来どおり <秒>。
	tjs_uint64 grace = (bye != 0) ? kByeGraceMs : (tjs_uint64)g_idle_sec * 1000;
	if (now - gone < grace) return;

	// 二重要求を避けるため武装解除してから落とす。
	g_idle_sec = 0;
	TVPAddImportantLog(TJS_W("ReplWeb: no browser connected — shutting down (-replwebidle)"));
	TVPTerminateAsync(0);
}

//---------------------------------------------------------------------------
// コントローラの状態を /sub/state へ push する (変化したときだけ)。
// メインスレッドから毎フレーム呼ばれる (TVPDrainREPL)。 ここで見ているので、
// **ゲーム自身が System.eventDisabled を変えてもブラウザの表示が追従する**。
//---------------------------------------------------------------------------
void PublishStateIfChanged()
{
	if (!g_running.load(std::memory_order_acquire)) return;
	static int last = -1;   // -1 = 未取得
	bool ed = TVPGetSystemEventDisabledState();
	int cur = ed ? 1 : 0;
	if (cur == last) return;
	last = cur;
	std::string json = StateJson(ed);
	tjs_string u16;
	TVPUtf8ToUtf16(u16, json);
	BroadcastChannel(ttstr(TJS_W("state")), ttstr(u16.c_str()));
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
// 埋め込みビューワー (単一 HTML)。 Console / Watch のタブ構成。
// 肥大するので本体は replweb_ui.inc へ分離してある (ビルド構成は変更なし)。
namespace {
#include "replweb_ui.inc"
} // anonymous

} // namespace TVPReplWeb

#endif // KRKRZ_REPL_WEB
