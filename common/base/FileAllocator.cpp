#include "tjsCommHead.h"
#include "Application.h"
#include "BinaryStreamBuffer.h"
#include "MemoryAllocatorStats.h"
#include "PooledAllocator.h"
#include "StorageCache.h"
#include "SysInitIntf.h"
#include "LogIntf.h"

#include <cstdlib>

//---------------------------------------------------------------------------
// ファイル読み込みバッファ用のアロケータ
// Application->CreateFileAllocator() で生成された iTVPMemoryAllocator を
// 介して file_malloc / file_free を提供する。確保失敗時は
// TVPClearOldStorageCache() でキャッシュ駆逐してリトライ。
//---------------------------------------------------------------------------

namespace {
// BasicFileAllocator は L1 軽量カウンタ + L2 サイズヒストグラム + L3/L4 tag 別
// 集計を Sized mode で追跡する (doc/legacy/MemoryBudgetNegotiation.md §11)。
// free 時に size と tag を回収するためにヘッダ (size + tag) を 16-byte align で
// 前置する。16 byte は max_align_t を保つための余裕で size_t (8) + tag (2) +
// padding (6) の組合せで収まる。
class BasicFileAllocator : public iTVPMemoryAllocator
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
		return allocate(size, TVPAllocTag::FileCache);
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

// FileAllocator のプールサイズ既定値 (バイト)。実 app 平均で 200 MB 程度の
// キャッシュ上限なので、プールはそれを少し上回るぶん用意する。
// CLI `-filepoolsize=N` (MB 指定、none/0 で pool 無効化 → 従来 raw malloc)。
size_t TVPGetFileAllocatorPoolSize()
{
	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-filepoolsize"), &val)) {
		ttstr str(val);
		if(str == TJS_W("none") || str == TJS_W("off") || str == TJS_W("0")) {
			return 0;  // 従来 raw malloc
		}
		tjs_int64 mb = (tjs_int64)val;
		if(mb > 0) return (size_t)mb * 1024 * 1024;
	}
	// 既定: 512 MB (StorageCache 既定 200 MB + α)
	return (size_t)512 * 1024 * 1024;
}

iTVPMemoryAllocator *
tTVPApplication::CreateFileAllocator()
{
	size_t pool_size = TVPGetFileAllocatorPoolSize();
	if(pool_size == 0) {
		TVPLOG_INFO("FileAllocator: using BasicFileAllocator (raw malloc)");
		return new BasicFileAllocator();
	}
	return new TVPPooledAllocator(pool_size, "FilePool", TVPAllocTag::FileCache);
}

//---------------------------------------------------------------------------

static iTVPMemoryAllocator *g_FileAllocator = nullptr;

iTVPMemoryAllocator *TVPGetFileAllocator() { return g_FileAllocator; }

void TVPInitializeFileAllocator()
{
	if (!g_FileAllocator) {
		g_FileAllocator = Application->CreateFileAllocator();
		// FileAllocator 容量と StorageCache 上限を整合
		// (doc/legacy/MemoryBudgetNegotiation.md §4.1)。
		// BasicFileAllocator (capacity=SIZE_MAX) なら no-op で従来挙動。
		TVPNegotiateStorageCacheBudget(g_FileAllocator);
	}
}

static void TVPFinalizeFileAllocator()
{
	if (g_FileAllocator) {
		delete g_FileAllocator;
		g_FileAllocator = nullptr;
	}
}
static tTVPAtExit
	TVPUninitFileAllocator(TVP_ATEXIT_PRI_CLEANUP, TVPFinalizeFileAllocator);

// T4: アプリ終了直前 (CLEANUP より僅かに前) で FileAllocator のリーク推定を出す。
// CLEANUP-1 = 9999 を使うことで TVPFinalizeFileAllocator (CLEANUP=10000) より
// 先に走る。
static void TVPDumpFileAllocatorLeaks()
{
	TVPDumpAllocatorLeakReport("FileAllocator", g_FileAllocator);
}
static tTVPAtExit
	TVPDumpFileAllocatorLeaksAtExit(TVP_ATEXIT_PRI_CLEANUP - 1, TVPDumpFileAllocatorLeaks);

//---------------------------------------------------------------------------

extern "C" void *file_malloc(size_t size)
{
	if (!g_FileAllocator) TVPInitializeFileAllocator();
	void *r = g_FileAllocator->allocate(size, TVPAllocTag::FileCache);
	if (!r) {
		TVPClearOldStorageCache(0, false);
		r = g_FileAllocator->allocate(size, TVPAllocTag::FileCache);
		if (!r) {
			TVPClearOldStorageCache(0, true);
			r = g_FileAllocator->allocate(size, TVPAllocTag::FileCache);
		}
	}
	return r;
}

extern "C" void file_free(void *p)
{
	if (p && g_FileAllocator) g_FileAllocator->free(p);
}
