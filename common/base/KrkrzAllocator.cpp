// Krkrz (GlobalAllocStats) 用 Allocator のデフォルト実装。
// Application->CreateKrkrzAllocator() から呼ばれる。
//
// KRKRZ_ENABLE_ALLOC_STATS=ON 時: TVPPooledAllocator (TLSF) を返す
// KRKRZ_ENABLE_ALLOC_STATS=OFF 時: nullptr を返す (stats 無効化)
//
// プラットフォーム固有の allocator を使いたい場合 (例: PS5 の Direct Memory)、
// Application サブクラスで CreateKrkrzAllocator() をオーバーライドすること。

#include "tjsCommHead.h"
#include "Application.h"
#include "LogIntf.h"
#include "SysInitIntf.h"

#include <algorithm>  // std::min
#include <cstring>    // std::memcpy

#ifdef KRKRZ_ENABLE_ALLOC_STATS
#include "PooledAllocator.h"
#endif

//---------------------------------------------------------------------------
// iTVPMemoryAllocator::reallocate デフォルト実装
// getAllocatedSize() が 0 を返す実装ではコピーされず内容が破棄される
// (PS5BitmapAllocator/PS5FileAllocator 等が該当)。コピー保証が必要な
// 実装は本メソッドを必ず override すること。
//---------------------------------------------------------------------------
void *iTVPMemoryAllocator::reallocate(void *old, size_t new_size, TVPAllocTag tag)
{
	if (!old) return allocate(new_size, tag);
	if (new_size == 0) { free(old); return nullptr; }
	void *p = allocate(new_size, tag);
	if (!p) return nullptr;
	size_t old_size = getAllocatedSize(old);
	if (old_size > 0) {
		std::memcpy(p, old, std::min(old_size, new_size));
	}
	free(old);
	return p;
}

//---------------------------------------------------------------------------
// Krkrz Allocator のプールサイズ取得
// CLI `-krkrzpoolsize=N` (MB 指定、none/0 で pool 無効化)
//---------------------------------------------------------------------------
size_t TVPGetKrkrzAllocatorPoolSize()
{
	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-krkrzpoolsize"), &val)) {
		ttstr str(val);
		if(str == TJS_W("none") || str == TJS_W("off") || str == TJS_W("0")) {
			return 0;
		}
		tjs_int64 mb = (tjs_int64)val;
		if(mb > 0) return (size_t)mb * 1024 * 1024;
	}
	// 既定: 256 MB
	return (size_t)256 * 1024 * 1024;
}

//---------------------------------------------------------------------------
// tTVPApplication::CreateKrkrzAllocator デフォルト実装
//---------------------------------------------------------------------------
iTVPMemoryAllocator *
tTVPApplication::CreateKrkrzAllocator()
{
#ifdef KRKRZ_ENABLE_ALLOC_STATS
	size_t pool_size = TVPGetKrkrzAllocatorPoolSize();
	if(pool_size == 0) {
		TVPLOG_INFO("KrkrzAllocator: pool disabled (-krkrzpoolsize=none)");
		return nullptr;
	}
	return new TVPPooledAllocator(pool_size, "GlobalKrkrz", TVPAllocTag::Unknown);
#else
	// KRKRZ_ENABLE_ALLOC_STATS=OFF: allocator は使用されない
	return nullptr;
#endif
}
