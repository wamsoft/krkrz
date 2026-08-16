//---------------------------------------------------------------------------
// REPL メインスレッド実行キュー実装
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ReplMainQueue.h"
#include "ScriptMgnIntf.h"   // TVPExecuteExpression / TVPGetScriptEngine
#include "tjs.h"             // tTJS::CompileScript / iTJSBinaryStream

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace {

// 提出のシリアライズ (同時に 1 件だけ in-flight)。
std::mutex g_submit_mtx;

// 任意タスクのキュー (SubmitTask)。script slot と違い複数件を保持でき、
// Drain が予算内で複数件処理する。待機者ごとに完了通知を持つ。
struct TaskWait {
	std::mutex mu;
	std::condition_variable cv;
	bool done = false;
};
struct TaskItem {
	std::function<void()> fn;
	std::shared_ptr<TaskWait> wait;
};
std::mutex g_task_mtx;
std::deque<TaskItem> g_tasks;

// 1 フレームで処理するタスク数の上限 (フレーム占有を防ぐ)。
constexpr int kTaskBudgetPerDrain = 32;

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

bool SubmitTask(const std::function<void()>& fn)
{
	if (g_terminating.load(std::memory_order_acquire)) return false;

	auto wait = std::make_shared<TaskWait>();
	{
		std::lock_guard<std::mutex> lk(g_task_mtx);
		g_tasks.push_back(TaskItem{ fn, wait });
	}

	std::unique_lock<std::mutex> lk(wait->mu);
	wait->cv.wait(lk, [&]{
		return wait->done || g_terminating.load(std::memory_order_acquire);
	});
	return wait->done;
}

namespace { // Drain 下請け

// tTJS::CompileScript の出力を捨てるだけの null ストリーム (パース判定用)。
class tNullBinaryStream : public iTJSBinaryStream {
public:
	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) override { return 0; }
	tjs_uint TJS_INTF_METHOD Read(void* buffer, tjs_uint read_size) override { return 0; }
	tjs_uint TJS_INTF_METHOD Write(const void* buffer, tjs_uint write_size) override { return write_size; }
	void TJS_INTF_METHOD SetEndOfStorage() override {}
	tjs_uint64 TJS_INTF_METHOD GetSize() override { return 0; }
	void TJS_INTF_METHOD Destruct() override {}   // スタック上で使うため何もしない
};

// script が「式」としてパース可能かを、実行せずにコンパイルだけで判定する。
bool ParsesAsExpression(const ttstr& script)
{
	tTJS* engine = TVPGetScriptEngine();
	if (!engine) return true;   // 判定不能なら従来どおり式として扱う
	tNullBinaryStream sink;
	try {
		engine->CompileScript(script.c_str(), &sink,
		                      true /*isresultneeded*/, false /*outputdebug*/,
		                      true /*isexpression*/);
		return true;
	} catch (...) {
		return false;
	}
}

// タスクを予算内で処理 (script slot の後に呼ばれる)。
void DrainTasks()
{
	for (int i = 0; i < kTaskBudgetPerDrain; ++i) {
		TaskItem item;
		{
			std::lock_guard<std::mutex> lk(g_task_mtx);
			if (g_tasks.empty()) return;
			item = std::move(g_tasks.front());
			g_tasks.pop_front();
		}
		// fn 内の例外はフレームループへ漏らさない (エラーは fn 側で結果に反映する契約)。
		try { if (item.fn) item.fn(); } catch (...) {}
		{
			std::lock_guard<std::mutex> lk(item.wait->mu);
			item.wait->done = true;
		}
		item.wait->cv.notify_all();
	}
}

} // anonymous

void Drain()
{
	DrainTasks();

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
	// 「式としてパース可能か」を実行せずに事前判定してから、式 or 文の
	// どちらか一方だけを実行する。以前は「式として評価 → 例外なら文として
	// 再実行」というフォールバックだったため、
	//   - 式の実行時例外 (メンバ無し/引数不正等) でも文として再パースされ、
	//     末尾 ';' 無しの入力が文法エラー扱いになり実際の例外メッセージが
	//     「文法エラーです(syntax error)」に化ける
	//   - 副作用のある式が途中まで実行された後もう一度実行される
	// という問題があった。
	if (ParsesAsExpression(script)) {
		try {
			// 式として評価 (結果を表示できる)。実行時例外はそのまま報告する。
			TVPExecuteExpression(script, &result);
			ok = true;
		} catch (eTJSScriptError& e) {
			error = ttstr(TJS_W("Error: ")) + e.GetMessage();
		} catch (eTJS& e) {
			error = ttstr(TJS_W("Error: ")) + e.GetMessage();
		} catch (...) {
			error = ttstr(TJS_W("Unknown error occurred"));
		}
	} else {
		// 式でない入力は文 (statement) として実行する。
		// for / if / while / var / function 宣言や複数文をまとめて実行できる。
		try {
			TVPExecuteScript(script, &result);
			ok = true;
		} catch (eTJSScriptError& e) {
			error = ttstr(TJS_W("Error: ")) + e.GetMessage();
		} catch (eTJS& e) {
			error = ttstr(TJS_W("Error: ")) + e.GetMessage();
		} catch (...) {
			error = ttstr(TJS_W("Unknown error occurred"));
		}
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
	// キューに残ったタスクの待機者を起こす (done=false のまま → SubmitTask は false を返す)。
	std::deque<TaskItem> leftover;
	{
		std::lock_guard<std::mutex> lk(g_task_mtx);
		leftover.swap(g_tasks);
	}
	for (auto& item : leftover) {
		{ std::lock_guard<std::mutex> lk(item.wait->mu); }
		item.wait->cv.notify_all();
	}
}

} // namespace TVPReplMainQueue
