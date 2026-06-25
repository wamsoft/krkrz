#pragma once

// iTVPMemoryAllocator 向け統計集計ヘルパ。
// doc/legacy/MemoryBudgetNegotiation.md §11 (L1 軽量カウンタ + L2 サイズビン) を
// 共通実装として切り出したもの。各 allocator は本クラスを composition で持ち、
// allocate / free 時に recordAlloc / recordFree を呼ぶ。getStats() は snapshot()
// を呼ぶだけで済む。
//
// Mode::Sized   : free 時に size が判明する allocator 用 (例: BasicFileAllocator
//                 のような header 前置型)。current_used / peak_used / total_freed を
//                 正確に追跡。
// Mode::Unsized : free 時に size が取れない allocator 用 (例: BitmapBitsAlloc 経由
//                 の各 allocator は上位で size を保持しているが iTVPMemoryAllocator
//                 レベルでは渡されない)。current_used / peak_used / total_freed は
//                 SIZE_MAX のまま (= 未対応) を返す。

#include "Application.h" // iTVPMemoryAllocator::Stats
#include "LogIntf.h"     // TVPLOG_WARNING / TVPLOG_INFO

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

class tTVPMemoryAllocatorStatsCollector {
public:
	enum class Mode { Sized, Unsized };

	explicit tTVPMemoryAllocatorStatsCollector(Mode mode = Mode::Sized)
		: mode_(mode) {}

	// L2 ビン区分 (doc/legacy/MemoryBudgetNegotiation.md §11.1)。
	// ビン: <128, <1K, <16K, <256K, <4M, <64M, <1G, ≥1G。
	static int sizeBinIndex(size_t size) {
		if (size < 128)                        return 0;
		if (size < 1024)                       return 1;
		if (size < 16ULL * 1024)               return 2;
		if (size < 256ULL * 1024)              return 3;
		if (size < 4ULL * 1024 * 1024)         return 4;
		if (size < 64ULL * 1024 * 1024)        return 5;
		if (size < 1024ULL * 1024 * 1024)      return 6;
		return 7;
	}

	// tag を渡さないバージョン。tag = Unknown 扱い。
	void recordAlloc(size_t size) { recordAlloc(size, TVPAllocTag::Unknown); }

	void recordAlloc(size_t size, TVPAllocTag tag) {
#ifdef KRKRZ_ENABLE_ALLOCATOR_STATS
		if (mode_ == Mode::Sized) {
			size_t cur = current_used_.fetch_add(size, std::memory_order_relaxed) + size;
			size_t peak = peak_used_.load(std::memory_order_relaxed);
			while (cur > peak &&
			       !peak_used_.compare_exchange_weak(peak, cur, std::memory_order_relaxed)) {}
		}
		total_allocated_.fetch_add(size, std::memory_order_relaxed);
		alloc_count_.fetch_add(1, std::memory_order_relaxed);
		alloc_size_hist_[sizeBinIndex(size)].fetch_add(1, std::memory_order_relaxed);

		// tag 別 (alloc 側のみ。free 側は T4 でヘッダ拡張時に対応予定)。
		auto &slot = tag_slots_[static_cast<size_t>(tag)];
		slot.alloc_count.fetch_add(1, std::memory_order_relaxed);
		slot.total_allocated.fetch_add(size, std::memory_order_relaxed);
		if (mode_ == Mode::Sized) {
			slot.current_used.fetch_add(size, std::memory_order_relaxed);
		}
#else
		(void)size; (void)tag;
#endif
	}

	// size_or_zero: Sized mode では実 size を、Unsized mode では 0 (= 不明) を渡す。
	// tag を渡さないバージョン (= Unknown tag。Unsized mode 等から)。
	void recordFree(size_t size_or_zero) { recordFree(size_or_zero, TVPAllocTag::Unknown); }

	// tag 付き free。Sized mode 側でヘッダから tag を回収できる場合に呼ぶ。
	void recordFree(size_t size_or_zero, TVPAllocTag tag) {
#ifdef KRKRZ_ENABLE_ALLOCATOR_STATS
		if (mode_ == Mode::Sized && size_or_zero > 0) {
			current_used_.fetch_sub(size_or_zero, std::memory_order_relaxed);
			total_freed_.fetch_add(size_or_zero, std::memory_order_relaxed);
		}
		free_count_.fetch_add(1, std::memory_order_relaxed);

		// tag 別 (Unknown 含む全 tag に対して計上)。
		auto &slot = tag_slots_[static_cast<size_t>(tag)];
		slot.free_count.fetch_add(1, std::memory_order_relaxed);
		if (mode_ == Mode::Sized && size_or_zero > 0) {
			slot.current_used.fetch_sub(size_or_zero, std::memory_order_relaxed);
			slot.total_freed.fetch_add(size_or_zero, std::memory_order_relaxed);
		}
#else
		(void)size_or_zero; (void)tag;
#endif
	}

	iTVPMemoryAllocator::Stats snapshot() const {
		iTVPMemoryAllocator::Stats s;
#ifdef KRKRZ_ENABLE_ALLOCATOR_STATS
		if (mode_ == Mode::Sized) {
			s.current_used = current_used_.load(std::memory_order_relaxed);
			s.peak_used    = peak_used_.load(std::memory_order_relaxed);
			s.total_freed  = total_freed_.load(std::memory_order_relaxed);
		}
		// Unsized mode では Stats のデフォルト (current_used = peak_used = SIZE_MAX,
		// total_freed = 0) のまま放置 → "未対応" を意味する。
		s.total_allocated = total_allocated_.load(std::memory_order_relaxed);
		s.alloc_count     = alloc_count_.load(std::memory_order_relaxed);
		s.free_count      = free_count_.load(std::memory_order_relaxed);
		for (size_t i = 0; i < s.alloc_size_hist.size(); ++i) {
			s.alloc_size_hist[i] = alloc_size_hist_[i].load(std::memory_order_relaxed);
		}
#else
		// 全フィールド「不明」扱い (Stats のデフォルトコンストラクト値)。
		// current_used / peak_used = SIZE_MAX、total_freed = 0、histogram は全 0。
#endif
		return s;
	}

	iTVPMemoryAllocator::TagStats tagSnapshot(TVPAllocTag tag) const {
		iTVPMemoryAllocator::TagStats t;
#ifdef KRKRZ_ENABLE_ALLOCATOR_STATS
		const auto &slot = tag_slots_[static_cast<size_t>(tag)];
		t.alloc_count     = slot.alloc_count.load(std::memory_order_relaxed);
		t.total_allocated = slot.total_allocated.load(std::memory_order_relaxed);
		t.free_count      = slot.free_count.load(std::memory_order_relaxed);
		if (mode_ == Mode::Sized) {
			t.current_used = slot.current_used.load(std::memory_order_relaxed);
			t.total_freed  = slot.total_freed.load(std::memory_order_relaxed);
		}
		// Unsized mode では current_used / total_freed = 0 (= 不明値)。
		// call-site で tag 別 alloc_count と free_count の差を見ればリーク傾向は分かる。
#else
		(void)tag;
#endif
		return t;
	}

	// Sized mode 用に used() を直接取得 (iTVPMemoryAllocator::used() の実装に流用)。
	// Unsized mode / OFF 時は SIZE_MAX を返す ("不明" マーカー)。
	size_t used() const {
#ifdef KRKRZ_ENABLE_ALLOCATOR_STATS
		if (mode_ == Mode::Sized) {
			return current_used_.load(std::memory_order_relaxed);
		}
#endif
		return SIZE_MAX;
	}

	// peak_used を current_used に揃え直す。Sized mode のみ意味あり。
	// (Unsized mode / OFF 時は peak_used は未使用なので no-op)。
	// レース時、間に到達した瞬間の peak は取りこぼし得るが用途的に問題なし。
	void resetPeak() {
#ifdef KRKRZ_ENABLE_ALLOCATOR_STATS
		if (mode_ != Mode::Sized) return;
		size_t cur = current_used_.load(std::memory_order_relaxed);
		peak_used_.store(cur, std::memory_order_relaxed);
#endif
	}

private:
	struct TagSlot {
		std::atomic<size_t>   current_used{0};   // Sized mode + tag-aware free のみ
		std::atomic<uint64_t> alloc_count{0};
		std::atomic<uint64_t> total_allocated{0};
		std::atomic<uint64_t> free_count{0};
		std::atomic<uint64_t> total_freed{0};    // Sized mode のみ
	};
	static constexpr size_t kTagCount = static_cast<size_t>(TVPAllocTag::_Count);

	const Mode mode_;
	std::atomic<size_t>   current_used_{0};
	std::atomic<size_t>   peak_used_{0};
	std::atomic<uint64_t> total_allocated_{0};
	std::atomic<uint64_t> total_freed_{0};
	std::atomic<uint64_t> alloc_count_{0};
	std::atomic<uint64_t> free_count_{0};
	std::array<std::atomic<uint64_t>, 8> alloc_size_hist_{};
	std::array<TagSlot, kTagCount> tag_slots_{};
};

// バイト数を人が読みやすい形式に整形 ("12.34MB" / "456.78KB" / "789B")。
// SIZE_MAX (= 未対応マーカー) は "?" を返す。
inline std::string TVPFormatBytes(uint64_t bytes) {
	if (bytes == SIZE_MAX) return std::string("?");
	char buf[32];
	if (bytes >= (1ULL << 30)) {
		std::snprintf(buf, sizeof(buf), "%.2fGB", bytes / double(1ULL << 30));
	} else if (bytes >= (1ULL << 20)) {
		std::snprintf(buf, sizeof(buf), "%.2fMB", bytes / double(1ULL << 20));
	} else if (bytes >= (1ULL << 10)) {
		std::snprintf(buf, sizeof(buf), "%.2fKB", bytes / double(1ULL << 10));
	} else {
		std::snprintf(buf, sizeof(buf), "%lluB", static_cast<unsigned long long>(bytes));
	}
	return std::string(buf);
}

inline const char *TVPAllocTagName(TVPAllocTag tag) {
	switch (tag) {
		case TVPAllocTag::Unknown:        return "Unknown";
		case TVPAllocTag::FileCache:      return "FileCache";
		case TVPAllocTag::BitmapBits:     return "BitmapBits";
		case TVPAllocTag::GraphicsLoader: return "GraphicsLoader";
		case TVPAllocTag::Texture:        return "Texture";
		case TVPAllocTag::Sound:          return "Sound";
		case TVPAllocTag::Movie:          return "Movie";
		case TVPAllocTag::TJS2:           return "TJS2";
		case TVPAllocTag::User:           return "User";
		default: return "?";
	}
}

// Allocator の現状を Stats / TagStats / サイズヒストグラム込みでログ出力。
// TVPHeapDump / 周期ダンプ / System.dumpHeap / REPL コマンドからの共通入口。
inline void TVPDumpAllocatorStats(const char *name, iTVPMemoryAllocator *alloc) {
#ifndef KRKRZ_ENABLE_ALLOCATOR_STATS
	(void)alloc;
	TVPLOG_INFO("MemoryAllocator [{}] disabled at compile time (KRKRZ_ENABLE_ALLOCATOR_STATS=OFF)",
	            name);
	return;
#endif
	if (!alloc) {
		TVPLOG_INFO("MemoryAllocator [{}] (null)", name);
		return;
	}
	auto stats = alloc->getStats();
	TVPLOG_INFO("MemoryAllocator [{}] cap={} used={} peak={} total_alloc={} total_freed={} alloc_n={} free_n={}",
	            name,
	            TVPFormatBytes(alloc->capacity()),
	            TVPFormatBytes(stats.current_used),
	            TVPFormatBytes(stats.peak_used),
	            TVPFormatBytes(stats.total_allocated),
	            TVPFormatBytes(stats.total_freed),
	            stats.alloc_count, stats.free_count);

	// L2 サイズビン (1 件でもあれば 1 行で出す)
	bool any_hist = false;
	for (size_t i = 0; i < stats.alloc_size_hist.size(); ++i) {
		if (stats.alloc_size_hist[i] > 0) { any_hist = true; break; }
	}
	if (any_hist) {
		static const char *kBin[8] = {"<128","<1K","<16K","<256K","<4M","<64M","<1G",">=1G"};
		TVPLOG_INFO("  size_hist: {}={} {}={} {}={} {}={} {}={} {}={} {}={} {}={}",
		            kBin[0], stats.alloc_size_hist[0],
		            kBin[1], stats.alloc_size_hist[1],
		            kBin[2], stats.alloc_size_hist[2],
		            kBin[3], stats.alloc_size_hist[3],
		            kBin[4], stats.alloc_size_hist[4],
		            kBin[5], stats.alloc_size_hist[5],
		            kBin[6], stats.alloc_size_hist[6],
		            kBin[7], stats.alloc_size_hist[7]);
	}

	// L3 tag 別 (alloc_count > 0 のものだけ)
	for (uint16_t i = 0; i < static_cast<uint16_t>(TVPAllocTag::_Count); ++i) {
		auto tag = static_cast<TVPAllocTag>(i);
		auto t = alloc->getTagStats(tag);
		if (t.alloc_count == 0) continue;
		TVPLOG_INFO("  tag[{}] alloc={} free={} used={} total_alloc={} total_freed={}",
		            TVPAllocTagName(tag),
		            t.alloc_count, t.free_count,
		            TVPFormatBytes(t.current_used),
		            TVPFormatBytes(t.total_allocated),
		            TVPFormatBytes(t.total_freed));
	}
}

// 1 行サマリ用文字列を返す。REPL コマンド等から ic_printf 等に流す用途。
inline std::string TVPSummarizeAllocator(const char *name, iTVPMemoryAllocator *alloc) {
#ifndef KRKRZ_ENABLE_ALLOCATOR_STATS
	(void)alloc;
	return std::string(name) + ": (disabled)";
#endif
	if (!alloc) {
		return std::string(name) + ": (null)";
	}
	auto stats = alloc->getStats();
	std::string out(name);
	out += ": used=";
	out += TVPFormatBytes(stats.current_used);
	out += " peak=";
	out += TVPFormatBytes(stats.peak_used);
	out += " alloc_n=";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats.alloc_count);
	out += buf;
	out += " free_n=";
	std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats.free_count);
	out += buf;
	return out;
}

// アプリ終了直前の tag 別リーク推定 (doc/legacy/MemoryBudgetNegotiation.md §11.3 / T4)。
// 各 allocator の getTagStats を全 tag 巡回し、alloc_count != free_count または
// current_used > 0 の tag を WARNING ログに出す。
// 呼び出しは tTVPAtExit (TVP_ATEXIT_PRI_CLEANUP - 1 推奨) から。
inline void TVPDumpAllocatorLeakReport(const char *name, iTVPMemoryAllocator *alloc) {
#ifndef KRKRZ_ENABLE_ALLOCATOR_STATS
	(void)name; (void)alloc;
	return; // 統計が取れない以上リーク推定もできない
#endif
	if (!alloc) return;
	auto stats = alloc->getStats();
	bool any_leak = false;
	for (uint16_t i = 0; i < static_cast<uint16_t>(TVPAllocTag::_Count); ++i) {
		auto tag = static_cast<TVPAllocTag>(i);
		auto t = alloc->getTagStats(tag);
		if (t.alloc_count == 0) continue; // tag 未使用は skip
		bool leak = false;
		if (t.alloc_count != t.free_count) leak = true;
		if (t.current_used != 0) leak = true; // Sized + tag-aware free のみ意味あり
		if (leak) {
			TVPLOG_WARNING("MemoryAllocator leak [{}] tag={} alloc={} free={} current={} total_alloc={} total_freed={}",
			               name, TVPAllocTagName(tag),
			               t.alloc_count, t.free_count,
			               TVPFormatBytes(t.current_used),
			               TVPFormatBytes(t.total_allocated),
			               TVPFormatBytes(t.total_freed));
			any_leak = true;
		}
	}
	if (!any_leak) {
		TVPLOG_INFO("MemoryAllocator [{}] clean (alloc={} free={} peak={})",
		            name, stats.alloc_count, stats.free_count,
		            TVPFormatBytes(stats.peak_used));
	}
}
