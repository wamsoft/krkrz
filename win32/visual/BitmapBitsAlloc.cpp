#include "tjsCommHead.h"
//---------------------------------------------------------------------------
#include "tjsUtils.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"

#include "Application.h"
#include "BitmapBitsAlloc.h"
#include "MemoryAllocatorStats.h"

// 各 BitmapBits 系 allocator は L1 軽量カウンタ + L2 サイズヒストグラムを
// Sized mode で追跡。size は free 時に iTVPMemoryAllocator::free(void*, size_t)
// 経由で tTVPBitmapBitsAlloc から渡される (record->size + ヘッダ込みの allocbytes)。
// 旧 free(void*) も残しているが、こちらは size 不明のため current_used が
// 減らない (= caller 側で必ず size 付きを使うこと)。

class BasicAllocator : public iTVPMemoryAllocator
{
	tTVPMemoryAllocatorStatsCollector stats_{
		tTVPMemoryAllocatorStatsCollector::Mode::Sized};
public:
	BasicAllocator() {
		TVPAddLog( TJS_W("(info) Use malloc for Bitmap") );
	}
	void* allocate( size_t size ) override { return allocate(size, TVPAllocTag::BitmapBits); }
	void* allocate( size_t size, TVPAllocTag tag ) override {
		void* p = malloc(size);
		if (p) stats_.recordAlloc(size, tag);
		return p;
	}	// Windowsでは ::HeapAlloc( _get_heap_handle(), 0, size ); と同じはず
	void free( void* mem ) override { stats_.recordFree(0); ::free( mem ); }
	void free( void* mem, size_t size ) override {
		stats_.recordFree(size, TVPAllocTag::BitmapBits);
		::free( mem );
	}
	Stats getStats() const override { return stats_.snapshot(); }
	TagStats getTagStats(TVPAllocTag tag) const override { return stats_.tagSnapshot(tag); }
	void resetPeak() override { stats_.resetPeak(); }
};
#ifdef WIN32
class GlobalAllocAllocator : public iTVPMemoryAllocator
{
	tTVPMemoryAllocatorStatsCollector stats_{
		tTVPMemoryAllocatorStatsCollector::Mode::Sized};
public:
	GlobalAllocAllocator() {
		TVPAddLog( TJS_W("(info) Use GlobalAlloc allocater for Bitmap") );
	}
	void* allocate( size_t size ) override { return allocate(size, TVPAllocTag::BitmapBits); }
	void* allocate( size_t size, TVPAllocTag tag ) override {
		void* p = GlobalAlloc(GMEM_FIXED,size);
		if (p) stats_.recordAlloc(size, tag);
		return p;
	}
	void free( void* mem ) override { stats_.recordFree(0); GlobalFree((HGLOBAL)mem ); }
	void free( void* mem, size_t size ) override {
		stats_.recordFree(size, TVPAllocTag::BitmapBits);
		GlobalFree((HGLOBAL)mem );
	}
	Stats getStats() const override { return stats_.snapshot(); }
	TagStats getTagStats(TVPAllocTag tag) const override { return stats_.tagSnapshot(tag); }
	void resetPeak() override { stats_.resetPeak(); }
};
class HeapAllocAllocator : public iTVPMemoryAllocator
{
	static const DWORD HeapFlag = 0;

	HANDLE HeapHandle;
	tTVPMemoryAllocatorStatsCollector stats_{
		tTVPMemoryAllocatorStatsCollector::Mode::Sized};
public:
	HeapAllocAllocator() : HeapHandle(NULL) {
		tTJSVariant val;
		tjs_uint64 size = 0;
		if(TVPGetCommandLine(TJS_W("-bitmapheapsize"), &val)) {
			ttstr str(val);
			if(str == TJS_W("auto")) {
				size = 0;
			} else {
				size = (tjs_int64)val;
				if( size == 0 ) {
					HeapHandle = ::HeapCreate( HeapFlag, 0, 0 );
				}
				size *= 1024*1024;
			}
		}
		if( HeapHandle == NULL ) {
			if( size == 0 ) {
				MEMORYSTATUSEX status = { sizeof(MEMORYSTATUSEX) };
				::GlobalMemoryStatusEx(&status);
				if( status.ullAvailVirtual < status.ullTotalPhys ) {
					size = status.ullAvailVirtual / 2;
				} else {
					size = status.ullTotalPhys / 2;
				}
			}
			while( HeapHandle == NULL && size > (1024*1024) ) {
				HeapHandle = ::HeapCreate( HeapFlag, (SIZE_T)size, 0 );
				if( HeapHandle == NULL ) {
					size /= 2;
				}
			}
		}

		if( HeapHandle ) {
			ULONG HeapInformation = 2;
			BOOL lfhenable = ::HeapSetInformation( HeapHandle, HeapCompatibilityInformation, &HeapInformation, sizeof(HeapInformation) );
		}
		TVPAddLog( TJS_W("(info) Use separate heap allocater for Bitmap") );
	}
	virtual ~HeapAllocAllocator() {
		if( HeapHandle ) ::HeapDestroy(HeapHandle);
		HeapHandle = NULL;
	}
	void* allocate( size_t size ) override { return allocate(size, TVPAllocTag::BitmapBits); }
	void* allocate( size_t size, TVPAllocTag tag ) override {
		if( HeapHandle == NULL ) return NULL;
		void* result = ::HeapAlloc( HeapHandle, HeapFlag, size );
		if( result == NULL ) {
			::HeapCompact( HeapHandle, HeapFlag );	// try compact
			result = ::HeapAlloc( HeapHandle, HeapFlag, size ); // retry
		}
		if (result) stats_.recordAlloc(size, tag);
		return result;
	}
	void free( void* mem ) override {
		if( HeapHandle ) {
			BOOL ret = ::HeapFree( HeapHandle, HeapFlag, mem );
			::HeapCompact( HeapHandle, HeapFlag );
		}
		stats_.recordFree(0);
	}
	void free( void* mem, size_t size ) override {
		if( HeapHandle ) {
			BOOL ret = ::HeapFree( HeapHandle, HeapFlag, mem );
			::HeapCompact( HeapHandle, HeapFlag );
		}
		stats_.recordFree(size, TVPAllocTag::BitmapBits);
	}
	Stats getStats() const override { return stats_.snapshot(); }
	TagStats getTagStats(TVPAllocTag tag) const override { return stats_.tagSnapshot(tag); }
	void resetPeak() override { stats_.resetPeak(); }
};
class ProcessHeapAllocAllocator : public iTVPMemoryAllocator
{
	tTVPMemoryAllocatorStatsCollector stats_{
		tTVPMemoryAllocatorStatsCollector::Mode::Sized};
public:
	ProcessHeapAllocAllocator() {
		TVPAddLog( TJS_W("(info) Use Process HeadAlloc allocater for Bitmap") );
	}
	void* allocate( size_t size ) override { return allocate(size, TVPAllocTag::BitmapBits); }
	void* allocate( size_t size, TVPAllocTag tag ) override {
		void* result = ::HeapAlloc( ::GetProcessHeap(), 0, size );
		if( result == NULL ) {
			::HeapCompact( ::GetProcessHeap(), 0 );	// try compact
			result = ::HeapAlloc( ::GetProcessHeap(), 0, size ); // retry
		}
		if (result) stats_.recordAlloc(size, tag);
		return result;
	}
	void free( void* mem ) override {
		stats_.recordFree(0);
		::HeapFree(::GetProcessHeap(), 0, mem);
	}
	void free( void* mem, size_t size ) override {
		stats_.recordFree(size, TVPAllocTag::BitmapBits);
		::HeapFree(::GetProcessHeap(), 0, mem);
	}
	Stats getStats() const override { return stats_.snapshot(); }
	TagStats getTagStats(TVPAllocTag tag) const override { return stats_.tagSnapshot(tag); }
	void resetPeak() override { stats_.resetPeak(); }
};
#endif

// Bitmap用のAllocatorを返す
iTVPMemoryAllocator *
tTVPApplication::CreateBitmapAllocator()
{
	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-bitmapallocator"), &val)) {
		ttstr str(val);
#ifdef WIN32
		if(str == TJS_W("globalalloc"))
			return new GlobalAllocAllocator();
		else if(str == TJS_W("separateheap"))
			return new HeapAllocAllocator();
		else if(str == TJS_W("processheap"))
			return new ProcessHeapAllocAllocator();
		else    // malloc
#endif
			return new BasicAllocator();
	} else {
#ifdef WIN32
		//Allocator = new GlobalAllocAllocator();
		return new ProcessHeapAllocAllocator();
#else
		return new BasicAllocator();
#endif
	}
}


// -------------------------------------------------------------


iTVPMemoryAllocator* tTVPBitmapBitsAlloc::Allocator = NULL;
tTJSCriticalSection tTVPBitmapBitsAlloc::AllocCS;

void tTVPBitmapBitsAlloc::InitializeAllocator() {
	if( Allocator == NULL ) {
		Allocator = Application->CreateBitmapAllocator();
	}
}
void tTVPBitmapBitsAlloc::FreeAllocator() {
	if( Allocator ) delete Allocator;
	Allocator = NULL;
}
static tTVPAtExit
	TVPUninitMessageLoad(TVP_ATEXIT_PRI_CLEANUP, tTVPBitmapBitsAlloc::FreeAllocator);

// T4: アプリ終了直前 (CLEANUP より僅かに前) で BitmapAllocator のリーク推定を出す。
static void TVPDumpBitmapAllocatorLeaks() {
	TVPDumpAllocatorLeakReport("BitmapAllocator", tTVPBitmapBitsAlloc::GetAllocator());
}
static tTVPAtExit
	TVPDumpBitmapAllocatorLeaksAtExit(TVP_ATEXIT_PRI_CLEANUP - 1, TVPDumpBitmapAllocatorLeaks);

extern void TVPHeapDump();
void* tTVPBitmapBitsAlloc::Alloc( tjs_uint size, tjs_uint width, tjs_uint height ) {
	if(size == 0) return NULL;
	tTJSCriticalSectionHolder Lock(AllocCS);	// Lock

	InitializeAllocator();
	tjs_uint8 * ptrorg, * ptr;
	tjs_uint allocbytes = 16 + size + sizeof(tTVPLayerBitmapMemoryRecord) + sizeof(tjs_uint32)*2;

	ptr = ptrorg = (tjs_uint8*)Allocator->allocate(allocbytes, TVPAllocTag::BitmapBits);
	if(!ptr) {
		// Do GC
		TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
#ifdef WIN32
		// Do compact CRT and Global Heap
		HANDLE hHeap = ::GetProcessHeap();
		if( hHeap ) {
			::HeapCompact( hHeap, 0 );
		}
		HANDLE hCrtHeap = (HANDLE)_get_heap_handle();
		if( hCrtHeap && hCrtHeap != hHeap ) {
			::HeapCompact( hCrtHeap, 0 );
		}
#endif
		ptr = ptrorg = (tjs_uint8*)Allocator->allocate(allocbytes, TVPAllocTag::BitmapBits);
		if(!ptr) {
			TVPHeapDump();
			TVPThrowExceptionMessage(TVPCannotAllocateBitmapBits,
				TJS_W("at TVPAllocBitmapBits"), ttstr((tjs_int)allocbytes) + TJS_W("(") +
				ttstr((int)width) + TJS_W("x") + ttstr((int)height) + TJS_W(")"));
		}
	}
	// align to a paragraph ( 16-bytes )
	ptr += 16 + sizeof(tTVPLayerBitmapMemoryRecord);
	*reinterpret_cast<tTJSPointerSizedInteger*>(&ptr) >>= 4;
	*reinterpret_cast<tTJSPointerSizedInteger*>(&ptr) <<= 4;

	// Bitmap の pitch padding (32bpp は bitmap_width = ceil(width,4) に切り上げ
	// られるため右端に最大 3px の不可視列がある) は decoder (TLG 等) が書き込ま
	// ない。allocator が再利用ブロックを返した場合、その padding 列に前回画像の
	// 色が残り、bilinear/affine サンプリング時にエッジゴミとして露見する。
	// これを防ぐため確保時点で使用領域全体を zero-fill する。
	memset(ptr, 0, size);

	tTVPLayerBitmapMemoryRecord * record =
		(tTVPLayerBitmapMemoryRecord*)
		(ptr - sizeof(tTVPLayerBitmapMemoryRecord) - sizeof(tjs_uint32));

	// fill memory allocation record
	record->alloc_ptr = (void *)ptrorg;
	record->size = size;
	record->sentinel_backup1 = rand() + (rand() << 16);
	record->sentinel_backup2 = rand() + (rand() << 16);

	// set sentinel
	*(tjs_uint32*)(ptr - sizeof(tjs_uint32)) = ~record->sentinel_backup1;
	*(tjs_uint32*)(ptr + size              ) = ~record->sentinel_backup2;
		// Stored sentinels are nagated, to avoid that the sentinel backups in
		// tTVPLayerBitmapMemoryRecord becomes the same value as the sentinels.
		// This trick will make the detection of the memory corruption easier.
		// Because on some occasions, running memory writing will write the same
		// values at first sentinel and the tTVPLayerBitmapMemoryRecord.

	// return buffer pointer
	return ptr;
}
void tTVPBitmapBitsAlloc::Free( void* ptr ) {
	if(ptr)
	{
		tTJSCriticalSectionHolder Lock(AllocCS);	// Lock

		// get memory allocation record pointer
		tjs_uint8 *bptr = (tjs_uint8*)ptr;
		tTVPLayerBitmapMemoryRecord * record =
			(tTVPLayerBitmapMemoryRecord*)
			(bptr - sizeof(tTVPLayerBitmapMemoryRecord) - sizeof(tjs_uint32));

		// check sentinel
		if(~(*(tjs_uint32*)(bptr - sizeof(tjs_uint32))) != record->sentinel_backup1)
			TVPThrowExceptionMessage( TVPLayerBitmapBufferUnderrunDetectedCheckYourDrawingCode );
		if(~(*(tjs_uint32*)(bptr + record->size      )) != record->sentinel_backup2)
			TVPThrowExceptionMessage( TVPLayerBitmapBufferOverrunDetectedCheckYourDrawingCode );

		// allocate 時の allocbytes と一致させる (Sized mode 集計用)。
		size_t allocbytes = 16 + record->size + sizeof(tTVPLayerBitmapMemoryRecord) + sizeof(tjs_uint32)*2;
		Allocator->free( record->alloc_ptr, allocbytes );
	}
}

