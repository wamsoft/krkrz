#include "tjsCommHead.h"
#include "AllocTagScope.h"

// 本 TU 全体は KRKRZ_ENABLE_MEMSTAT_DETAIL 未定義時は無効化される。
// (header 側で no-op inline stub を提供するため、本 TU が空でも問題なし)
#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL

#include "Application.h"   // 完全な TVPAllocTag enum 定義
#include "LogIntf.h"

#include <atomic>
#include <cstring>

// ---------------------------------------------------------------------------
// thread-local tag stack
// ---------------------------------------------------------------------------
// MSVC で thread_local POD は static 初期化 (ゼロクリア) なので、ctor 等で
// 余計な alloc が起きない。operator new override から触っても再帰しない。
namespace {

constexpr int kMaxDepth = 16;

struct TagStack {
	TVPAllocTag entries[kMaxDepth];
	int         top;  // 0 = 空。entries[top-1] が現在 tag
};

thread_local TagStack g_stack = {};

// overflow 警告は process 全体で 1 回だけ
std::atomic<bool> g_overflow_warned{false};

} // namespace

// ---------------------------------------------------------------------------
// RAII / 手動 push/pop
// ---------------------------------------------------------------------------

TVPAllocTagScope::TVPAllocTagScope(TVPAllocTag tag) noexcept
{
	if (g_stack.top >= kMaxDepth) {
		pushed_ = false;
		if (!g_overflow_warned.exchange(true, std::memory_order_relaxed)) {
			TVPLOG_WARNING("AllocTagScope: stack overflow (depth>{}); tag ignored", kMaxDepth);
		}
		return;
	}
	g_stack.entries[g_stack.top++] = tag;
	pushed_ = true;
}

TVPAllocTagScope::TVPAllocTagScope(const char *tag_name) noexcept
	: TVPAllocTagScope(TVPAllocTagFromName(tag_name))
{}

TVPAllocTagScope::~TVPAllocTagScope() noexcept
{
	if (pushed_ && g_stack.top > 0) {
		--g_stack.top;
	}
}

void TVPPushAllocTag(TVPAllocTag tag) noexcept
{
	if (g_stack.top >= kMaxDepth) {
		if (!g_overflow_warned.exchange(true, std::memory_order_relaxed)) {
			TVPLOG_WARNING("TVPPushAllocTag: stack overflow (depth>{}); tag ignored", kMaxDepth);
		}
		return;
	}
	g_stack.entries[g_stack.top++] = tag;
}

void TVPPopAllocTag() noexcept
{
	if (g_stack.top <= 0) {
		// unbalanced pop は重大なバグの兆候だが、ここでクラッシュさせるのは
		// 過剰。警告 1 回出すだけにする。
		static std::atomic<bool> warned{false};
		if (!warned.exchange(true, std::memory_order_relaxed)) {
			TVPLOG_WARNING("TVPPopAllocTag: unbalanced pop on empty stack");
		}
		return;
	}
	--g_stack.top;
}

TVPAllocTag TVPCurrentAllocTag() noexcept
{
	if (g_stack.top <= 0) return TVPAllocTag::Unknown;
	return g_stack.entries[g_stack.top - 1];
}

// ---------------------------------------------------------------------------
// 名前 → enum
// ---------------------------------------------------------------------------
TVPAllocTag TVPAllocTagFromName(const char *name) noexcept
{
	if (!name) return TVPAllocTag::User;
	struct Entry { const char *name; TVPAllocTag tag; };
	static const Entry kTable[] = {
		{"Unknown",        TVPAllocTag::Unknown},
		{"FileCache",      TVPAllocTag::FileCache},
		{"BitmapBits",     TVPAllocTag::BitmapBits},
		{"GraphicsLoader", TVPAllocTag::GraphicsLoader},
		{"Texture",        TVPAllocTag::Texture},
		{"Sound",          TVPAllocTag::Sound},
		{"Movie",          TVPAllocTag::Movie},
		{"TJS2",           TVPAllocTag::TJS2},
		{"User",           TVPAllocTag::User},
	};
	for (const auto &e : kTable) {
		if (std::strcmp(e.name, name) == 0) return e.tag;
	}
	return TVPAllocTag::User;
}

#endif // KRKRZ_ENABLE_MEMSTAT_DETAIL
