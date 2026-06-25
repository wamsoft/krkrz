// TLSF (Two-Level Segregated Fit) ベースのプール allocator 実装。
//
// 主目的: FileAllocator と BitmapAllocator の "実メモリ利用域" を起動時に
//         確保した独立ブロック内に閉じ込めて、TJS2 ヒープ等とのメモリ断片化を
//         分離する。各プールは 1 個の大きい malloc バッファで構成され、
//         OS から見ると単一の常駐領域として扱える。
//
// アルゴリズム要点:
//   - FL = floor(log2(size)) - FL_MIN を 27 ビン (2^5..2^31)
//   - SL = (size >> (fl - SL_BITS)) & (SL_COUNT-1) で 32 サブビン
//   - 自由ブロックは fl_bitmap_ + sl_bitmap_[fl] のビットマップで管理
//     (ffs / clz で O(1) 検索)
//   - 物理隣接ブロックは prev_phys 連結 + size_and_flags の PREV_FREE flag
//     で coalesce 判定 (O(1))
//
// ヘッダ構造 (16 バイト):
//   Block { Block *prev_phys; size_t size_and_flags; }
//   - bit0 = FREE, bit1 = PREV_FREE, それ以上 = サイズ (16 byte 単位丸め)
//   - FREE 時は payload 先頭に next_free / prev_free ポインタを置く
//     (= USED 時の payload 領域と同じ場所、union 的に使用)
//   - 末尾に sentinel ブロック (size=0, USED) を置き coalesce が pool 範囲を
//     越えないように歯止め
//
// fallback (system malloc): pool 枯渇時はそのまま malloc に逃がす。pool 範囲外
// ポインタを free する際にもそれを判定して system free に流す。両者は
// FallbackHeader を前置することで size/tag を保持。

#include "PooledAllocator.h"
#include "LogIntf.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

//---------------------------------------------------------------------------
// 内部 helpers
//---------------------------------------------------------------------------

// fls = "find last set" (1-based). 0 → 0、それ以外は最上位 bit の位置 (1..)。
int TVPPooledAllocator::fls_size(size_t v) {
	if (v == 0) return 0;
#if defined(_MSC_VER)
	unsigned long idx;
	#if SIZE_MAX > 0xFFFFFFFFu
	_BitScanReverse64(&idx, static_cast<unsigned long long>(v));
	#else
	_BitScanReverse(&idx, static_cast<unsigned long>(v));
	#endif
	return static_cast<int>(idx) + 1;
#else
	#if SIZE_MAX > 0xFFFFFFFFu
	return sizeof(size_t) * 8 - __builtin_clzll(v);
	#else
	return sizeof(size_t) * 8 - __builtin_clz(v);
	#endif
#endif
}

// TLSF index 計算。small-block (< 2^(FL_MIN+SL_BITS) = 1024 bytes) は
// FL=0 固定で SL を線形 (size / 32) に分割。それ以上は通常の log2 + 上位 5bit。
// 小ブロック特例を入れないと size 16 (FL=4 → 5 にクランプ) と size 48 (FL=5)
// が同じ bucket [0][16] に入ってしまい、48 byte 要求で 16 byte ブロックを掴む
// 致命的な誤判定が起きる (= TLSF の標準実装が必須とする処理)。

void TVPPooledAllocator::mappingInsertIndex(size_t size, int &fl, int &sl) const {
	constexpr size_t SMALL = size_t(1) << (FL_MIN + SL_BITS); // 1024
	if (size < SMALL) {
		fl = 0;
		// SL は size / 32 (32 単位の線形 bucket)。サイズ 0..31→0, 32..63→1, ...
		sl = static_cast<int>(size >> FL_MIN);
		if (sl >= SL_COUNT) sl = SL_COUNT - 1; // SMALL に近い size では起きない
	} else {
		int top = fls_size(size) - 1;
		if (top > FL_MAX) top = FL_MAX;
		fl = top - FL_MIN;
		sl = static_cast<int>((size >> (top - SL_BITS)) & (SL_COUNT - 1));
	}
}

void TVPPooledAllocator::mappingSearchIndex(size_t size, int &fl, int &sl) const {
	// 検索時は size を bucket 境界で round-up し、「mapping 後の bucket 内の
	// 全ブロックが size 以上」を保証する。これで bucket 先頭を取るだけで
	// O(1) で十分大きい block が掴める。
	constexpr size_t SMALL = size_t(1) << (FL_MIN + SL_BITS);
	if (size < SMALL) {
		// 小 bucket の幅は 32 (= 1 << FL_MIN)
		size += (size_t(1) << FL_MIN) - 1;
	} else {
		int top = fls_size(size) - 1;
		size += (size_t(1) << (top - SL_BITS)) - 1;
	}
	mappingInsertIndex(size, fl, sl);
}

TVPPooledAllocator::Block *
TVPPooledAllocator::findSuitable(size_t size, int &fl, int &sl) {
	mappingSearchIndex(size, fl, sl);
	// 同 FL 内で sl 以上のサブビンを探す
	uint32_t sl_map = sl_bitmap_[fl] & (~uint32_t(0) << sl);
	if (sl_map == 0) {
		// 上位 FL を探す
		uint32_t fl_map = fl_bitmap_ & (~uint32_t(0) << (fl + 1));
		if (fl_map == 0) return nullptr;
#if defined(_MSC_VER)
		unsigned long idx;
		_BitScanForward(&idx, fl_map);
		fl = static_cast<int>(idx);
#else
		fl = __builtin_ctz(fl_map);
#endif
		sl_map = sl_bitmap_[fl];
	}
#if defined(_MSC_VER)
	unsigned long idx;
	_BitScanForward(&idx, sl_map);
	sl = static_cast<int>(idx);
#else
	sl = __builtin_ctz(sl_map);
#endif
	return blocks_[fl][sl];
}

void TVPPooledAllocator::insertFree(Block *b) {
	int fl, sl;
	size_t size = sf_size(b->size_and_flags);
	mappingInsertIndex(size, fl, sl);
	Block *head = blocks_[fl][sl];
	b->next_free = head;
	b->prev_free = nullptr;
	if (head) head->prev_free = b;
	blocks_[fl][sl] = b;
	fl_bitmap_      |= (uint32_t(1) << fl);
	sl_bitmap_[fl]  |= (uint32_t(1) << sl);
	b->size_and_flags |= FLAG_FREE;
	// 物理次のブロックの PREV_FREE フラグも更新 (sentinel も含む。
	// sentinel は USED 固定で size=0 だが PREV_FREE は意味を持つ)
	Block *next = nextPhys(b);
	next->size_and_flags |= FLAG_PREV_FREE;
}

void TVPPooledAllocator::removeFree(Block *b) {
	int fl, sl;
	size_t size = sf_size(b->size_and_flags);
	mappingInsertIndex(size, fl, sl);
	if (b->prev_free) b->prev_free->next_free = b->next_free;
	if (b->next_free) b->next_free->prev_free = b->prev_free;
	if (blocks_[fl][sl] == b) {
		blocks_[fl][sl] = b->next_free;
		if (!blocks_[fl][sl]) {
			sl_bitmap_[fl] &= ~(uint32_t(1) << sl);
			if (sl_bitmap_[fl] == 0) {
				fl_bitmap_ &= ~(uint32_t(1) << fl);
			}
		}
	}
	b->size_and_flags &= ~FLAG_FREE;
	// 物理次のブロックの PREV_FREE flag を解除
	Block *next = nextPhys(b);
	next->size_and_flags &= ~FLAG_PREV_FREE;
}

void TVPPooledAllocator::splitIfPossible(Block *b, size_t need_size) {
	size_t cur = sf_size(b->size_and_flags);
	if (cur < need_size + MIN_BLOCK) return;  // 残りが MIN_BLOCK 未満なら split しない

	size_t left = cur - need_size - BLOCK_HEADER_SIZE;
	// b の size を need_size に縮める (flags + tag は維持)
	sf_setSize(b->size_and_flags, need_size);

	Block *split = reinterpret_cast<Block *>(reinterpret_cast<char *>(b) + BLOCK_HEADER_SIZE + need_size);
	split->prev_phys      = b;
	split->size_and_flags = sf_pack(left, TVPAllocTag::Unknown, 0);  // FREE flag は insertFree が立てる
	// 元の物理次の prev_phys を split に張り替え
	Block *orig_next = reinterpret_cast<Block *>(reinterpret_cast<char *>(b) + BLOCK_HEADER_SIZE + cur);
	orig_next->prev_phys = split;

	insertFree(split);
}

void TVPPooledAllocator::mergeWithNext(Block *b) {
	Block *next = nextPhys(b);
	size_t b_size    = sf_size(b->size_and_flags);
	size_t next_size = sf_size(next->size_and_flags);
	removeFree(next);
	// b の size のみ更新 (flags + tag は維持)
	sf_setSize(b->size_and_flags, b_size + BLOCK_HEADER_SIZE + next_size);
	// 物理 next の next の prev_phys を b に張り替え
	Block *next_next = reinterpret_cast<Block *>(reinterpret_cast<char *>(b) + BLOCK_HEADER_SIZE + sf_size(b->size_and_flags));
	next_next->prev_phys = b;
}

//---------------------------------------------------------------------------
// 公開 API
//---------------------------------------------------------------------------

TVPPooledAllocator::TVPPooledAllocator(size_t pool_size,
                                       const char *name,
                                       TVPAllocTag default_tag)
	: name_(name ? name : "?"), default_tag_(default_tag), pool_size_(pool_size)
{
	if (pool_size_ < MIN_BLOCK + 2 * BLOCK_HEADER_SIZE) {
		// 小さすぎる pool は意味がないので素 malloc 経路 (= fallback のみ) で動かす
		pool_size_ = 0;
		TVPLOG_INFO("PooledAllocator [{}]: pool disabled (size too small or 0), using system malloc only", name_);
		return;
	}
	pool_buf_ = static_cast<char *>(std::malloc(pool_size_));
	if (!pool_buf_) {
		TVPLOG_WARNING("PooledAllocator [{}]: failed to malloc pool of {} bytes; falling back to system malloc only",
		               name_, pool_size_);
		pool_size_ = 0;
		return;
	}
	// 16 byte align チェック (malloc は max_align_t alignment を保証するので通常 OK)
	if ((reinterpret_cast<uintptr_t>(pool_buf_) & (ALIGN - 1)) != 0) {
		TVPLOG_WARNING("PooledAllocator [{}]: pool not 16-byte aligned ({})",
		               name_, reinterpret_cast<uintptr_t>(pool_buf_));
	}

	// 初期構造: [first free block (size = pool_size_ - 2*HEADER)] [sentinel (size=0, USED)]
	Block *first = reinterpret_cast<Block *>(pool_buf_);
	sentinel_end_ = reinterpret_cast<Block *>(pool_buf_ + pool_size_ - BLOCK_HEADER_SIZE);

	first->prev_phys      = nullptr;
	first->size_and_flags = sf_pack(pool_size_ - 2 * BLOCK_HEADER_SIZE,
	                                 TVPAllocTag::Unknown, 0);  // free flag は insertFree

	sentinel_end_->prev_phys      = first;
	sentinel_end_->size_and_flags = 0;  // size=0, USED, PREV_FREE は insertFree 内で立つ

	insertFree(first);
	pool_used_.store(2 * BLOCK_HEADER_SIZE, std::memory_order_relaxed);
	TVPLOG_INFO("PooledAllocator [{}]: pool {} bytes ({:.2f} MB) initialized at {:p}",
	            name_, pool_size_, pool_size_ / (1024.0 * 1024.0),
	            static_cast<void *>(pool_buf_));
}

TVPPooledAllocator::~TVPPooledAllocator() {
	std::free(pool_buf_);
	pool_buf_ = nullptr;
}

void *TVPPooledAllocator::allocate(size_t size, TVPAllocTag tag) {
	if (size == 0) return nullptr;

	// pool 経路を試す
	if (pool_size_ > 0) {
		// 要求サイズを 16 byte align に丸め、最低 (MIN_BLOCK - HEADER) 確保
		size_t need = (size + ALIGN - 1) & ~(ALIGN - 1);
		if (need < MIN_BLOCK - BLOCK_HEADER_SIZE) need = MIN_BLOCK - BLOCK_HEADER_SIZE;

		std::lock_guard<std::mutex> lk(mu_);
		int fl, sl;
		Block *b = findSuitable(need, fl, sl);
		if (b) {
			removeFree(b);
			splitIfPossible(b, need);
			// 確定サイズで stats を記録 (free 時も同じ rounded size を引くので
			// current_used / total_alloc / total_freed が balance する)
			size_t actual_payload = sf_size(b->size_and_flags);
			// tag を block header に格納 (free 時に retrieve して per-tag stats へ)
			sf_setTag(b->size_and_flags, tag);
			pool_used_.fetch_add(actual_payload, std::memory_order_relaxed);
			stats_.recordAlloc(actual_payload, tag);
			void *payload = payloadFromBlock(b);
#if TVP_POOL_VERBOSE_LOG
			TVPLOG_DEBUG("PoolAlloc:[{}] pool ptr={:p} size={} (req {}) tag={} pool_used={}",
			             name_, payload, actual_payload, size,
			             TVPAllocTagName(tag), pool_used_.load(std::memory_order_relaxed));
#endif
			firePressureIfNeeded(pool_used_.load(std::memory_order_relaxed));
			return payload;
		}
		// pool 枯渇 → fallback
	}

	// fallback: system malloc + ヘッダ前置
	void *raw = std::malloc(size + FALLBACK_HEADER_SIZE);
	if (!raw) return nullptr;
	auto *h = static_cast<FallbackHeader *>(raw);
	h->size = size;
	h->tag  = tag;
	stats_.recordAlloc(size, tag);
	fallback_alloc_count_.fetch_add(1, std::memory_order_relaxed);
	fallback_bytes_.fetch_add(size, std::memory_order_relaxed);
	void *payload = static_cast<char *>(raw) + FALLBACK_HEADER_SIZE;
#if TVP_POOL_VERBOSE_LOG
	TVPLOG_DEBUG("PoolAlloc:[{}] fallback ptr={:p} size={} tag={}",
	             name_, payload, size, TVPAllocTagName(tag));
#endif
	return payload;
}

// pool 内 free の共通実装。merge prev/next + insertFree + stats 更新。
// stat_size は alloc 時に記録した rounded payload (ヘッダから読んだ初期値)。
// stat_tag は alloc 時に block header に格納した tag。
// merge で size が変わっても stats には初期 stat_size をそのまま使う
// (alloc 時の recordAlloc と balance させるため)。
void TVPPooledAllocator::freePoolBlock(Block *b) {
	const size_t stat_size = sf_size(b->size_and_flags);
	const TVPAllocTag stat_tag = sf_tag(b->size_and_flags);

	// merge with prev (PREV_FREE flag が立っていれば prev は free)
	if (b->size_and_flags & FLAG_PREV_FREE) {
		Block *prev = b->prev_phys;
		size_t b_size = sf_size(b->size_and_flags);
		removeFree(prev);
		size_t prev_size = sf_size(prev->size_and_flags);
		sf_setSize(prev->size_and_flags, prev_size + BLOCK_HEADER_SIZE + b_size);
		Block *next_after = reinterpret_cast<Block *>(reinterpret_cast<char *>(prev) + BLOCK_HEADER_SIZE + sf_size(prev->size_and_flags));
		next_after->prev_phys = prev;
		b = prev;
	}
	// merge with next (next が sentinel_end_ なら次は無し)
	Block *next = nextPhys(b);
	if (next != sentinel_end_ && (next->size_and_flags & FLAG_FREE)) {
		mergeWithNext(b);
	}
	insertFree(b);
	// stats: alloc 時の rounded size + tag を引く (balance するように)
	stats_.recordFree(stat_size, stat_tag);
	pool_used_.fetch_sub(stat_size, std::memory_order_relaxed);
}

void TVPPooledAllocator::free(void *mem) {
	if (!mem) return;

	if (pool_size_ > 0 && inPool(mem)) {
		std::lock_guard<std::mutex> lk(mu_);
		Block *b = blockFromPayload(mem);
		const size_t freed_size = sf_size(b->size_and_flags);
		const TVPAllocTag freed_tag = sf_tag(b->size_and_flags);
		freePoolBlock(b);
#if TVP_POOL_VERBOSE_LOG
		TVPLOG_DEBUG("PoolFree:[{}] pool ptr={:p} size={} tag={} pool_used={}",
		             name_, mem, freed_size, TVPAllocTagName(freed_tag),
		             pool_used_.load(std::memory_order_relaxed));
#else
		(void)freed_size; (void)freed_tag;
#endif
		return;
	}

	// fallback path
	auto *raw = static_cast<char *>(mem) - FALLBACK_HEADER_SIZE;
	auto *h   = reinterpret_cast<FallbackHeader *>(raw);
	const size_t fb_size = h->size;
	const TVPAllocTag fb_tag = h->tag;
	stats_.recordFree(fb_size, fb_tag);
	fallback_free_count_.fetch_add(1, std::memory_order_relaxed);
	fallback_bytes_.fetch_sub(fb_size, std::memory_order_relaxed);
	std::free(raw);
#if TVP_POOL_VERBOSE_LOG
	TVPLOG_DEBUG("PoolFree:[{}] fallback ptr={:p} size={} tag={}",
	             name_, mem, fb_size, TVPAllocTagName(fb_tag));
#else
	(void)fb_size; (void)fb_tag;
#endif
}

void TVPPooledAllocator::free(void *mem, size_t /*size*/) {
	// 渡された size は無視。pool 内 block は header に確定 size + tag が
	// 入っているのでそちらを優先する (alloc 時記録した値と一致)。
	// fallback も FallbackHeader に size + tag が入っているので同じ。
	free(mem);
}

// 既存ポインタの確保サイズを返す (reallocate のコピー量算出用)。
// pool 内 → Block::size_and_flags、fallback → FallbackHeader::size。
size_t TVPPooledAllocator::getAllocatedSize(void *mem) const {
	if (!mem) return 0;
	if (pool_size_ > 0 && inPool(mem)) {
		Block *b = blockFromPayload(mem);
		return sf_size(b->size_and_flags);
	}
	auto *raw = static_cast<char *>(mem) - FALLBACK_HEADER_SIZE;
	return reinterpret_cast<const FallbackHeader *>(raw)->size;
}

void TVPPooledAllocator::setPressureCallback(PressureCallback cb) {
	std::lock_guard<std::mutex> lk(pressure_cb_mu_);
	pressure_cb_ = std::move(cb);
}

void TVPPooledAllocator::firePressureIfNeeded(size_t cur_used) {
	if (pool_size_ == 0) return;
	float p = static_cast<float>(cur_used) / static_cast<float>(pool_size_);
	float prev = last_pressure_.load(std::memory_order_relaxed);
	// 0.05 単位で量子化して連続発火を抑制
	float qp   = std::floor(p * 20.0f) / 20.0f;
	float qprev = prev < 0 ? -1.0f : std::floor(prev * 20.0f) / 20.0f;
	if (qp == qprev) return;
	last_pressure_.store(qp, std::memory_order_relaxed);
	PressureCallback cb;
	{
		std::lock_guard<std::mutex> lk(pressure_cb_mu_);
		cb = pressure_cb_;
	}
	if (cb) cb(p);
}
