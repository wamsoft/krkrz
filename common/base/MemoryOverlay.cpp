#include "tjsCommHead.h"
#include "MemoryOverlay.h"

#ifdef KRKRZ_ENABLE_MEMORY_OVERLAY

#include "tjsUtils.h"             // tTJSCriticalSection (BitmapBitsAlloc.h 依存)
#include "BinaryStreamBuffer.h"   // TVPGetFileAllocator
#include "BitmapBitsAlloc.h"      // tTVPBitmapBitsAlloc::GetAllocator
#include "SoundAllocator.h"       // TVPGetSoundAllocator
#include "ProcessMemory.h"        // TVPGetProcessMemoryInfo
#include "SystemAllocatorInfo.h"  // TVPGetSystemAllocatorInfo
#include "TickCount.h"            // TVPGetTickCount
#include "ThreadIntf.h"
#include "SysInitIntf.h"
#include "Application.h"          // iTVPMemoryAllocator
#include "StorageCache.h"         // TVPGetStorageCacheCount
#include "GraphicsLoaderIntf.h"   // TVPGetGraphicCacheCount
#include "GlobalAllocStats.h"     // TVPGlobalAllocStats::Get*Stats

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

namespace {

class tTVPOverlaySamplerThread : public tTVPThread {
	tTVPThreadEvent stop_event_;
public:
	tTVPOverlaySamplerThread() : tTVPThread("MemOverlaySampler") {}
	void RequestStop() {
		Terminate();
		stop_event_.Set();
	}
protected:
	void Execute() override;
};

std::mutex                         g_mutex;
std::deque<TVPMemoryOverlaySample> g_samples;
std::atomic<bool>                  g_enabled{false};
tTVPOverlaySamplerThread          *g_sampler = nullptr;

void TVPCollectSample()
{
	TVPMemoryOverlaySample s;
	s.tick_ms = TVPGetTickCount();
	if (auto *fa = TVPGetFileAllocator()) {
		auto stats = fa->getStats();
		// SIZE_MAX (= 不明) は 0 扱いにして graph 軸が壊れないようにする。
		s.file_used        = (stats.current_used != SIZE_MAX) ? stats.current_used : 0;
		s.file_peak        = (stats.peak_used    != SIZE_MAX) ? stats.peak_used    : 0;
		s.file_alloc_count = stats.alloc_count;
	}
	if (auto *ba = tTVPBitmapBitsAlloc::GetAllocator()) {
		auto stats = ba->getStats();
		// Bitmap allocator も Sized mode 化済 (size 付き free を tTVPBitmapBitsAlloc::Free
		// から呼ぶ)。Unsized fallback (旧 plugin allocator 等) では 0 扱い。
		s.bitmap_used        = (stats.current_used != SIZE_MAX) ? stats.current_used : 0;
		s.bitmap_peak        = (stats.peak_used    != SIZE_MAX) ? stats.peak_used    : 0;
		s.bitmap_alloc_count = stats.alloc_count;
	}
	if (auto *sa = TVPGetSoundAllocator()) {
		auto stats = sa->getStats();
		// SoundAllocator も Sized mode (BasicSoundAllocator/TVPPooledAllocator)。
		s.sound_used        = (stats.current_used != SIZE_MAX) ? stats.current_used : 0;
		s.sound_peak        = (stats.peak_used    != SIZE_MAX) ? stats.peak_used    : 0;
		s.sound_alloc_count = stats.alloc_count;
	}
	auto pmi = TVPGetProcessMemoryInfo();
	s.process_rss = (pmi.rss != SIZE_MAX) ? pmi.rss : 0;

	// キャッシュ件数 (file 層 / decode 層)。Count 取得は内部 CS を取るが
	// hash 走査だけで短時間。サンプラ thread (4Hz / ttpIdle) からの呼び出しなので
	// メインスレッドへの影響は無視できる範囲。
	TVPGetStorageCacheCount(s.file_cache_count, s.file_cache_pinned);
	TVPGetGraphicCacheCount(s.image_cache_count, s.image_cache_pinned);

	// GlobalAllocStats (TVPGlobalAllocStats) のスナップショット。
	// tracking 未活性 (Initialize 前) なら全 0 が返るので overlay 側は
	// "(tracking off)" 等を出すか単に 0 表示で済む。
	// SDL stats は KRKRZ_SDLMEMORY_STAT=ON のときだけ収集 (それ以外は
	// SDL_SetMemoryFunctions も仕掛けないので全部 0)。
	{
		auto k = TVPGlobalAllocStats::GetKrkrzStats();
		s.krkrz_live           = k.live_bytes;
		s.krkrz_pool_used      = k.pool_used;
		s.krkrz_pool_cap       = k.pool_capacity;
		s.krkrz_fallback_count = k.fallback_count;
#ifdef KRKRZ_SDLMEMORY_STAT
		auto sd = TVPGlobalAllocStats::GetSdlStats();
		s.sdl_live             = sd.live_bytes;
		s.sdl_pool_used        = sd.pool_used;
		s.sdl_pool_cap         = sd.pool_capacity;
		s.sdl_fallback_count   = sd.fallback_count;
#endif
	}

	// システムアロケータ情報 (iTVPSystemAllocatorInfo 経由)。
	// コンソール機等のプラットフォーム固有実装ではより正確な空き容量が取得できる。
	// 一般 OS ではシステムの空き物理メモリが近似値として入る。
	if (auto *sysInfo = TVPGetSystemAllocatorInfo()) {
		auto stats = sysInfo->GetStats();
		auto toU64 = [](size_t v) -> uint64_t {
			return (v == SIZE_MAX) ? 0 : static_cast<uint64_t>(v);
		};
		s.sys_total_free     = toU64(stats.total_free_size);
		s.sys_allocatable    = toU64(stats.allocatable_size);
		s.sys_avail_physical = toU64(stats.system_avail_physical);
	}

	std::lock_guard<std::mutex> lk(g_mutex);
	if (g_samples.size() >= TVPMemoryOverlay::kMaxSamples) g_samples.pop_front();
	g_samples.push_back(s);
}

void tTVPOverlaySamplerThread::Execute()
{
	SetPriority(ttpIdle);
	while (!GetTerminated()) {
		// Enabled な時のみサンプル収集 (OFF 時は wait のみ)。
		if (g_enabled.load(std::memory_order_relaxed)) {
			TVPCollectSample();
		}
		// kSampleIntervalMs 待つか stop_event で抜ける
		if (stop_event_.WaitFor(TVPMemoryOverlay::kSampleIntervalMs)) {
			break;
		}
	}
}

} // anonymous namespace

namespace TVPMemoryOverlay {

void Initialize()
{
	if (g_sampler) return;
	g_sampler = new tTVPOverlaySamplerThread();
	g_sampler->StartThread();
}

void Finalize()
{
	if (g_sampler) {
		g_sampler->RequestStop();
		g_sampler->WaitFor();
		delete g_sampler;
		g_sampler = nullptr;
	}
	std::lock_guard<std::mutex> lk(g_mutex);
	g_samples.clear();
}

void SetEnabled(bool enabled)
{
	g_enabled.store(enabled, std::memory_order_relaxed);
	if (!enabled) {
		std::lock_guard<std::mutex> lk(g_mutex);
		g_samples.clear();
	}
}

bool IsEnabled()
{
	return g_enabled.load(std::memory_order_relaxed);
}

void GetSnapshot(std::vector<TVPMemoryOverlaySample> &out)
{
	out.clear();
	if (!g_enabled.load(std::memory_order_relaxed)) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	out.reserve(g_samples.size());
	for (auto &s : g_samples) out.push_back(s);
}

} // namespace TVPMemoryOverlay

static void TVPFinalizeMemoryOverlay()
{
	TVPMemoryOverlay::Finalize();
}
static tTVPAtExit
	TVPFinalizeMemoryOverlayAtExit(TVP_ATEXIT_PRI_CLEANUP - 2,
	                               TVPFinalizeMemoryOverlay);

#else // !KRKRZ_ENABLE_MEMORY_OVERLAY

// ---------------------------------------------------------------------------
// KRKRZ_ENABLE_MEMORY_OVERLAY=OFF: sampler thread / 描画 / 収集ロジックを除去。
// API は呼ばれても何も起こらない (REPL .memoverlay / System.setMemoryOverlay も
// "disabled" 相当の挙動)。
// ---------------------------------------------------------------------------
namespace TVPMemoryOverlay {

void Initialize() {}
void Finalize()   {}
void SetEnabled(bool) {}
bool IsEnabled()  { return false; }
void GetSnapshot(std::vector<TVPMemoryOverlaySample> &out) { out.clear(); }

} // namespace TVPMemoryOverlay

#endif // KRKRZ_ENABLE_MEMORY_OVERLAY
