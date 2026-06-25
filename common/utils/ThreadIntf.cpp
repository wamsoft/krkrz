//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Thread base class
//---------------------------------------------------------------------------
#define NOMINMAX
#include "tjsCommHead.h"

#include "ThreadIntf.h"
#include "MsgIntf.h"

#include <algorithm>
#include <assert.h>
#include <system_error>
#include <chrono>

//---------------------------------------------------------------------------
// DrawThreadPool 利用統計 (KRKRZ_DRAW_STATS で ON)
// Switch 等で「メイン CPU だけ詰まって他コア空き」現象の分析用。
// OFF ビルドでは全マクロが空展開され、atomic 操作も生成されない。
//---------------------------------------------------------------------------
#ifdef KRKRZ_DRAW_STATS
namespace {
	struct DrawThreadStats {
		std::atomic<tjs_uint64> begin_count{0};
		std::atomic<tjs_uint64> task_hist[TVPMaxThreadNum + 1]{};
		std::atomic<tjs_uint64> worker_active_ns{0};
		std::atomic<tjs_uint64> main_active_ns{0};
		std::atomic<tjs_uint64> wait_spin_ns{0};
	};
	static DrawThreadStats g_draw_stats;

	// callsite テーブル: open-addressing。site は文字列リテラルポインタ (literal は
	// プロセス寿命中安定なのでそのまま比較キーに使える)。容量は 32 (= TVPDrawCallsiteMax)、
	// 全コードベースで TVPBeginThreadTask が呼ばれている箇所は ~15 程度なので余裕。
	struct CallsiteSlot {
		std::atomic<const char *> site{nullptr};
		std::atomic<tjs_uint64>   count{0};
		std::atomic<tjs_uint64>   t1_count{0};
	};
	static CallsiteSlot g_callsites[TVPDrawCallsiteMax];

	inline tjs_uint64 NowNs() {
		using namespace std::chrono;
		return (tjs_uint64)duration_cast<nanoseconds>(
			steady_clock::now().time_since_epoch()).count();
	}

	inline void RecordCallsite(const char *site, tjs_int taskNum) {
		if (!site) return;
		// 文字列リテラルアドレスをハッシュキーに使う。アドレス下位 4bit はアラインで偏るので右シフト。
		size_t h = ((size_t)site) >> 4;
		for (int i = 0; i < TVPDrawCallsiteMax; ++i) {
			size_t idx = (h + i) & (TVPDrawCallsiteMax - 1);
			const char *k = g_callsites[idx].site.load(std::memory_order_acquire);
			if (k == site) {
				g_callsites[idx].count.fetch_add(1, std::memory_order_relaxed);
				if (taskNum == 1) g_callsites[idx].t1_count.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (k == nullptr) {
				const char *expected = nullptr;
				if (g_callsites[idx].site.compare_exchange_strong(
						expected, site,
						std::memory_order_release, std::memory_order_acquire)) {
					g_callsites[idx].count.fetch_add(1, std::memory_order_relaxed);
					if (taskNum == 1) g_callsites[idx].t1_count.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				// CAS 失敗時は expected に他者書き込みが入っている。同 site なら加算、異なれば次 slot へ
				if (expected == site) {
					g_callsites[idx].count.fetch_add(1, std::memory_order_relaxed);
					if (taskNum == 1) g_callsites[idx].t1_count.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
		}
		// 全 slot 埋まり: 黙って drop (callsite 数 > 32 は想定外)。
	}
}
#define DRAWSTATS_BEGIN(taskNum)                                              \
	do {                                                                      \
		g_draw_stats.begin_count.fetch_add(1, std::memory_order_relaxed);     \
		int _idx = (taskNum);                                                 \
		if (_idx < 0) _idx = 0;                                               \
		if (_idx > TVPMaxThreadNum) _idx = TVPMaxThreadNum;                   \
		g_draw_stats.task_hist[_idx].fetch_add(1, std::memory_order_relaxed); \
	} while(0)
#define DRAWSTATS_T0()        tjs_uint64 _ds_t0 = NowNs()
#define DRAWSTATS_ADD(field)  g_draw_stats.field.fetch_add(NowNs() - _ds_t0, std::memory_order_relaxed)
#else
#define DRAWSTATS_BEGIN(taskNum)  ((void)0)
#define DRAWSTATS_T0()            ((void)0)
#define DRAWSTATS_ADD(field)      ((void)0)
#endif

// TJS から制御する log 出力フラグ。memoverlay の 500ms tick で参照される。
bool TVPDrawStatsLogEnabled = false;

void TVPGetDrawThreadStats(TVPDrawThreadStatsSnapshot &out)
{
	using namespace std::chrono;
	out.snapshot_tick_ms = (tjs_uint64)duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
#ifdef KRKRZ_DRAW_STATS
	out.begin_count = g_draw_stats.begin_count.load(std::memory_order_relaxed);
	for (int i = 0; i <= TVPMaxThreadNum; ++i) {
		out.task_hist[i] = g_draw_stats.task_hist[i].load(std::memory_order_relaxed);
	}
	out.worker_active_ns = g_draw_stats.worker_active_ns.load(std::memory_order_relaxed);
	out.main_active_ns   = g_draw_stats.main_active_ns.load(std::memory_order_relaxed);
	out.wait_spin_ns     = g_draw_stats.wait_spin_ns.load(std::memory_order_relaxed);
#else
	out.begin_count = 0;
	for (int i = 0; i <= TVPMaxThreadNum; ++i) out.task_hist[i] = 0;
	out.worker_active_ns = 0;
	out.main_active_ns   = 0;
	out.wait_spin_ns     = 0;
#endif
}

// SDL3 テクスチャ更新計測 (KRKRZ_DRAW_STATS=ON のときだけ atomic 操作が生成される)。
#ifdef KRKRZ_DRAW_STATS
namespace {
	struct RenderStats {
		std::atomic<tjs_uint64> tex_update_ns{0};
		std::atomic<tjs_uint64> tex_render_ns{0};
		std::atomic<tjs_uint64> tex_bytes{0};
		std::atomic<tjs_uint64> tex_rect_count{0};
		std::atomic<tjs_uint64> frame_count{0};
		std::atomic<tjs_uint64> show_clear_ns{0};
		std::atomic<tjs_uint64> show_tex_ns{0};
		std::atomic<tjs_uint64> show_overlay_ns{0};
		std::atomic<tjs_uint64> show_present_ns{0};
		std::atomic<tjs_uint64> frame_update_ns{0};
		std::atomic<tjs_uint64> frame_show_ns{0};
		std::atomic<tjs_uint64> frame_dispatch_ns{0};
		std::atomic<tjs_uint64> layer_complete_ns{0};
		std::atomic<tjs_uint64> layer_draw_ns{0};
		std::atomic<tjs_uint64> layer_complete_window_ns{0};
		std::atomic<tjs_uint64> layer_before_completion_ns{0};
		std::atomic<tjs_uint64> layer_after_completion_ns{0};
	};
	static RenderStats g_render_stats;
}
#endif

void TVPRenderStatsAddTexUpdate(tjs_uint64 ns, tjs_uint64 bytes)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.tex_update_ns.fetch_add(ns, std::memory_order_relaxed);
	g_render_stats.tex_bytes.fetch_add(bytes, std::memory_order_relaxed);
	g_render_stats.tex_rect_count.fetch_add(1, std::memory_order_relaxed);
#else
	(void)ns; (void)bytes;
#endif
}

void TVPRenderStatsAddTexRender(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.tex_render_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsBumpFrame()
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.frame_count.fetch_add(1, std::memory_order_relaxed);
#endif
}

void TVPRenderStatsAddShowClear(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.show_clear_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddShowTex(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.show_tex_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddShowOverlay(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.show_overlay_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddShowPresent(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.show_present_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddFrameUpdate(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.frame_update_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddFrameShow(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.frame_show_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddFrameDispatch(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.frame_dispatch_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddLayerComplete(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.layer_complete_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddLayerDraw(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.layer_draw_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddLayerCompleteWindow(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.layer_complete_window_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddLayerBeforeCompletion(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.layer_before_completion_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPRenderStatsAddLayerAfterCompletion(tjs_uint64 ns)
{
#ifdef KRKRZ_DRAW_STATS
	g_render_stats.layer_after_completion_ns.fetch_add(ns, std::memory_order_relaxed);
#else
	(void)ns;
#endif
}

void TVPGetRenderStats(TVPRenderStatsSnapshot &out)
{
	using namespace std::chrono;
	out.snapshot_tick_ms = (tjs_uint64)duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
#ifdef KRKRZ_DRAW_STATS
	out.tex_update_ns      = g_render_stats.tex_update_ns.load(std::memory_order_relaxed);
	out.tex_render_ns      = g_render_stats.tex_render_ns.load(std::memory_order_relaxed);
	out.tex_bytes          = g_render_stats.tex_bytes.load(std::memory_order_relaxed);
	out.tex_rect_count     = g_render_stats.tex_rect_count.load(std::memory_order_relaxed);
	out.frame_count        = g_render_stats.frame_count.load(std::memory_order_relaxed);
	out.show_clear_ns      = g_render_stats.show_clear_ns.load(std::memory_order_relaxed);
	out.show_tex_ns        = g_render_stats.show_tex_ns.load(std::memory_order_relaxed);
	out.show_overlay_ns    = g_render_stats.show_overlay_ns.load(std::memory_order_relaxed);
	out.show_present_ns    = g_render_stats.show_present_ns.load(std::memory_order_relaxed);
	out.frame_update_ns    = g_render_stats.frame_update_ns.load(std::memory_order_relaxed);
	out.frame_show_ns      = g_render_stats.frame_show_ns.load(std::memory_order_relaxed);
	out.frame_dispatch_ns  = g_render_stats.frame_dispatch_ns.load(std::memory_order_relaxed);
	out.layer_complete_ns         = g_render_stats.layer_complete_ns.load(std::memory_order_relaxed);
	out.layer_draw_ns             = g_render_stats.layer_draw_ns.load(std::memory_order_relaxed);
	out.layer_complete_window_ns    = g_render_stats.layer_complete_window_ns.load(std::memory_order_relaxed);
	out.layer_before_completion_ns  = g_render_stats.layer_before_completion_ns.load(std::memory_order_relaxed);
	out.layer_after_completion_ns   = g_render_stats.layer_after_completion_ns.load(std::memory_order_relaxed);
#else
	out.tex_update_ns      = 0;
	out.tex_render_ns      = 0;
	out.tex_bytes          = 0;
	out.tex_rect_count     = 0;
	out.frame_count        = 0;
	out.show_clear_ns      = 0;
	out.show_tex_ns        = 0;
	out.show_overlay_ns    = 0;
	out.show_present_ns    = 0;
	out.frame_update_ns    = 0;
	out.frame_show_ns      = 0;
	out.frame_dispatch_ns  = 0;
	out.layer_complete_ns         = 0;
	out.layer_draw_ns             = 0;
	out.layer_complete_window_ns    = 0;
	out.layer_before_completion_ns  = 0;
	out.layer_after_completion_ns   = 0;
#endif
}

void TVPGetDrawCallsiteSnapshots(TVPDrawCallsiteSnapshot out[TVPDrawCallsiteMax])
{
#ifdef KRKRZ_DRAW_STATS
	for (int i = 0; i < TVPDrawCallsiteMax; ++i) {
		out[i].site     = g_callsites[i].site.load(std::memory_order_relaxed);
		out[i].count    = g_callsites[i].count.load(std::memory_order_relaxed);
		out[i].t1_count = g_callsites[i].t1_count.load(std::memory_order_relaxed);
	}
#else
	for (int i = 0; i < TVPDrawCallsiteMax; ++i) {
		out[i].site = nullptr;
		out[i].count = 0;
		out[i].t1_count = 0;
	}
#endif
}

//---------------------------------------------------------------------------
// tTVPThread : a wrapper class for thread
//---------------------------------------------------------------------------
tTVPThread::tTVPThread(const char *name)
 : Thread(nullptr), Terminated(false), ThreadName(name), ThreadStarting(false), ThreadPriority(ttpNormal)
{
	Thread = TVPCreateNativeThread();
}

//---------------------------------------------------------------------------
tTVPThread::~tTVPThread()
{
	delete Thread;
}
//---------------------------------------------------------------------------

void tTVPThread::StartFunc(void *arg)
{
	tTVPThread *thread = (tTVPThread*)arg;
	thread->StartProc();
}

void tTVPThread::StartProc()
{
	{	// スレッドが開始されたのでフラグON
		std::lock_guard<std::mutex> lock( Mtx );
		ThreadStarting = true;
	}
	Cond.notify_all();
	Execute();
	// return 0;
}

//---------------------------------------------------------------------------
void tTVPThread::StartThread()
{
	if (Thread) {
		try {
			Thread->Start(StartFunc, (void*)this, ThreadPriority, ThreadName);
			// スレッドが開始されるのを待つ
			std::unique_lock<std::mutex> lock( Mtx );
			Cond.wait( lock, [this] { return ThreadStarting; } );
		} catch( std::system_error & ) {
			TVPThrowInternalError;
		}
	}

}

//---------------------------------------------------------------------------
void tTVPThread::WaitFor() 
{
	if (Thread) {
		Thread->WaitFor();
	}
}

//---------------------------------------------------------------------------
tTVPThreadPriority tTVPThread::GetPriority()
{
	return ThreadPriority;
}

//---------------------------------------------------------------------------
void tTVPThread::SetPriority(tTVPThreadPriority pri)
{
	ThreadPriority = pri;
	if (!ThreadStarting) {
		return;
	}
	if (Thread) {
		Thread->SetPriority(pri);
	}
}

void
tTVPThread::SetProcessorNo(int no)
{
	if (Thread) {
		Thread->SetProcessorNo(no);
	}
}

#ifdef _WIN32
HANDLE
tTVPThread::GetHandle() const {
	return Thread ? Thread->GetHandle() : nullptr;
}
#endif

//---------------------------------------------------------------------------
tjs_int TVPDrawThreadNum = 1;
//---------------------------------------------------------------------------
extern tjs_int TVPGetProcessorNum( void );

//---------------------------------------------------------------------------
tjs_int TVPGetThreadNum( void )
{
	tjs_int threadNum = TVPDrawThreadNum ? TVPDrawThreadNum : TVPGetProcessorNum();
	threadNum = std::min( threadNum, TVPMaxThreadNum );
	return threadNum;
}
//---------------------------------------------------------------------------
static void TJS_USERENTRY DummyThreadTask( void * ) {}
//---------------------------------------------------------------------------
class DrawThreadPool;
class DrawThread : public tTVPThread {
	std::mutex mtx;
	std::condition_variable cv;
	TVP_THREAD_TASK_FUNC  lpStartAddress;
	TVP_THREAD_PARAM lpParameter;
	DrawThreadPool* parent;
protected:
	virtual void Execute();

public:
	DrawThread( DrawThreadPool* p ) : tTVPThread("DrawThread"), parent( p ), lpStartAddress( nullptr ), lpParameter( nullptr ) {}
	void SetTask( TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param ) {
		std::lock_guard<std::mutex> lock( mtx );
		lpStartAddress = func;
		lpParameter = param;
		cv.notify_one();
	}
};
//---------------------------------------------------------------------------
class DrawThreadPool {
	std::vector<DrawThread*> workers;
	std::atomic<int> running_thread_count;
	tjs_int task_num;
	tjs_int task_count;
	// 旧実装は busy-spin + yield で全 worker 完了を待っていた (main CPU を 100% 占有)。
	// Switch 実機計測で Spin が 30-40 ms/s を消費していたため、condition_variable に変更:
	// - 最後の worker が DecCount で counter==0 にしたとき notify
	// - main は WaitForTask で fast-path (counter==0 即 return) → cv.wait
	// 副作用は完了時の lock+notify 1 回 + main の wait 時の lock+wait のみで、
	// fast-path (= WaitForTask 到達時に既に完了) ではロック取らない。
	std::mutex                completion_mtx;
	std::condition_variable   completion_cv;
private:
	void PoolThread( tjs_int taskNum );

public:
	DrawThreadPool() : running_thread_count( 0 ), task_num( 0 ), task_count( 0 ) {}
	~DrawThreadPool() {
		for( auto i = workers.begin(); i != workers.end(); ++i ) {
			DrawThread *th = *i;
			th->Terminate();
			th->SetTask( DummyThreadTask, nullptr );
			th->WaitFor();
			delete th;
		}
	}
	inline void DecCount() {
		// fetch_sub の prev (この decrement 前の値) が 1 のときだけ "我々が最後だった"。
		// その瞬間にだけ lock+notify を取る。それ以外の decrement では一切ロック取らない。
		int prev = running_thread_count.fetch_sub(1, std::memory_order_acq_rel);
		if (prev == 1) {
			std::lock_guard<std::mutex> lock(completion_mtx);
			completion_cv.notify_all();
		}
	}
	void BeginTask( tjs_int taskNum, const char *site ) {
		(void)site; // OFF ビルドでは未使用
		task_num = taskNum;
		task_count = 0;
		PoolThread( taskNum );
		DRAWSTATS_BEGIN(taskNum);
#ifdef KRKRZ_DRAW_STATS
		if (site) RecordCallsite(site, taskNum);
#endif
	}
	void ExecTask( TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param ) {
		if( task_count >= task_num - 1 ) {
			// 最後の 1 task はメインスレッドが直接実行 (= main_active_ns に計上)
			DRAWSTATS_T0();
			func( param );
			DRAWSTATS_ADD(main_active_ns);
			return;
		}
		running_thread_count.fetch_add(1, std::memory_order_release);
		DrawThread* thread = workers[task_count];
		task_count++;
		thread->SetTask( func, param );
		TVPYieldNativeThread();
	}
	void WaitForTask() {
		DRAWSTATS_T0();
		// fast-path: メインが直接実行する最後のタスクが終わってここに来た時点で
		// 既に他 worker が全部完了している場合は lock を取らずに即抜ける。
		if (running_thread_count.load(std::memory_order_acquire) != 0) {
			std::unique_lock<std::mutex> lock(completion_mtx);
			completion_cv.wait(lock, [this] {
				return running_thread_count.load(std::memory_order_acquire) == 0;
			});
		}
		DRAWSTATS_ADD(wait_spin_ns);
	}
};
//---------------------------------------------------------------------------
void DrawThread::Execute() {
	while( !GetTerminated() ) {
		{
			std::unique_lock<std::mutex> uniq_lk( mtx );
			cv.wait( uniq_lk, [this] { return lpStartAddress != nullptr; } );
		}
		if( lpStartAddress != nullptr ) {
			// worker active 区間を計上
			DRAWSTATS_T0();
			( lpStartAddress )( lpParameter );
			DRAWSTATS_ADD(worker_active_ns);
		}
		lpStartAddress = nullptr;
		parent->DecCount();
	}
}
//---------------------------------------------------------------------------
void DrawThreadPool::PoolThread( tjs_int taskNum ) {
	tjs_int extraThreadNum = TVPGetThreadNum();


	// スレッド数がextraThreadNumに達していないので(suspend状態で)生成する
	while( (tjs_int)workers.size() < extraThreadNum ) {
		DrawThread* th = new DrawThread( this );
		th->StartThread();
		// プロセッサ番号指定
		th->SetProcessorNo(workers.size()+1);
		workers.push_back( th );
	}
	// スレッド数が多い場合終了させる
	while( (tjs_int)workers.size() > extraThreadNum ) {
		DrawThread *th = workers.back();
		th->Terminate();
		running_thread_count++;
		th->SetTask( DummyThreadTask, nullptr );
		th->WaitFor();
		workers.pop_back();
		delete th;
	}
}
//---------------------------------------------------------------------------
static DrawThreadPool TVPTheadPool;
//---------------------------------------------------------------------------
// マクロ override (ThreadIntf.h で定義) を一旦解除して関数本体を書く。
// 本ファイル内の TVPBeginThreadTask 呼出は無いので副作用なし。
#ifdef TVPBeginThreadTask
#undef TVPBeginThreadTask
#endif
void TVPBeginThreadTask( tjs_int taskNum ) {
	TVPTheadPool.BeginTask( taskNum, nullptr );
}
//---------------------------------------------------------------------------
void TVPBeginThreadTaskAt( tjs_int taskNum, const char *site ) {
	TVPTheadPool.BeginTask( taskNum, site );
}
//---------------------------------------------------------------------------
void TVPExecThreadTask( TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param ) {
	TVPTheadPool.ExecTask( func, param );
}
//---------------------------------------------------------------------------
void TVPEndThreadTask( void ) {
	TVPTheadPool.WaitForTask();
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------


