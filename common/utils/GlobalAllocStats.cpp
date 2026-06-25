// GlobalAllocStats: グローバル new/delete + TJS_malloc + SDL3 alloc 統合カウンタ
// + TVPPooledAllocator (TLSF) ベースの事前確保プール。
// 詳細は GlobalAllocStats.h と doc/legacy/GlobalAllocationStats.md (案 A) を参照。
//
// KRKRZ_ENABLE_ALLOC_STATS=OFF 時の挙動:
//   - operator new / delete override が消える (CRT default が効く)
//   - TVPKrkrz* wrapper / Sdl* wrapper は素 std::malloc / std::free 直行
//   - 観測 API (Get*Stats / Summarize / Dump / ResetPeak / Initialize) は stub。
//     Snapshot は zeros、Summarize は "(disabled)" を返す
//   - Krkrz pool / TVPPooledAllocator は構築しない

#include "tjsCommHead.h"
#include "GlobalAllocStats.h"
#include "LogIntf.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>

#ifdef KRKRZ_ENABLE_ALLOC_STATS

#include "MemoryAllocatorStats.h"   // TVPFormatBytes / TVPAllocTagName
#include "PooledAllocator.h"        // TVPPooledAllocator (TLSF) + Application.h 経由で TVPAllocTag
#include "AllocTagScope.h"          // TVPCurrentAllocTag()
#include "SysInitIntf.h"            // TVPGetCommandLine

// ---------------------------------------------------------------------------
// 内部状態 (file-scope anonymous namespace = 内部リンケージ)。
// 全 wrapper / operator new / extern "C" 関数から非修飾で参照する。
// ---------------------------------------------------------------------------
namespace {

// tracking スイッチ。Initialize() で true に上げる。これより前は全 alloc が
// 素 std::malloc/free に直行 (header なし、stats なし、pool なし)。
//
// なぜこの構造か:
//   - TVPGetCommandLine から pool size を読みたい (CLI + .cf 両対応)
//   - その読み出しには Application が組み上がっている必要がある
//   - その間 (= SDL_Init 直後 〜 InitPath 完了まで) の alloc を pool に入れる
//     ためには pool 構築自体を後ろ倒し → 結果として「初期は素 malloc」
//   - 早期 alloc を素 malloc に流せば、CRT default と同じ振る舞いで
//     overhead ゼロ。post-init の alloc だけが本機構の対象になる
//
// 注: pre-init で確保したポインタが post-init で free されるケース
// (tracking on 後の magic check) は magic mismatch → 素 std::free 経路で
// 一貫処理される。逆に post-init alloc を pre-init 中に free することは
// 起こらない (tracking が一度 on になったら off にしない前提)。
std::atomic<bool> g_tracking_active{false};

// alloc 結果ポインタの直前に置くヘッダ。free 時に size / 確保経路 (raw vs pool) /
// tag を取り戻すために使う。post-init モードでだけ付与される。
//
// 2 種の magic を使い分ける:
//   kMagicRaw  : pool 未紐付け or pool 構築失敗で素 std::malloc に流れたもの
//   kMagicPool : TVPPooledAllocator::allocate 経由で確保したもの
//
// Tag は alloc 時点での TVPCurrentAllocTag() を保存。free 時に TagSlot[tag] に
// recordFree を当て直す。
constexpr uint32_t kMagicRaw  = 0x4D454D31u; // "1MEM"  (32-bit short)
constexpr uint32_t kMagicPool = 0x4D454D32u; // "2MEM"

struct Header {
	size_t   size;     // 8
	uint32_t magic;    // 4
	uint16_t tag;      // 2 (TVPAllocTag as raw uint16)
	uint16_t pad;      // 2 (alignment, unused)
};
// MSVC x64 / Linux x86_64 共通で size_t = 8 byte なので Header = 16 byte。
// __STDCPP_DEFAULT_NEW_ALIGNMENT__ (典型 8 or 16) を満たすため 16 倍数を確認。
static_assert(sizeof(Header) == 16, "Header must be exactly 16 bytes");
static_assert(sizeof(Header) % 16 == 0, "Header must be 16-byte aligned");

// doc/legacy/MemoryInspection.md §3.2 と同じビン区分。
inline int size_bin_index(size_t size) {
	if (size < 128)                        return 0;
	if (size < 1024)                       return 1;
	if (size < 16ULL * 1024)               return 2;
	if (size < 256ULL * 1024)              return 3;
	if (size < 4ULL * 1024 * 1024)         return 4;
	if (size < 64ULL * 1024 * 1024)        return 5;
	if (size < 1024ULL * 1024 * 1024)      return 6;
	return 7;
}

class Collector {
public:
	void recordAlloc(size_t size, uint16_t tag) {
		alloc_count_.fetch_add(1, std::memory_order_relaxed);
		alloc_bytes_.fetch_add(size, std::memory_order_relaxed);
		uint64_t cur = live_bytes_.fetch_add(size, std::memory_order_relaxed) + size;
		uint64_t pk = peak_bytes_.load(std::memory_order_relaxed);
		while (cur > pk &&
		       !peak_bytes_.compare_exchange_weak(pk, cur, std::memory_order_relaxed)) {}
#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL
		// size histogram
		size_hist_[size_bin_index(size)].fetch_add(1, std::memory_order_relaxed);
		// per-tag
		if (tag < TVPGlobalAllocStats::kMaxTags) {
			auto &slot = tag_slots_[tag];
			slot.alloc_count.fetch_add(1, std::memory_order_relaxed);
			slot.total_allocated.fetch_add(size, std::memory_order_relaxed);
			slot.current_used.fetch_add(size, std::memory_order_relaxed);
		}
#else
		(void)tag;
#endif
	}

	void recordFree(size_t size, uint16_t tag) {
		free_count_.fetch_add(1, std::memory_order_relaxed);
		free_bytes_.fetch_add(size, std::memory_order_relaxed);
		live_bytes_.fetch_sub(size, std::memory_order_relaxed);
#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL
		if (tag < TVPGlobalAllocStats::kMaxTags) {
			auto &slot = tag_slots_[tag];
			slot.free_count.fetch_add(1, std::memory_order_relaxed);
			slot.total_freed.fetch_add(size, std::memory_order_relaxed);
			slot.current_used.fetch_sub(size, std::memory_order_relaxed);
		}
#else
		(void)tag;
#endif
	}

	TVPGlobalAllocStats::Snapshot snapshot() const {
		TVPGlobalAllocStats::Snapshot s;
		s.alloc_count = alloc_count_.load(std::memory_order_relaxed);
		s.alloc_bytes = alloc_bytes_.load(std::memory_order_relaxed);
		s.free_count  = free_count_.load(std::memory_order_relaxed);
		s.free_bytes  = free_bytes_.load(std::memory_order_relaxed);
		s.live_bytes  = live_bytes_.load(std::memory_order_relaxed);
		s.peak_bytes  = peak_bytes_.load(std::memory_order_relaxed);
		if (iTVPMemoryAllocator *alloc = pool_.load(std::memory_order_acquire)) {
			s.pool_capacity  = alloc->capacity();
			s.pool_used      = alloc->used();
			s.pool_peak      = alloc->getStats().peak_used;
			// fallback 情報。iTVPMemoryAllocator のデフォルト実装は 0 を返す。
			s.fallback_count = alloc->fallbackAllocCount();
			s.fallback_bytes = alloc->fallbackBytesInUse();
		}
#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL
		for (int i = 0; i < TVPGlobalAllocStats::kSizeHistBins; ++i) {
			s.alloc_size_hist[i] = size_hist_[i].load(std::memory_order_relaxed);
		}
		for (int i = 0; i < TVPGlobalAllocStats::kMaxTags; ++i) {
			const auto &slot = tag_slots_[i];
			auto &out = s.tag_stats[i];
			out.alloc_count     = slot.alloc_count.load(std::memory_order_relaxed);
			out.total_allocated = slot.total_allocated.load(std::memory_order_relaxed);
			out.free_count      = slot.free_count.load(std::memory_order_relaxed);
			out.total_freed     = slot.total_freed.load(std::memory_order_relaxed);
			out.current_used    = slot.current_used.load(std::memory_order_relaxed);
		}
#endif
		return s;
	}

	void resetPeak() {
		uint64_t cur = live_bytes_.load(std::memory_order_relaxed);
		peak_bytes_.store(cur, std::memory_order_relaxed);
		if (iTVPMemoryAllocator *alloc = pool_.load(std::memory_order_acquire)) {
			alloc->resetPeak();
		}
	}

	void bindPool(iTVPMemoryAllocator *p, const char *name) {
		name_ = name ? name : "?";
		// release で構造体の中身が他スレッドにも見えるようにする
		pool_.store(p, std::memory_order_release);
	}

	iTVPMemoryAllocator *pool() const {
		return pool_.load(std::memory_order_acquire);
	}

	const char *name() const { return name_; }

	// fallback (pool 枯渇 → system malloc) を最初に検知したスレッドだけ true を返す。
	bool firstOverflowDetected() {
		bool was = warned_overflow_.exchange(true, std::memory_order_relaxed);
		return !was;
	}

private:
	std::atomic<uint64_t> alloc_count_{0};
	std::atomic<uint64_t> alloc_bytes_{0};
	std::atomic<uint64_t> free_count_{0};
	std::atomic<uint64_t> free_bytes_{0};
	std::atomic<uint64_t> live_bytes_{0};
	std::atomic<uint64_t> peak_bytes_{0};

	std::atomic<uint64_t> size_hist_[TVPGlobalAllocStats::kSizeHistBins]{};

	struct TagSlotAtomic {
		std::atomic<uint64_t> alloc_count{0};
		std::atomic<uint64_t> total_allocated{0};
		std::atomic<uint64_t> free_count{0};
		std::atomic<uint64_t> total_freed{0};
		std::atomic<uint64_t> current_used{0};
	};
	TagSlotAtomic tag_slots_[TVPGlobalAllocStats::kMaxTags]{};

	std::atomic<iTVPMemoryAllocator *> pool_{nullptr};
	std::atomic<bool>                 warned_overflow_{false};
	const char                       *name_ = "?";
};

// zero-init される atomic は static 初期化順序問題から免れる。
Collector g_krkrz;
Collector g_sdl;

// pre-init: 素 malloc/free 直行。tracking スイッチが立つ前は本パスのみ通る。
inline void *do_malloc(Collector &c, size_t size) {
	if (!g_tracking_active.load(std::memory_order_acquire)) {
		// header なし、stats 触らず、pool 触らず。CRT default と同等。
		return std::malloc(size);
	}

	// post-init: header + 経路振り分け。tag は thread-local stack top を採用。
	uint16_t tag = static_cast<uint16_t>(TVPCurrentAllocTag());
	size_t total = size + sizeof(Header);
	void *raw = nullptr;
	uint32_t magic = kMagicRaw;

	if (iTVPMemoryAllocator *alloc = c.pool()) {
		// fallback 検出を行う (非 pool ベース allocator は常に 0 を返すので無影響)
		uint64_t fb_before = alloc->fallbackAllocCount();
		raw = alloc->allocate(total);
		uint64_t fb_after  = alloc->fallbackAllocCount();
		// fb_after > fb_before なら、pool 容量を初めて (or 累積で) 超えた。
		// 集計として overflow が起きた事実は確かなので 1 度だけ警告を出す。
		if (fb_after > fb_before && c.firstOverflowDetected()) {
			TVPLOG_WARNING("GlobalAllocStats[{}]: pool capacity exceeded "
			               "({} bytes); subsequent allocs fall back to system malloc",
			               c.name(), alloc->capacity());
		}
		if (raw) magic = kMagicPool;
	}

	if (!raw) {
		raw = std::malloc(total);
		magic = kMagicRaw;
	}

	if (!raw) return nullptr;
	Header *h = static_cast<Header *>(raw);
	h->size  = size;
	h->magic = magic;
	h->tag   = tag;
	h->pad   = 0;
	c.recordAlloc(size, tag);
	return static_cast<char *>(raw) + sizeof(Header);
}

inline void do_free(Collector &c, void *p) {
	if (!p) return;
	if (!g_tracking_active.load(std::memory_order_acquire)) {
		// pre-init モード: header 付与もしてないので素の free に直行。
		std::free(p);
		return;
	}

	// post-init モード: header magic で経路を識別。
	Header *h = reinterpret_cast<Header *>(static_cast<char *>(p) - sizeof(Header));
	if (h->magic == kMagicPool) {
		size_t size = h->size;
		uint16_t tag = h->tag;
		c.recordFree(size, tag);
		// pool は bindPool 後 unbind しない前提。
		iTVPMemoryAllocator *alloc = c.pool();
		if (alloc) {
			alloc->free(h);
		} else {
			std::free(h);
		}
		return;
	}
	if (h->magic == kMagicRaw) {
		c.recordFree(h->size, h->tag);
		std::free(h);
		return;
	}
	// magic 不一致 = pre-init で確保されたポインタ (header なし)、または
	// 別アロケータ (プラグイン CRT 等) が返したもの。素の free に直行。
	std::free(p);
}

inline void *do_calloc(Collector &c, size_t n, size_t size) {
	if (!g_tracking_active.load(std::memory_order_acquire)) {
		return std::calloc(n, size);
	}
	size_t total = n * size; // overflow check 省略 (SDL3 calloc も同等)
	void *p = do_malloc(c, total);
	if (p && total > 0) std::memset(p, 0, total);
	return p;
}

inline void *do_realloc(Collector &c, void *p, size_t size) {
	if (!g_tracking_active.load(std::memory_order_acquire)) {
		return std::realloc(p, size);
	}
	if (!p) return do_malloc(c, size);
	if (size == 0) {
		do_free(c, p);
		return nullptr;
	}
	Header *oldH = reinterpret_cast<Header *>(static_cast<char *>(p) - sizeof(Header));
	if (oldH->magic != kMagicRaw && oldH->magic != kMagicPool) {
		// pre-init で確保 / 別アロケータ由来。素 realloc に委譲。
		// 結果も header なしのままなので、後の free でも magic mismatch
		// → std::free に流れて整合する。
		return std::realloc(p, size);
	}
	size_t old_size = oldH->size;
	void *q = do_malloc(c, size);
	if (!q) return nullptr; // 元の p は触らずそのまま (標準準拠)
	std::memcpy(q, p, old_size < size ? old_size : size);
	do_free(c, p);
	return q;
}

// CLI / .cf から `-<flag>=N` (MB) を取り、バイト数で返す。
//   "none" / "off" / "0" → 0 (= pool 無効化)
//   それ以外の正の整数  → N MB をバイトに換算
//   未指定 / parse 失敗 → default_mb MB をバイトに換算
size_t read_pool_size_mb(const tjs_char *flag, size_t default_mb) {
	tTJSVariant val;
	if (TVPGetCommandLine(flag, &val)) {
		ttstr str(val);
		if (str == TJS_W("none") || str == TJS_W("off") || str == TJS_W("0")) {
			return 0;
		}
		tjs_int64 mb = (tjs_int64)val;
		if (mb > 0) return (size_t)mb * 1024ULL * 1024ULL;
	}
	return default_mb * 1024ULL * 1024ULL;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 観測 / 操作 API
// ---------------------------------------------------------------------------
namespace TVPGlobalAllocStats {

Snapshot GetKrkrzStats() { return g_krkrz.snapshot(); }
Snapshot GetSdlStats()   { return g_sdl.snapshot(); }
void ResetKrkrzPeak()    { g_krkrz.resetPeak(); }
void ResetSdlPeak()      { g_sdl.resetPeak(); }

void *SdlMalloc(size_t size)             { return do_malloc(g_sdl, size); }
void *SdlCalloc(size_t nmemb, size_t s)  { return do_calloc(g_sdl, nmemb, s); }
void *SdlRealloc(void *p, size_t size)   { return do_realloc(g_sdl, p, size); }
void  SdlFree(void *p)                   { do_free(g_sdl, p); }

void Initialize() {
	static std::atomic<bool> initialized{false};
	bool was = initialized.exchange(true, std::memory_order_relaxed);
	if (was) {
		TVPLOG_WARNING("GlobalAllocStats::Initialize called twice; ignoring");
		return;
	}

	// Krkrz allocator を Application 経由で取得。
	// プラットフォーム固有の allocator (例: PS5 Direct Memory) を使いたい場合は
	// Application サブクラスで CreateKrkrzAllocator() をオーバーライドする。
	// デフォルト実装は TVPPooledAllocator (TLSF) を返す。
	//
	// pool 構築は new TVPPooledAllocator(...) で本 TU の operator new override
	// を経由するが、まだ tracking off なので素 malloc 直行 = 再帰の心配なし。
	iTVPMemoryAllocator *krkrz_alloc = Application ? Application->CreateKrkrzAllocator() : nullptr;
	g_krkrz.bindPool(krkrz_alloc, "Krkrz");
	size_t krkrz_cap = krkrz_alloc ? krkrz_alloc->capacity() : 0;

#if defined(__GENERIC__) && defined(KRKRZ_SDLMEMORY_STAT)
	// __GENERIC__ build (SDL3 / LIB) かつ KRKRZ_SDLMEMORY_STAT=ON のときだけ
	// SDL pool を立ち上げる。OFF (デフォルト) のときは SDL_SetMemoryFunctions
	// 自体を仕掛けないので Sdl* wrapper は呼ばれず、pool 確保は単に無駄。
	// CLI / .cf から pool size を取得。none/off/0 で pool 無効化。
	size_t sdl_bytes = read_pool_size_mb(TJS_W("-sdlpoolsize"), 64);
	if (sdl_bytes > 0) {
		auto *pool = new TVPPooledAllocator(sdl_bytes, "GlobalSdl",
		                                    TVPAllocTag::Unknown);
		g_sdl.bindPool(pool, "SDL");
	} else {
		g_sdl.bindPool(nullptr, "SDL");
	}
#else
	// WINVER または KRKRZ_SDLMEMORY_STAT=OFF: SDL collector は使われない。
	g_sdl.bindPool(nullptr, "SDL");
#endif

	// tracking を on。これ以降の alloc は header + 経路振り分けが効く。
	// release で pool_ store を含むすべての書き込みが他スレッドにも見える状態にする。
	g_tracking_active.store(true, std::memory_order_release);
#if defined(__GENERIC__) && defined(KRKRZ_SDLMEMORY_STAT)
	size_t sdl_cap = g_sdl.pool() ? g_sdl.pool()->capacity() : 0;
	TVPLOG_INFO("GlobalAllocStats: tracking activated (Krkrz pool={} bytes, SDL pool={} bytes)",
	            krkrz_cap, sdl_cap);
#else
	TVPLOG_INFO("GlobalAllocStats: tracking activated (Krkrz pool={} bytes, SDL pool=disabled)",
	            krkrz_cap);
#endif
}

std::string Summarize() {
	auto k = GetKrkrzStats();
	char buf[64];
	std::string out;
	if (!g_tracking_active.load(std::memory_order_relaxed)) {
		out = "(GlobalAllocStats: tracking not yet activated)";
		return out;
	}
	out = "Krkrz: live=";
	out += TVPFormatBytes(k.live_bytes);
	out += " peak=";
	out += TVPFormatBytes(k.peak_bytes);
	out += " alloc_n=";
	std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)k.alloc_count);
	out += buf;
	if (k.pool_capacity > 0) {
		out += " pool=";
		out += TVPFormatBytes(k.pool_used);
		out += "/";
		out += TVPFormatBytes(k.pool_capacity);
		if (k.fallback_count > 0) {
			std::snprintf(buf, sizeof(buf), " fb=%llu",
			              (unsigned long long)k.fallback_count);
			out += buf;
		}
	}
#ifdef KRKRZ_SDLMEMORY_STAT
	auto s = GetSdlStats();
	out += " | SDL: live=";
	out += TVPFormatBytes(s.live_bytes);
	out += " peak=";
	out += TVPFormatBytes(s.peak_bytes);
	out += " alloc_n=";
	std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)s.alloc_count);
	out += buf;
	if (s.pool_capacity > 0) {
		out += " pool=";
		out += TVPFormatBytes(s.pool_used);
		out += "/";
		out += TVPFormatBytes(s.pool_capacity);
		if (s.fallback_count > 0) {
			std::snprintf(buf, sizeof(buf), " fb=%llu",
			              (unsigned long long)s.fallback_count);
			out += buf;
		}
	}
#endif
	return out;
}

void Dump() {
	if (!g_tracking_active.load(std::memory_order_relaxed)) {
		TVPLOG_INFO("GlobalAlloc: tracking not yet activated");
		return;
	}
	auto k = GetKrkrzStats();
	TVPLOG_INFO("GlobalAlloc[Krkrz] live={} peak={} total_alloc={} total_freed={} alloc_n={} free_n={}",
	            TVPFormatBytes(k.live_bytes), TVPFormatBytes(k.peak_bytes),
	            TVPFormatBytes(k.alloc_bytes), TVPFormatBytes(k.free_bytes),
	            k.alloc_count, k.free_count);
	if (k.pool_capacity > 0) {
		TVPLOG_INFO("GlobalAlloc[Krkrz] pool used={} peak={} cap={} fallback_n={} fallback_live={}",
		            TVPFormatBytes(k.pool_used), TVPFormatBytes(k.pool_peak),
		            TVPFormatBytes(k.pool_capacity),
		            k.fallback_count, TVPFormatBytes(k.fallback_bytes));
	}
#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL
	// size histogram (1 件でもあれば 1 行で)
	{
		bool any = false;
		for (int i = 0; i < kSizeHistBins; ++i) {
			if (k.alloc_size_hist[i] > 0) { any = true; break; }
		}
		if (any) {
			static const char *kBin[kSizeHistBins] =
				{"<128","<1K","<16K","<256K","<4M","<64M","<1G",">=1G"};
			TVPLOG_INFO("GlobalAlloc[Krkrz]   size_hist: {}={} {}={} {}={} {}={} {}={} {}={} {}={} {}={}",
			            kBin[0], k.alloc_size_hist[0],
			            kBin[1], k.alloc_size_hist[1],
			            kBin[2], k.alloc_size_hist[2],
			            kBin[3], k.alloc_size_hist[3],
			            kBin[4], k.alloc_size_hist[4],
			            kBin[5], k.alloc_size_hist[5],
			            kBin[6], k.alloc_size_hist[6],
			            kBin[7], k.alloc_size_hist[7]);
		}
	}
	// tag 別 (alloc_count > 0 のものだけ)
	for (uint16_t i = 0; i < static_cast<uint16_t>(TVPAllocTag::_Count); ++i) {
		const auto &t = k.tag_stats[i];
		if (t.alloc_count == 0) continue;
		TVPLOG_INFO("GlobalAlloc[Krkrz]   tag[{}] alloc={} free={} used={} total_alloc={} total_freed={}",
		            TVPAllocTagName(static_cast<TVPAllocTag>(i)),
		            t.alloc_count, t.free_count,
		            TVPFormatBytes(t.current_used),
		            TVPFormatBytes(t.total_allocated),
		            TVPFormatBytes(t.total_freed));
	}
#endif // KRKRZ_ENABLE_MEMSTAT_DETAIL
#ifdef KRKRZ_SDLMEMORY_STAT
	auto s = GetSdlStats();
	TVPLOG_INFO("GlobalAlloc[SDL]   live={} peak={} total_alloc={} total_freed={} alloc_n={} free_n={}",
	            TVPFormatBytes(s.live_bytes), TVPFormatBytes(s.peak_bytes),
	            TVPFormatBytes(s.alloc_bytes), TVPFormatBytes(s.free_bytes),
	            s.alloc_count, s.free_count);
	if (s.pool_capacity > 0) {
		TVPLOG_INFO("GlobalAlloc[SDL]   pool used={} peak={} cap={} fallback_n={} fallback_live={}",
		            TVPFormatBytes(s.pool_used), TVPFormatBytes(s.pool_peak),
		            TVPFormatBytes(s.pool_capacity),
		            s.fallback_count, TVPFormatBytes(s.fallback_bytes));
	}
#endif
}

} // namespace TVPGlobalAllocStats

// ---------------------------------------------------------------------------
// extern "C" wrapper (tjsConfig.h の TJS_malloc redirect 先)
// ---------------------------------------------------------------------------
extern "C" void *TVPKrkrzMalloc(size_t size)               { return do_malloc(g_krkrz, size); }
extern "C" void *TVPKrkrzCalloc(size_t nmemb, size_t size) { return do_calloc(g_krkrz, nmemb, size); }
extern "C" void *TVPKrkrzRealloc(void *p, size_t size)     { return do_realloc(g_krkrz, p, size); }
extern "C" void  TVPKrkrzFree(void *p)                     { do_free(g_krkrz, p); }

// ---------------------------------------------------------------------------
// グローバル operator new / delete override (replaceable functions)
// ---------------------------------------------------------------------------
// MSVC /MT 環境では本 TU の定義が CRT の default 実装を上書きする。
// pre-init 時 (= Initialize 未呼出) は do_malloc/free が素 malloc/free に
// 直行するので、観測フックは事実上「Initialize 後に有効化」される形になる。

void *operator new(std::size_t size) {
	if (size == 0) size = 1;  // C++ 標準: size 0 でも有効なポインタを返す
	void *p = do_malloc(g_krkrz, size);
	if (!p) throw std::bad_alloc();
	return p;
}

void *operator new[](std::size_t size) {
	if (size == 0) size = 1;  // C++ 標準: size 0 でも有効なポインタを返す
	void *p = do_malloc(g_krkrz, size);
	if (!p) throw std::bad_alloc();
	return p;
}

void *operator new(std::size_t size, const std::nothrow_t&) noexcept {
	if (size == 0) size = 1;  // C++ 標準: size 0 でも有効なポインタを返す
	return do_malloc(g_krkrz, size);
}

void *operator new[](std::size_t size, const std::nothrow_t&) noexcept {
	if (size == 0) size = 1;  // C++ 標準: size 0 でも有効なポインタを返す
	return do_malloc(g_krkrz, size);
}

void operator delete(void *p) noexcept                         { do_free(g_krkrz, p); }
void operator delete[](void *p) noexcept                       { do_free(g_krkrz, p); }
void operator delete(void *p, std::size_t) noexcept            { do_free(g_krkrz, p); }
void operator delete[](void *p, std::size_t) noexcept          { do_free(g_krkrz, p); }
void operator delete(void *p, const std::nothrow_t&) noexcept  { do_free(g_krkrz, p); }
void operator delete[](void *p, const std::nothrow_t&) noexcept { do_free(g_krkrz, p); }

#else // !KRKRZ_ENABLE_ALLOC_STATS

// ---------------------------------------------------------------------------
// KRKRZ_ENABLE_ALLOC_STATS=OFF: 観測機構を完全に除去した stub 実装。
// operator new override / TJS_malloc redirect / pool / 観測 API は全て無効化。
// TJS_malloc / SDL_malloc wrapper は素の std::malloc / std::free 直行となり、
// CRT default の挙動と同等。
// ---------------------------------------------------------------------------
namespace TVPGlobalAllocStats {

Snapshot GetKrkrzStats() { return Snapshot{}; }
Snapshot GetSdlStats()   { return Snapshot{}; }
void ResetKrkrzPeak()    {}
void ResetSdlPeak()      {}

void *SdlMalloc(size_t size)                 { return std::malloc(size); }
void *SdlCalloc(size_t nmemb, size_t size)   { return std::calloc(nmemb, size); }
void *SdlRealloc(void *p, size_t size)       { return std::realloc(p, size); }
void  SdlFree(void *p)                       { std::free(p); }

void Initialize() {
	TVPLOG_INFO("GlobalAllocStats: disabled at compile time (KRKRZ_ENABLE_ALLOC_STATS=OFF)");
}

std::string Summarize() {
	return std::string("(GlobalAllocStats: disabled)");
}

void Dump() {
	TVPLOG_INFO("GlobalAlloc: disabled at compile time (KRKRZ_ENABLE_ALLOC_STATS=OFF)");
}

} // namespace TVPGlobalAllocStats

extern "C" void *TVPKrkrzMalloc(size_t size)               { return std::malloc(size); }
extern "C" void *TVPKrkrzCalloc(size_t nmemb, size_t size) { return std::calloc(nmemb, size); }
extern "C" void *TVPKrkrzRealloc(void *p, size_t size)     { return std::realloc(p, size); }
extern "C" void  TVPKrkrzFree(void *p)                     { std::free(p); }

#endif // KRKRZ_ENABLE_ALLOC_STATS
