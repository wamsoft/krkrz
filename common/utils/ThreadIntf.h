//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Thread base class
//---------------------------------------------------------------------------
#ifndef ThreadIntfH
#define ThreadIntfH
#include "tjsNative.h"

#include <condition_variable>
#include <mutex>
#include <atomic>

/*[*/

//---------------------------------------------------------------------------
// tTVPThreadPriority
//---------------------------------------------------------------------------
enum tTVPThreadPriority
{
	ttpIdle, ttpLowest, ttpLower, ttpNormal, ttpHigher, ttpHighest, ttpTimeCritical
};
//---------------------------------------------------------------------------

typedef void (*tTVPThreadFunc)(void *arg);

//---------------------------------------------------------------------------
// Native Thread Wrapper
//---------------------------------------------------------------------------

class tTVPNativeThreadIntf
{
public:
	virtual ~tTVPNativeThreadIntf() {};
	virtual void Start(tTVPThreadFunc func, void *arg, tTVPThreadPriority pri, const char *name) = 0;
	virtual void WaitFor() = 0;
	virtual void SetPriority(tTVPThreadPriority pri) = 0;
	virtual void SetProcessorNo(int no) = 0;
#ifdef _WIN32
	virtual HANDLE GetHandle() const = 0; /* win32 specific */
#endif
};

/*]*/

TJS_EXP_FUNC_DEF(void, TVPYieldNativeThread, (int Millisecontds=0));
TJS_EXP_FUNC_DEF(tTVPNativeThreadIntf*, TVPCreateNativeThread, ());


//---------------------------------------------------------------------------
// tTVPThread
//---------------------------------------------------------------------------
class tTVPThread
{

private:
	tTVPNativeThreadIntf *Thread;
	bool Terminated;
	const char *ThreadName;

	std::mutex Mtx;
	std::condition_variable Cond;
	bool ThreadStarting;
	tTVPThreadPriority ThreadPriority;

public:
	static void StartFunc(void *arg);
	void StartProc();

public:
	explicit tTVPThread(const char *name);
	virtual ~tTVPThread();

	bool GetTerminated() const { return Terminated; }
	void SetTerminated(bool s) { Terminated = s; }
	void Terminate() { Terminated = true; }

protected:
	virtual void Execute() {}

public:
	void StartThread();
	void WaitFor();

	tTVPThreadPriority GetPriority();
	void SetPriority(tTVPThreadPriority pri);

	void SetProcessorNo(int no);

#ifdef _WIN32
	HANDLE GetHandle() const; /* win32 specific */
#endif
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTVPThreadEvent
//---------------------------------------------------------------------------
class tTVPThreadEvent
{
	std::mutex Mtx;
	std::condition_variable Cond;
	bool IsReady;

public:
	tTVPThreadEvent() : IsReady(false) {}
	virtual ~tTVPThreadEvent() {}

	void Set() {
		{
			std::lock_guard<std::mutex> lock(Mtx);
			IsReady = true;
		}
		Cond.notify_all();
	}
	/*
	void Reset() {
		std::lock_guard<std::mutex> lock(Mtx);
		IsReady = false;
	}
	*/
	bool WaitFor( tjs_uint timeout ) {
		std::unique_lock<std::mutex> lk( Mtx );
		if( timeout == 0 ) {
			Cond.wait( lk, [this]{ return IsReady;} );
			IsReady = false;
			return true;
		} else {
			//std::cv_status result = Cond.wait_for( lk, std::chrono::milliseconds( timeout ) );
			//return result == std::cv_status::no_timeout;
			bool result = Cond.wait_for( lk, std::chrono::milliseconds( timeout ), [this]{ return IsReady;} );
			IsReady = false;
			return result;
		}
	}
};
//---------------------------------------------------------------------------


// 適応スレッド数判定 (GetAdaptiveThreadNum 等) の pixel 数閾値乗数。
// "pixelNum >= factor * KRKRZ_THREAD_PIXEL_SCALE" で threading 化される。
// CMake -DKRKRZ_THREAD_PIXEL_SCALE=N で上書き。低い値ほど積極的に threading 化される。
// デフォルト 100 (Switch 実機 500/200/100 比較で決定、2026-05-08)。
// 旧挙動 (PC で無難) は 500。
#ifndef KRKRZ_THREAD_PIXEL_SCALE
#define KRKRZ_THREAD_PIXEL_SCALE 100
#endif

/*[*/
const tjs_int TVPMaxThreadNum = 64;
typedef void (TJS_USERENTRY *TVP_THREAD_TASK_FUNC)(void *);
typedef void * TVP_THREAD_PARAM;
/*]*/

TJS_EXP_FUNC_DEF(tjs_int, TVPGetProcessorNum, ());
TJS_EXP_FUNC_DEF(tjs_int, TVPGetThreadNum, ());
TJS_EXP_FUNC_DEF(void, TVPBeginThreadTask, (tjs_int num));
// callsite (= __FILE__ ":" __LINE__) を渡す版。KRKRZ_DRAW_STATS=ON では
// 下のマクロで TVPBeginThreadTask 呼び出しがこちらに置き換わり、site が記録される。
// 非 plugin 内部 API なので makestub を要さないよう extern のまま (TJS_EXP_FUNC_DEF にしない)。
extern void TVPBeginThreadTaskAt(tjs_int num, const char *site);
TJS_EXP_FUNC_DEF(void, TVPExecThreadTask, (TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param));
TJS_EXP_FUNC_DEF(void, TVPEndThreadTask, ());

#ifdef KRKRZ_DRAW_STATS
// 既存の TVPBeginThreadTask(num) 呼び出しを TVPBeginThreadTaskAt(num, "<file>:<line>") に
// 透過的に置換するマクロ。OFF ビルドではマクロが定義されず、関数アドレスを取る側にも
// 影響しない (ただし現コードベースで &TVPBeginThreadTask の利用は無い)。
#define KRKRZ_DRAW_STATS_STR2_(x) #x
#define KRKRZ_DRAW_STATS_STR_(x) KRKRZ_DRAW_STATS_STR2_(x)
#define TVPBeginThreadTask(num) TVPBeginThreadTaskAt((num), __FILE__ ":" KRKRZ_DRAW_STATS_STR_(__LINE__))
#endif

// DrawThreadPool 利用統計のスナップショット。
// KRKRZ_DRAW_STATS が ON のときに値が入る。OFF 時は全フィールド 0 を返す。
// 累計値なので、呼び出し側で前回 snapshot との delta を取って 1 秒換算する想定。
struct TVPDrawThreadStatsSnapshot {
	tjs_uint64 begin_count;             // BeginTask 累計
	tjs_uint64 task_hist[TVPMaxThreadNum + 1]; // taskNum=k で何回 Begin したか (k=0..N)
	tjs_uint64 worker_active_ns;        // worker (= main 直接実行ぶんを除く) アクティブ時間累計
	tjs_uint64 main_active_ns;          // main が最後の 1 task を直接実行していた時間累計
	tjs_uint64 wait_spin_ns;            // WaitForTask で main がスピン待ちしていた時間累計
	tjs_uint64 snapshot_tick_ms;        // Snapshot 時点のシステムティック (ms)
};
void TVPGetDrawThreadStats(TVPDrawThreadStatsSnapshot &out);

// 1 つの callsite の累計値。slot.site == nullptr のスロットは未使用。
// site は __FILE__ ":" __LINE__ の文字列リテラル先頭ポインタ (寿命はプロセス全体)。
struct TVPDrawCallsiteSnapshot {
	const char *site;
	tjs_uint64 count;     // BeginThreadTaskAt 累計呼出回数
	tjs_uint64 t1_count;  // うち taskNum=1 で来た回数
};
const int TVPDrawCallsiteMax = 32;
void TVPGetDrawCallsiteSnapshots(TVPDrawCallsiteSnapshot out[TVPDrawCallsiteMax]);

// DrawStats を memoverlay の 500ms 更新タイミングで TVPAddLog にも書き出すフラグ。
// TJS から System.setDrawStatsLog(true/false) で切替。KRKRZ_DRAW_STATS=OFF ビルドでは
// 何も起きない (フラグ自体は存在するので TJS 側コードの互換性は保たれる)。
// オーバレイが有効なときに polling されるので、log 取得には memoverlay も ON が必要。
extern bool TVPDrawStatsLogEnabled;

// SDL3 のテクスチャ更新パス (= NotifyBitmapCompleted → SDL_LockTexture/memcpy 系) の
// 計測値。DrawThreadPool 外で main CPU を消費する経路の可視化用。
// α フェード等で画面全体 dirty のときが特にコスト大。KRKRZ_DRAW_STATS=ON で蓄積。
struct TVPRenderStatsSnapshot {
	tjs_uint64 tex_update_ns;     // SDLTextureUpdateRect::Update の累計時間 (src→中間 memcpy)
	tjs_uint64 tex_render_ns;     // SDLTextureUpdateRect::RenderToTexture の累計時間 (中間→GPU memcpy)
	tjs_uint64 tex_bytes;         // 累計コピーバイト数 (Update 側、片方向ぶんで概算)
	tjs_uint64 tex_rect_count;    // 累計 dirty rect 数 (Update 呼出回数)
	tjs_uint64 frame_count;       // RenderToTexture 呼出回数 (= 大体フレーム数)
	// Show() 内 section 別計測 (DrawThreadPool 外 + テクスチャ転送外で消えてる時間の正体探索用)
	tjs_uint64 show_clear_ns;     // SDL_RenderClear (+ logical presentation / draw color setup)
	tjs_uint64 show_tex_ns;       // SDL_RenderTextureRotated (本体描画) もしくは ShowVideo の RenderTexture
	tjs_uint64 show_overlay_ns;   // TVPRenderMemoryOverlay
	tjs_uint64 show_present_ns;   // SDL_RenderPresent (vsync 待ち + flush)
	// Frame phase 計測 (1 frame の main core 占有を Update / Show / Dispatch で 3 分割)。
	// Update は Layer 合成 (TexUp/TexRen を含む)、Show は GPU 描画 (Clr/Tex/Ovl/Pres を含む)、
	// Dispatch は event/scenario engine。3 つの差分から「未計測の合成パイプライン処理」が見える。
	tjs_uint64 frame_update_ns;   // tTVPDrawDevice::Update 全体時間
	tjs_uint64 frame_show_ns;     // tTVPDrawDevice::Show 全体時間
	tjs_uint64 frame_dispatch_ns; // tTVPApplication::Dispatch 全体時間
	// Layer 合成パイプライン phase (Phase 7、未計測 ~200 ms/s の正体探索)。
	// Update から InternalComplete2 (top-level only、再帰除外) までの差で
	// LayerManager 周辺 overhead が見える。Draw は再帰込み累計で、Layer 木 traversal
	// と各 Layer の Blt/Fill 呼び出しを包括する。
	tjs_uint64 layer_complete_ns;        // tTJSNI_BaseLayer::InternalComplete2 top-level only
	tjs_uint64 layer_draw_ns;            // tTJSNI_BaseLayer::Draw 累計 (再帰込み)
	// Phase 8: CompleteForWindow 全体時間 (= BeforeCompletion + NotifyUpdateRegionFixed +
	// StartBitmapCompletion + InternalComplete2 + EndBitmapCompletion + AfterCompletion)。
	// Update - layer_complete_window_ns ≈ UpdateToDrawDevice overhead で、ほぼゼロのはず。
	// layer_complete_window_ns - layer_complete_ns で「InternalComplete2 の外」の合計時間が見える。
	tjs_uint64 layer_complete_window_ns; // tTJSNI_BaseLayer::CompleteForWindow 全体
	// Phase 9: CompleteForWindow - InternalComplete2 の中身を Before/After に分けて計測。
	// 両方 Layer 木全体に再帰呼び出しされるので、thread_local depth で top-level only にする。
	tjs_uint64 layer_before_completion_ns; // tTJSNI_BaseLayer::BeforeCompletion top-level only
	tjs_uint64 layer_after_completion_ns;  // tTJSNI_BaseLayer::AfterCompletion top-level only
	tjs_uint64 snapshot_tick_ms;
};
void TVPGetRenderStats(TVPRenderStatsSnapshot &out);
// instrumentation 側からのカウンタ操作 API (KRKRZ_DRAW_STATS=OFF では no-op)。
void TVPRenderStatsAddTexUpdate(tjs_uint64 ns, tjs_uint64 bytes);
void TVPRenderStatsAddTexRender(tjs_uint64 ns);
void TVPRenderStatsBumpFrame();
void TVPRenderStatsAddShowClear(tjs_uint64 ns);
void TVPRenderStatsAddShowTex(tjs_uint64 ns);
void TVPRenderStatsAddShowOverlay(tjs_uint64 ns);
void TVPRenderStatsAddShowPresent(tjs_uint64 ns);
void TVPRenderStatsAddFrameUpdate(tjs_uint64 ns);
void TVPRenderStatsAddFrameShow(tjs_uint64 ns);
void TVPRenderStatsAddFrameDispatch(tjs_uint64 ns);
void TVPRenderStatsAddLayerComplete(tjs_uint64 ns);
void TVPRenderStatsAddLayerDraw(tjs_uint64 ns);
void TVPRenderStatsAddLayerCompleteWindow(tjs_uint64 ns);
void TVPRenderStatsAddLayerBeforeCompletion(tjs_uint64 ns);
void TVPRenderStatsAddLayerAfterCompletion(tjs_uint64 ns);

// 任意の scope の経過時間を destructor で adder に渡す簡易 timer。
// 上の TVPRenderStatsAdd... 系 API を引数に取る形で使う。
// 例:
//   { TVPRenderStatsScopedTimer _t(TVPRenderStatsAddFrameUpdate); ... }
#ifdef KRKRZ_DRAW_STATS
#include <chrono>
class TVPRenderStatsScopedTimer {
public:
	using Adder = void(*)(tjs_uint64);
	explicit TVPRenderStatsScopedTimer(Adder a)
		: t0_(std::chrono::steady_clock::now()), adder_(a) {}
	~TVPRenderStatsScopedTimer() {
		const auto t1 = std::chrono::steady_clock::now();
		adder_((tjs_uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0_).count());
	}
private:
	const std::chrono::steady_clock::time_point t0_;
	const Adder adder_;
};
// 再帰呼び出しがある関数で「top-level (depth==0) のときだけ計測」する。
// 呼び出し側で int の depth カウンタ (thread_local 推奨) を持って渡す。
class TVPRenderStatsTopLevelScopedTimer {
public:
	using Adder = void(*)(tjs_uint64);
	TVPRenderStatsTopLevelScopedTimer(int& depth, Adder a)
		: depth_(depth), adder_(a), top_level_(depth == 0) {
		if (top_level_) t0_ = std::chrono::steady_clock::now();
		++depth_;
	}
	~TVPRenderStatsTopLevelScopedTimer() {
		--depth_;
		if (top_level_) {
			const auto t1 = std::chrono::steady_clock::now();
			adder_((tjs_uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0_).count());
		}
	}
private:
	int& depth_;
	const Adder adder_;
	const bool top_level_;
	std::chrono::steady_clock::time_point t0_;
};
#define KRKRZ_RENDER_STATS_SCOPE(adder) TVPRenderStatsScopedTimer _krkrz_stats_timer_(adder)
#else
#define KRKRZ_RENDER_STATS_SCOPE(adder) ((void)0)
#endif

#endif
