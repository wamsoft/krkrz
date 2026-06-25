#include "tjsCommHead.h"
//---------------------------------------------------------------------------
#include "tjsUtils.h"
#include "MsgIntf.h"
#include "SysInitIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "MsgImpl.h"

#include "Application.h"
#include "BitmapBitsAlloc.h"
#include "MemoryAllocatorStats.h"

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

void* tTVPBitmapBitsAlloc::Alloc( tjs_uint size, tjs_uint width, tjs_uint height ) {
	if(size == 0) return NULL;
	tTJSCriticalSectionHolder Lock(AllocCS);	// Lock

	InitializeAllocator();
	tjs_uint8 * ptrorg, * ptr;
	tjs_uint allocbytes = 16 + size + sizeof(tTVPLayerBitmapMemoryRecord) + sizeof(tjs_uint32)*2;

	ptr = ptrorg = (tjs_uint8*)Allocator->allocate(allocbytes, TVPAllocTag::BitmapBits);
	if(!ptr) {
		TVPThrowExceptionMessage(TVPCannotAllocateBitmapBits,
		TJS_W("at TVPAllocBitmapBits"), ttstr((tjs_int)allocbytes) + TJS_W("(") +
			ttstr((int)width) + TJS_W("x") + ttstr((int)height) + TJS_W(")"));
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

