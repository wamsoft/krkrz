//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Debug Adapter Protocol TCP server (Phase 2 minimal).
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "DAPServer.h"

#ifdef KRKRZ_ENABLE_DAP

#include "DebugIntf.h"  // TVPAddImportantLog

#include <cstring>
#include <cstdint>
#include <string>

// ----- platform abstraction -----
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
   static inline int CloseSocket(socket_t s) { return ::closesocket(s); }
   static inline int LastSockError()         { return ::WSAGetLastError(); }
   static const socket_t INVALID_SOCK = INVALID_SOCKET;
   static const int      SOCK_ERR     = SOCKET_ERROR;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   using socket_t = int;
   static inline int CloseSocket(socket_t s) { return ::close(s); }
   static inline int LastSockError()         { return errno; }
   static const socket_t INVALID_SOCK = -1;
   static const int      SOCK_ERR     = -1;
#endif

// listen_sock_ptr_ / client_sock_ptr_ は void* で持つので reinterpret する
static inline socket_t SockOf(void* p)
{
	if (!p) return INVALID_SOCK;
	return (socket_t)(intptr_t)p;
}
static inline void* PtrOf(socket_t s)
{
	return (void*)(intptr_t)s;
}

//---------------------------------------------------------------------------
// WinSock の参照カウント付き初期化
//---------------------------------------------------------------------------
#ifdef _WIN32
static int g_wsa_refcount = 0;
static std::mutex g_wsa_mtx;
static bool WSAEnsureInit()
{
	std::lock_guard<std::mutex> lk(g_wsa_mtx);
	if (g_wsa_refcount == 0) {
		WSADATA d;
		if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
	}
	g_wsa_refcount++;
	return true;
}
static void WSARelease()
{
	std::lock_guard<std::mutex> lk(g_wsa_mtx);
	if (g_wsa_refcount > 0) {
		g_wsa_refcount--;
		if (g_wsa_refcount == 0) ::WSACleanup();
	}
}
#else
static bool WSAEnsureInit() { return true; }
static void WSARelease() {}
#endif

//---------------------------------------------------------------------------
tTVPDAPServerThread::tTVPDAPServerThread(int port, NotifyCallback on_message)
	: tTVPThread("DAPServerThread"), port_(port), on_message_(std::move(on_message))
{
	WSAEnsureInit();
	StartThread();
}

tTVPDAPServerThread::~tTVPDAPServerThread()
{
	Shutdown();
	WaitFor();
	WSARelease();
}

void tTVPDAPServerThread::Shutdown()
{
	terminating_.store(true, std::memory_order_release);
	Terminate();
	// listen socket を強制 close して accept から抜けさせる
	if (listen_sock_ptr_) {
		CloseSocket(SockOf(listen_sock_ptr_));
		listen_sock_ptr_ = nullptr;
	}
	if (client_sock_ptr_) {
		CloseSocket(SockOf(client_sock_ptr_));
		client_sock_ptr_ = nullptr;
	}
}

//---------------------------------------------------------------------------
bool tTVPDAPServerThread::TryPopMessage(picojson::value& out)
{
	std::lock_guard<std::mutex> lk(incoming_mtx_);
	if (incoming_.empty()) return false;
	out = std::move(incoming_.front());
	incoming_.pop_front();
	return true;
}

void tTVPDAPServerThread::PostMessage(const picojson::value& msg)
{
	std::lock_guard<std::mutex> lk(outgoing_mtx_);
	outgoing_.push_back(msg);
}

void tTVPDAPServerThread::CloseClient()
{
	if (client_sock_ptr_) {
		CloseSocket(SockOf(client_sock_ptr_));
		client_sock_ptr_ = nullptr;
	}
	connected_.store(false, std::memory_order_release);
	// 切断時は受信/送信キューをクリア (前接続の残骸を持ち越さない)
	{ std::lock_guard<std::mutex> lk(incoming_mtx_); incoming_.clear(); }
	{ std::lock_guard<std::mutex> lk(outgoing_mtx_); outgoing_.clear(); }
}

//---------------------------------------------------------------------------
// Content-Length: N\r\n\r\n<json> framing parser.
// buf に追記された raw bytes から完全な 1 メッセージを切り出して out に返す。
// 成功時 true, 不完全 (bufを保持) なら false。
//---------------------------------------------------------------------------
bool tTVPDAPServerThread::TryParseFramed(std::string& buf, picojson::value& out)
{
	const std::string sep = "\r\n\r\n";
	size_t header_end = buf.find(sep);
	if (header_end == std::string::npos) return false;

	// ヘッダ部分を解釈し Content-Length を取得
	std::string header = buf.substr(0, header_end);
	size_t cl_pos = header.find("Content-Length:");
	if (cl_pos == std::string::npos) {
		// 不正ヘッダ → そのフレーム分捨てて先に進む
		buf.erase(0, header_end + sep.size());
		return false;
	}
	size_t cl_value_pos = cl_pos + std::strlen("Content-Length:");
	while (cl_value_pos < header.size() && (header[cl_value_pos] == ' ' || header[cl_value_pos] == '\t'))
		cl_value_pos++;
	int content_length = std::atoi(header.c_str() + cl_value_pos);
	if (content_length <= 0 || content_length > (16 * 1024 * 1024)) {
		// 上限 16MB 防御
		buf.erase(0, header_end + sep.size());
		return false;
	}

	size_t body_start = header_end + sep.size();
	if (buf.size() < body_start + (size_t)content_length) {
		// まだ全 body 来てない
		return false;
	}

	std::string body = buf.substr(body_start, content_length);
	buf.erase(0, body_start + content_length);

	std::string err;
	picojson::parse(out, body.begin(), body.end(), &err);
	if (!err.empty()) {
		TVPAddImportantLog(ttstr(TJS_W("DAP: invalid JSON received: ")) + ttstr(err.c_str()));
		return false;
	}
	return true;
}

//---------------------------------------------------------------------------
bool tTVPDAPServerThread::DrainSocketRead(std::string& recv_buf)
{
	socket_t cs = SockOf(client_sock_ptr_);
	if (cs == INVALID_SOCK) return false;

	char tmp[4096];
	int n = ::recv(cs, tmp, sizeof(tmp), 0);
	if (n == 0) return false; // 正常切断
	if (n < 0) {
#ifdef _WIN32
		int err = LastSockError();
		if (err == WSAEWOULDBLOCK) return true;
#else
		int err = LastSockError();
		if (err == EAGAIN || err == EWOULDBLOCK) return true;
#endif
		return false;
	}
	recv_buf.append(tmp, n);

	// 切り出せるだけメッセージを取り出してキューに積む
	bool any = false;
	while (true) {
		picojson::value v;
		if (!TryParseFramed(recv_buf, v)) break;
		{
			std::lock_guard<std::mutex> lk(incoming_mtx_);
			incoming_.push_back(std::move(v));
		}
		any = true;
	}
	if (any && on_message_) on_message_();
	return true;
}

bool tTVPDAPServerThread::DrainSocketWrite()
{
	socket_t cs = SockOf(client_sock_ptr_);
	if (cs == INVALID_SOCK) return false;

	picojson::value v;
	{
		std::lock_guard<std::mutex> lk(outgoing_mtx_);
		if (outgoing_.empty()) return true;
		v = std::move(outgoing_.front());
		outgoing_.pop_front();
	}

	std::string body = v.serialize();
	std::string framed = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

	size_t sent = 0;
	while (sent < framed.size()) {
		int n = ::send(cs, framed.data() + sent, (int)(framed.size() - sent), 0);
		if (n <= 0) return false;
		sent += (size_t)n;
	}
	return true;
}

//---------------------------------------------------------------------------
// 1 クライアントとのセッションループ。
// 戻り値 true なら次の accept へ、false なら全体終了。
//---------------------------------------------------------------------------
bool tTVPDAPServerThread::ServeOneClient()
{
	connected_.store(true, std::memory_order_release);
	std::string recv_buf;

	while (!terminating_.load(std::memory_order_acquire) && !GetTerminated()) {
		socket_t cs = SockOf(client_sock_ptr_);
		if (cs == INVALID_SOCK) break;

		// outgoing がある場合 / ない場合で select の writefds を切り替え
		bool has_outgoing = false;
		{ std::lock_guard<std::mutex> lk(outgoing_mtx_); has_outgoing = !outgoing_.empty(); }

		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(cs, &rfds);
		if (has_outgoing) FD_SET(cs, &wfds);

		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 100 * 1000; // 100ms

		int sel = ::select((int)cs + 1, &rfds, has_outgoing ? &wfds : nullptr, nullptr, &tv);
		if (sel < 0) {
#ifdef _WIN32
			if (LastSockError() == WSAEINTR) continue;
#else
			if (LastSockError() == EINTR) continue;
#endif
			break;
		}

		if (FD_ISSET(cs, &rfds)) {
			if (!DrainSocketRead(recv_buf)) {
				// 切断
				return true;
			}
		}
		if (has_outgoing && FD_ISSET(cs, &wfds)) {
			if (!DrainSocketWrite()) return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------
void tTVPDAPServerThread::Execute()
{
	socket_t ls = ::socket(AF_INET, SOCK_STREAM, 0);
	if (ls == INVALID_SOCK) {
		TVPAddImportantLog(TJS_W("DAP: failed to create listen socket"));
		return;
	}
	int yes = 1;
	::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((uint16_t)port_);
#ifdef _WIN32
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
#else
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif

	if (::bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCK_ERR) {
		TVPAddImportantLog(ttstr(TJS_W("DAP: bind failed on port ")) + ttstr(port_));
		CloseSocket(ls);
		return;
	}
	if (::listen(ls, 1) == SOCK_ERR) {
		TVPAddImportantLog(TJS_W("DAP: listen failed"));
		CloseSocket(ls);
		return;
	}
	listen_sock_ptr_ = PtrOf(ls);

	TVPAddImportantLog(ttstr(TJS_W("DAP server listening on 127.0.0.1:")) + ttstr(port_));

	while (!terminating_.load(std::memory_order_acquire) && !GetTerminated()) {
		sockaddr_in caddr;
#ifdef _WIN32
		int caddr_len = sizeof(caddr);
#else
		socklen_t caddr_len = sizeof(caddr);
#endif
		socket_t cs = ::accept(ls, (sockaddr*)&caddr, &caddr_len);
		if (cs == INVALID_SOCK) {
			if (terminating_.load(std::memory_order_acquire)) break;
			continue;
		}

		// 接続受け入れ
		client_sock_ptr_ = PtrOf(cs);
		TVPAddImportantLog(TJS_W("DAP client connected"));

		bool keep = ServeOneClient();
		CloseClient();
		TVPAddImportantLog(TJS_W("DAP client disconnected"));
		if (!keep) break;
	}

	if (listen_sock_ptr_) {
		CloseSocket(SockOf(listen_sock_ptr_));
		listen_sock_ptr_ = nullptr;
	}
}

//---------------------------------------------------------------------------
// public API
//---------------------------------------------------------------------------
static tTVPDAPServerThread* g_dap_server = nullptr;

void TVPCreateDAPServer(int port, tTVPDAPServerThread::NotifyCallback on_message)
{
	if (g_dap_server) return;
	g_dap_server = new tTVPDAPServerThread(port, std::move(on_message));
}

void TVPDestroyDAPServer()
{
	if (g_dap_server) {
		delete g_dap_server;
		g_dap_server = nullptr;
	}
}

tTVPDAPServerThread* TVPGetDAPServer()
{
	return g_dap_server;
}

#endif // KRKRZ_ENABLE_DAP
