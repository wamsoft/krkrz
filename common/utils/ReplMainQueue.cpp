//---------------------------------------------------------------------------
// REPL メインスレッド実行キュー実装
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ReplMainQueue.h"
#include "ScriptMgnIntf.h"   // TVPExecuteExpression

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace {

// 提出のシリアライズ (同時に 1 件だけ in-flight)。
std::mutex g_submit_mtx;

// request slot (worker → main)
std::mutex g_req_mtx;
ttstr g_req_script;
bool g_req_pending = false;

// response slot (main → worker)
std::mutex g_resp_mtx;
std::condition_variable g_resp_cv;
tTJSVariant g_resp_result;
ttstr g_resp_error;
bool g_resp_ok = false;
bool g_resp_ready = false;

std::atomic<bool> g_terminating{false};

} // anonymous

namespace TVPReplMainQueue {

void Reset()
{
	g_terminating.store(false, std::memory_order_release);
}

bool Submit(const ttstr& script, tTJSVariant& out, ttstr& error)
{
	// 同時提出を 1 件に制限 (console と file channel が競合しても安全)。
	std::lock_guard<std::mutex> submit_lk(g_submit_mtx);

	if (g_terminating.load(std::memory_order_acquire)) return false;

	{
		std::lock_guard<std::mutex> lk(g_req_mtx);
		g_req_script = script;
		g_req_pending = true;
	}

	std::unique_lock<std::mutex> lk(g_resp_mtx);
	g_resp_cv.wait(lk, []{
		return g_resp_ready || g_terminating.load(std::memory_order_acquire);
	});

	if (g_terminating.load(std::memory_order_acquire) && !g_resp_ready) {
		return false;
	}

	out = g_resp_result;
	error = g_resp_error;
	bool ok = g_resp_ok;
	g_resp_ready = false;
	g_resp_ok = false;
	g_resp_result.Clear();
	g_resp_error.Clear();
	return ok;
}

void Drain()
{
	ttstr script;
	{
		std::lock_guard<std::mutex> lk(g_req_mtx);
		if (!g_req_pending) return;
		script = g_req_script;
		g_req_pending = false;
		g_req_script.Clear();
	}

	tTJSVariant result;
	ttstr error;
	bool ok = false;
	try {
		TVPExecuteExpression(script, &result);
		ok = true;
	} catch (eTJSScriptError& e) {
		error = ttstr(TJS_W("Error: ")) + e.GetMessage();
	} catch (eTJS& e) {
		error = ttstr(TJS_W("Error: ")) + e.GetMessage();
	} catch (...) {
		error = ttstr(TJS_W("Unknown error occurred"));
	}

	{
		std::lock_guard<std::mutex> lk(g_resp_mtx);
		g_resp_result = result;
		g_resp_error = error;
		g_resp_ok = ok;
		g_resp_ready = true;
	}
	g_resp_cv.notify_all();
}

void Shutdown()
{
	g_terminating.store(true, std::memory_order_release);
	g_resp_cv.notify_all();
}

} // namespace TVPReplMainQueue
