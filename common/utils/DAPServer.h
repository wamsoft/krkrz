//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Debug Adapter Protocol TCP server (Phase 2 minimal).
// localhost に listen し、Content-Length 付き JSON-RPC で 1 クライアントと
// 通信する。recv/send は専用ワーカースレッド、メインスレッドからは
// TryPopMessage / PostMessage / TVPDrainDAP() で叩く。
//---------------------------------------------------------------------------
#pragma once

#ifdef KRKRZ_ENABLE_DAP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include "ThreadIntf.h"

// picojson は target_include_directories の PICOJSON_INCLUDE_DIRS から見える。
// PICOJSON_USE_INT64 は CMakeLists.txt でビルド全体に定義される (TU ローカル
// define は ODR 不整合のもとなのでここでは定義しない)
#include "picojson/picojson.h"

class tTVPDAPServerThread : public tTVPThread
{
public:
	// メッセージ到着時に呼ばれるコールバック (任意スレッド)。
	// DebuggerCore はここで自前の condition_variable を notify して
	// メイン側の停止待ちループを起こす。
	using NotifyCallback = std::function<void()>;

	tTVPDAPServerThread(int port, NotifyCallback on_message);
	~tTVPDAPServerThread();

	// 受信キューから 1 件取り出す (非ブロッキング)。空なら false。
	bool TryPopMessage(picojson::value& out);

	// 送信キューに 1 件積む (任意スレッド)。
	void PostMessage(const picojson::value& msg);

	bool IsClientConnected() const { return connected_.load(std::memory_order_acquire); }

	void Shutdown();

protected:
	void Execute() override;

private:
	int port_;
	NotifyCallback on_message_;

	std::atomic<bool> terminating_{false};
	std::atomic<bool> connected_{false};

	std::mutex incoming_mtx_;
	std::deque<picojson::value> incoming_;

	std::mutex outgoing_mtx_;
	std::deque<picojson::value> outgoing_;

	// プラットフォーム独立な socket ハンドル (実装側で void* / int に解決)
	void* listen_sock_ptr_ = nullptr;
	void* client_sock_ptr_ = nullptr;

	// 内部
	bool ServeOneClient();        // accept 〜 disconnect までのループ
	bool DrainSocketRead(std::string& recv_buf);
	bool TryParseFramed(std::string& buf, picojson::value& out);
	bool DrainSocketWrite();
	void CloseClient();
};

// ----- public API -----
void TVPCreateDAPServer(int port, tTVPDAPServerThread::NotifyCallback on_message);
void TVPDestroyDAPServer();
tTVPDAPServerThread* TVPGetDAPServer();

#endif // KRKRZ_ENABLE_DAP
