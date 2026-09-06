#include "tjsCommHead.h"
#include "Application.h"
#include "SoundAllocator.h"
#include "MemoryAllocatorStats.h"
#include "PooledAllocator.h"
#include "SysInitIntf.h"
#include "LogIntf.h"

#include <cstdlib>
#include <cstring>

//---------------------------------------------------------------------------
// サウンド用バッファのアロケータ
// Application->CreateSoundAllocator() で生成された iTVPMemoryAllocator を
// 介して sound_malloc / sound_free を提供する。
// 既定では BasicSoundAllocator (raw malloc + 16B header) を使い、TVPAllocTag::Sound
// に紐づく per-tag 統計を tTVPMemoryAllocatorStatsCollector で集計する。
// Pool 化 (-soundpoolsize) は Phase 2 で対応予定。
//---------------------------------------------------------------------------

namespace {
// BasicFileAllocator と同じ 16B header 方式。size と tag をヘッダに保存し、
// sound_free(p) 単独で正しく recordFree(size, tag) を呼べるようにする。
class BasicSoundAllocator : public iTVPMemoryAllocator
{
	struct Header {
		size_t      size;
		TVPAllocTag tag;
		// padding to 16 bytes; do not access padding bytes
	};
	static constexpr size_t kHeaderSize = 16;
	static_assert(sizeof(Header) <= kHeaderSize,
	              "Header must fit within kHeaderSize");

	tTVPMemoryAllocatorStatsCollector stats_{
		tTVPMemoryAllocatorStatsCollector::Mode::Sized};

public:
	void *allocate(size_t size) override {
		return allocate(size, TVPAllocTag::Sound);
	}

	void *allocate(size_t size, TVPAllocTag tag) override {
		void *raw = ::malloc(size + kHeaderSize);
		if (!raw) return nullptr;
		Header *h = static_cast<Header *>(raw);
		h->size = size;
		h->tag  = tag;
		stats_.recordAlloc(size, tag);
		return static_cast<char *>(raw) + kHeaderSize;
	}

	void free(void *p) override {
		if (!p) return;
		void *raw = static_cast<char *>(p) - kHeaderSize;
		Header *h = static_cast<Header *>(raw);
		stats_.recordFree(h->size, h->tag);
		::free(raw);
	}

	size_t getAllocatedSize(void *p) const override {
		if (!p) return 0;
		auto *raw = static_cast<char *>(p) - kHeaderSize;
		return static_cast<const Header *>(static_cast<const void *>(raw))->size;
	}

	size_t used() const override { return stats_.used(); }
	Stats getStats() const override { return stats_.snapshot(); }
	TagStats getTagStats(TVPAllocTag tag) const override {
		return stats_.tagSnapshot(tag);
	}
	void resetPeak() override { stats_.resetPeak(); }
};
} // namespace

// SoundAllocator のプールサイズ既定値 (バイト)。
// CLI `-soundpoolsize=N` (MB 指定、none/0 で pool 無効化 → 従来 raw malloc)。
// PCM/リング/クロスフェード一時 + miniaudio 内部 + libogg/libvorbis decode
// 作業 (Phase 3 で hook 後) を含める。
// ⚠ pool は起動時に malloc で丸ごと確保されるので、 既定を大きくすると
//   汎用ヒープを圧迫する (詳細は TVPGetBitmapAllocatorPoolSize のコメント)。
size_t TVPGetSoundAllocatorPoolSize()
{
	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-soundpoolsize"), &val)) {
		ttstr str(val);
		if(str == TJS_W("none") || str == TJS_W("off") || str == TJS_W("0")) {
			return 0;  // 従来 raw malloc
		}
		tjs_int64 mb = (tjs_int64)val;
		if(mb > 0) return (size_t)mb * 1024 * 1024;
	}
	// 既定: 16 MB (足りない案件は -soundpoolsize=N で増やす)
	return (size_t)16 * 1024 * 1024;
}

iTVPMemoryAllocator *
tTVPApplication::CreateSoundAllocator()
{
	size_t pool_size = TVPGetSoundAllocatorPoolSize();
	if(pool_size == 0) {
		TVPLOG_INFO("SoundAllocator: using BasicSoundAllocator (raw malloc)");
		return new BasicSoundAllocator();
	}
	return new TVPPooledAllocator(pool_size, "SoundPool", TVPAllocTag::Sound);
}

//---------------------------------------------------------------------------

static iTVPMemoryAllocator *g_SoundAllocator = nullptr;

iTVPMemoryAllocator *TVPGetSoundAllocator() { return g_SoundAllocator; }

void TVPInitializeSoundAllocator()
{
	if (!g_SoundAllocator) {
		g_SoundAllocator = Application->CreateSoundAllocator();
	}
}

static void TVPFinalizeSoundAllocator()
{
	if (g_SoundAllocator) {
		delete g_SoundAllocator;
		g_SoundAllocator = nullptr;
	}
}
static tTVPAtExit
	TVPUninitSoundAllocator(TVP_ATEXIT_PRI_CLEANUP, TVPFinalizeSoundAllocator);

// アプリ終了直前 (CLEANUP より僅かに前) で SoundAllocator のリーク推定を出す。
static void TVPDumpSoundAllocatorLeaks()
{
	TVPDumpAllocatorLeakReport("SoundAllocator", g_SoundAllocator);
}
static tTVPAtExit
	TVPDumpSoundAllocatorLeaksAtExit(TVP_ATEXIT_PRI_CLEANUP - 1, TVPDumpSoundAllocatorLeaks);

//---------------------------------------------------------------------------

extern "C" void *sound_malloc(size_t size)
{
	if (!g_SoundAllocator) TVPInitializeSoundAllocator();
	return g_SoundAllocator->allocate(size, TVPAllocTag::Sound);
}

extern "C" void *sound_calloc(size_t nmemb, size_t size)
{
	if (!g_SoundAllocator) TVPInitializeSoundAllocator();
	size_t total = nmemb * size;
	void *p = g_SoundAllocator->allocate(total, TVPAllocTag::Sound);
	if (p) std::memset(p, 0, total);
	return p;
}

extern "C" void *sound_realloc(void *p, size_t size)
{
	if (!g_SoundAllocator) TVPInitializeSoundAllocator();
	return g_SoundAllocator->reallocate(p, size, TVPAllocTag::Sound);
}

extern "C" void sound_free(void *p)
{
	if (p && g_SoundAllocator) g_SoundAllocator->free(p);
}
