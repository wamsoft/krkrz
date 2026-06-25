#include "tjsCommHead.h"
#include "MemoryStatPeriodicDump.h"

#ifdef KRKRZ_ENABLE_PERIODIC_DUMP

#include "MemoryOverlay.h"     // TVPMemoryOverlay::SetEnabled
#include "SysInitIntf.h"
#include "SystemImpl.h"        // TVPHeapDump
#include "StorageCache.h"      // TVPDumpFileCacheList
#include "GraphicsLoaderIntf.h"// TVPDumpImageCacheList
#include "ThreadIntf.h"
#include "DebugIntf.h"         // TVPGetCommandLine

namespace {
class tTVPMemoryStatDumpThread : public tTVPThread {
	tjs_uint interval_ms_;
	tTVPThreadEvent stop_event_;
public:
	explicit tTVPMemoryStatDumpThread(tjs_uint interval_ms)
		: tTVPThread("MemStatDumpThread"), interval_ms_(interval_ms) {}

	void RequestStop() {
		Terminate();
		stop_event_.Set();
	}

protected:
	void Execute() override {
		SetPriority(ttpIdle);
		while (!GetTerminated()) {
			// interval_ms 待つか、Stop 要求 (Set) で抜ける
			if (stop_event_.WaitFor(interval_ms_)) {
				break; // 早期終了要求
			}
			if (GetTerminated()) break;
			TVPHeapDump();
		}
	}
};

tTVPMemoryStatDumpThread *g_DumpThread = nullptr;
bool g_DumpOnExit = false;
// -cachelistonexit=<mode>: 終了時にキャッシュ一覧をダンプするか。
//   none/0/未指定 = false / false (= デフォルト: 何もしない)
//   file          = file のみ
//   image         = image のみ
//   1 / all       = file + image 両方
bool g_DumpFileCacheOnExit  = false;
bool g_DumpImageCacheOnExit = false;
}

static void TVPFinalizeMemoryStatPeriodicDump()
{
	if (g_DumpThread) {
		g_DumpThread->RequestStop();
		g_DumpThread->WaitFor();
		delete g_DumpThread;
		g_DumpThread = nullptr;
	}
	if (g_DumpOnExit) {
		TVPHeapDump();
	}
	// HeapDump の後に出すと統計値 (件数・合計バイト) と一覧の整合が取りやすい。
	if (g_DumpFileCacheOnExit) {
		TVPDumpFileCacheList();
	}
	if (g_DumpImageCacheOnExit) {
		TVPDumpImageCacheList();
	}
}

// 終了時 dump は周期ダンプスレッド停止後に走らせたいので、
// CLEANUP より前 (= スレッドより先に走る) ではなく CLEANUP - 2 にして
// FileAllocator/BitmapAllocator finalize (CLEANUP) より前で実行。
static tTVPAtExit
	TVPFinalizeMemoryStatPeriodicDumpAtExit(TVP_ATEXIT_PRI_CLEANUP - 2,
	                                        TVPFinalizeMemoryStatPeriodicDump);

void TVPInitializeMemoryStatPeriodicDump()
{
	tTJSVariant val;

	// -memstatonexit=1
	if (TVPGetCommandLine(TJS_W("-memstatonexit"), &val)) {
		g_DumpOnExit = ((tjs_int)val) != 0;
	}

	// -cachelistonexit=<mode>: 終了時に Storages cache 一覧をダンプ
	//   1 / all  → file + image (`TVPDumpFileCacheList` + `TVPDumpImageCacheList`)
	//   file     → file 一覧のみ
	//   image    → image 一覧のみ
	//   0 / none → 何もしない (= 未指定時と同じ)
	if (TVPGetCommandLine(TJS_W("-cachelistonexit"), &val)) {
		ttstr s(val);
		if (s == TJS_W("file")) {
			g_DumpFileCacheOnExit = true;
		} else if (s == TJS_W("image")) {
			g_DumpImageCacheOnExit = true;
		} else if (s == TJS_W("all") || ((tjs_int)val) != 0) {
			g_DumpFileCacheOnExit  = true;
			g_DumpImageCacheOnExit = true;
		}
	}

	// -memstatinterval=N (秒)
	if (TVPGetCommandLine(TJS_W("-memstatinterval"), &val)) {
		tjs_int sec = (tjs_int)val;
		if (sec > 0) {
			tjs_uint interval_ms = static_cast<tjs_uint>(sec) * 1000;
			g_DumpThread = new tTVPMemoryStatDumpThread(interval_ms);
			g_DumpThread->StartThread();
			TVPAddLog(ttstr(TJS_W("(info) MemoryStat periodic dump enabled: interval=")) +
			          ttstr(sec) + ttstr(TJS_W("s")));
		}
	}

	// -memoverlay=1 で起動時から画面オーバレイを ON にする
	// (SDL3 build のみ実描画。WINVER は flag だけ立つ)。
	if (TVPGetCommandLine(TJS_W("-memoverlay"), &val)) {
		if (((tjs_int)val) != 0) {
			TVPMemoryOverlay::SetEnabled(true);
			TVPAddLog(TJS_W("(info) MemoryOverlay enabled at startup"));
		}
	}
}

#else // !KRKRZ_ENABLE_PERIODIC_DUMP

// OFF 時: cmdline 解析も thread 起動も一切しない。
// -memstatinterval / -memstatonexit / -cachelistonexit / -memoverlay は無視される。
void TVPInitializeMemoryStatPeriodicDump() {}

#endif // KRKRZ_ENABLE_PERIODIC_DUMP
