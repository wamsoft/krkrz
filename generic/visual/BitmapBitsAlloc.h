
#ifndef __BITMAP_BITS_ALLOC_H__
#define __BITMAP_BITS_ALLOC_H__

//---------------------------------------------------------------------------
// heap allocation functions for bitmap bits
//---------------------------------------------------------------------------
class tTVPBitmapBitsAlloc {
	typedef tjs_uint32 tTJSPointerSizedInteger;
	// this must be a integer type that has the same size with the normal
	// pointer ( void *)
	//---------------------------------------------------------------------------
	#pragma pack(push, 1)
	struct tTVPLayerBitmapMemoryRecord
	{
		void * alloc_ptr; // allocated pointer
		tjs_uint size; // original bmp bits size, in bytes
		tjs_uint32 sentinel_backup1; // sentinel value 1
		tjs_uint32 sentinel_backup2; // sentinel value 2
	};
	#pragma pack(pop)
	static iTVPMemoryAllocator* Allocator;
	static tTJSCriticalSection AllocCS;

public:
	static void InitializeAllocator();
	static void FreeAllocator();
	static void* Alloc( tjs_uint size, tjs_uint width, tjs_uint height );
	static void Free( void* ptr );
	// テレメトリ取得用 (doc/legacy/MemoryBudgetNegotiation.md §11)。
	static iTVPMemoryAllocator* GetAllocator() { return Allocator; }
};

#endif // __BITMAP_BITS_ALLOC_H__

