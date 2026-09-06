#pragma once

// 起動時に大きいメモリブロックを 1 個確保し、そこから TLSF で切り出す
// iTVPMemoryAllocator 実装。FileAllocator / BitmapAllocator の両方で使う想定。
//
// 目的: それぞれの "実メモリ利用域" を別プールに分離し、OS から見て独立した
//       常駐ブロックにする。capacity() = pool size を申告するので
//       MemoryBudgetNegotiation §4 の StorageCache との整合も自然に効く。
//
// 内部構造: TLSF (Two-Level Segregated Fit)。
//   - FL (first level): 27 bins (2^5 .. 2^31)
//   - SL (second level): 32 bins per FL (5-bit 二段分割)
//   - allocate / free / coalesce すべて O(1)
//
// 失敗時 fallback: pool が枯渇した場合は system malloc に流す。free 時は
// ポインタが pool 範囲内かを比較して経路を分岐 (O(1))。fallback 経由分は
// stats に "fallback=1" タグで集計してログから区別可能にする。

#include "tjsCommHead.h"

#include "Application.h"           // iTVPMemoryAllocator
#include "MemoryAllocatorStats.h"  // tTVPMemoryAllocatorStatsCollector

#include <atomic>
#include <cstddef>  // offsetof, size_t
#include <cstdint>
#include <mutex>

// PoolAlloc:/PoolFree: の毎呼出 DEBUG ログをコンパイル時に有効化するスイッチ。
// 通常 OFF (= 文字列フォーマット自体が消える)。leak 追跡時のみソース側で
// `#define TVP_POOL_VERBOSE_LOG 1` するか cmake で -DTVP_POOL_VERBOSE_LOG=1 を渡す。
#ifndef TVP_POOL_VERBOSE_LOG
#define TVP_POOL_VERBOSE_LOG 0
#endif

class TVPPooledAllocator : public iTVPMemoryAllocator {
public:
	// pool_size: バックエンドの 1 回 malloc サイズ (バイト)。
	//            あまりに小さい (< 1MB) と意味が薄いので caller 側で適度な
	//            下限を設定すること。0 なら一切 pool を作らず system malloc 直行
	//            (= BasicFileAllocator 互換)。
	// name:      ログ識別用名前 ("FilePool" / "BitmapPool" 等)。literal 推奨
	//            (本クラスはコピーしない)。
	// default_tag: tag を渡されない allocate(size) で使う既定タグ。
	explicit TVPPooledAllocator(size_t pool_size,
	                            const char *name,
	                            TVPAllocTag default_tag);
	~TVPPooledAllocator() override;

	// iTVPMemoryAllocator
	void  *allocate(size_t size) override { return allocate(size, default_tag_); }
	void  *allocate(size_t size, TVPAllocTag tag) override;
	void   free(void *mem) override;
	void   free(void *mem, size_t size) override;

	size_t capacity() const override { return pool_size_; }
	size_t used() const override     { return stats_.used(); }
	size_t getAllocatedSize(void *mem) const override;
	void   setPressureCallback(PressureCallback cb) override;
	Stats  getStats() const override { return stats_.snapshot(); }
	TagStats getTagStats(TVPAllocTag tag) const override {
		return stats_.tagSnapshot(tag);
	}
	void   resetPeak() override { stats_.resetPeak(); }

	// Pool 内の空き / 使用バイトと fallback 経由カウンタの取得 (デバッグ用)。
	size_t poolUsed() const   { return pool_used_.load(std::memory_order_relaxed); }
	uint64_t fallbackAllocCount() const override { return fallback_alloc_count_.load(std::memory_order_relaxed); }
	uint64_t fallbackFreeCount() const  { return fallback_free_count_.load(std::memory_order_relaxed); }
	uint64_t fallbackBytesInUse() const override { return fallback_bytes_.load(std::memory_order_relaxed); }

private:
	// TLSF パラメータ。詳細は .cpp 冒頭コメント参照。
	static constexpr int    SL_BITS     = 5;
	static constexpr int    SL_COUNT    = 1 << SL_BITS;        // 32
	static constexpr int    FL_MIN      = 5;                   // 2^5 = 32
	static constexpr int    FL_MAX      = 31;                  // 2^31 = 2GB
	static constexpr int    FL_COUNT    = FL_MAX - FL_MIN + 1; // 27
	static constexpr size_t ALIGN       = 16;                  // payload align
	static constexpr size_t MIN_BLOCK   = 32;                  // header + free list ptrs

	// 物理連結 (隣接ブロック検索) 用 + 状態フラグ
	// 全ブロックの先頭に置く。プール内は連続したヘッダ+payload で構成。
	//
	// size_and_flags の bit 配置 (常に uint64_t 固定):
	//   [0..1]  flags (FREE, PREV_FREE)
	//   [2..9]  TVPAllocTag (8 bit、_Count < 256 を想定)
	//   [10..63] payload サイズ (byte 単位、16-align 済み)
	// 32-bit ビルドで size_t が 32-bit になっても、SIZE_SHIFT=10 の残り 22-bit
	// では 4MB を超える block を表現できず pool が成立しないため、ここは
	// 必ず 64-bit 整数を使う。32-bit 環境では prev_phys (4 byte) の後に
	// uint64_t の natural alignment (8 byte) で 4 byte の padding が入り、
	// header (prev_phys + padding + size_and_flags) は 32/64-bit 共通で 16 byte。
	// tag は USED ブロックに対して有効。FREE ブロックは tag=Unknown 扱い。
	// alloc 時に setTag され、free 時に retrieve して stats_.recordFree
	// に渡すことで per-tag stats を正しく更新する (旧実装は free 時に tag 不明
	// で recordFree(0) と呼んでいて global current_used すら減算されないバグ
	// があった)。
	struct Block {
		Block             *prev_phys;        // 物理的に直前のブロック (= 連結リスト)。先頭は nullptr
		alignas(8) uint64_t size_and_flags;  // 上記レイアウト
		// 以下は FREE 時のみ有効。USED 時は payload の一部として再利用される。
		Block             *next_free;
		Block             *prev_free;
	};
	static constexpr size_t BLOCK_HEADER_SIZE = 16;
	static_assert(offsetof(Block, next_free) == BLOCK_HEADER_SIZE,
	              "Block header part (prev_phys + size_and_flags) must be 16 bytes");
	static_assert(sizeof(Block) <= MIN_BLOCK, "Block fields must fit in MIN_BLOCK");

	// size_and_flags の bit 操作 helpers (内部は uint64_t、外部 IF は size_t)
	static constexpr int      TAG_SHIFT   = 2;
	static constexpr int      SIZE_SHIFT  = 10;
	static constexpr uint64_t TAG_BITMASK = uint64_t(0xFF) << TAG_SHIFT;   // [2..9]
	static size_t      sf_size(uint64_t saf)   { return static_cast<size_t>(saf >> SIZE_SHIFT); }
	static TVPAllocTag sf_tag(uint64_t saf)    { return static_cast<TVPAllocTag>((saf >> TAG_SHIFT) & 0xFF); }
	static uint64_t    sf_flags(uint64_t saf)  { return saf & 3; }
	static uint64_t    sf_pack(size_t size, TVPAllocTag tag, uint64_t flags) {
		return (static_cast<uint64_t>(size) << SIZE_SHIFT)
		     | ((static_cast<uint64_t>(tag) & 0xFF) << TAG_SHIFT)
		     | (flags & 3);
	}
	// size を更新 (flags + tag は維持)
	static void sf_setSize(uint64_t &saf, size_t size) {
		saf = (saf & ((uint64_t(1) << SIZE_SHIFT) - 1))
		    | (static_cast<uint64_t>(size) << SIZE_SHIFT);
	}
	// tag を更新 (flags + size は維持)
	static void sf_setTag(uint64_t &saf, TVPAllocTag tag) {
		saf = (saf & ~TAG_BITMASK) | ((static_cast<uint64_t>(tag) & 0xFF) << TAG_SHIFT);
	}

	// fallback (system malloc) 経路用ヘッダ。pool 範囲外のポインタはこちら。
	// pool との混同を起こさないため pool アドレス範囲との比較で識別。
	struct FallbackHeader {
		size_t      size;
		TVPAllocTag tag;
	};
	static constexpr size_t FALLBACK_HEADER_SIZE = 16; // align 16

	// flags (size_and_flags は uint64_t なので enum も uint64_t で揃える。
	// size_t (= 32-bit on x86) で定義すると `~FLAG_FREE` が上位 32-bit を
	// すべて 0 にしてしまい size 領域を破壊する)
	enum : uint64_t {
		FLAG_FREE      = 1,
		FLAG_PREV_FREE = 2,
		FLAG_MASK      = 3,
	};

	// --- TLSF helpers (.cpp 側) ---
	static int    fls_size(size_t v);
	void          mappingInsertIndex(size_t size, int &fl, int &sl) const;
	void          mappingSearchIndex(size_t size, int &fl, int &sl) const;
	Block        *findSuitable(size_t size, int &fl, int &sl);

	// Block ヘッダの妥当性検査 (mu_ 保持下で呼ぶ)。範囲/整列/サイズ/リンクを
	// 確認し、破損を見つけたら false。expect_free = freelist 上のブロックとして
	// FREE flag と free リンクも検査するか。
	bool          validateBlock(const Block *b, bool expect_free) const;
	// 破損検知時の縮退処理 (mu_ 保持下で呼ぶ)。以後 pool からの新規確保を止め、
	// pool 内ポインタの free はリークさせる (プロセスは落とさない)。
	void          markCorrupted(const char *where, const void *b);

	void          insertFree(Block *b);
	void          removeFree(Block *b);
	void          splitIfPossible(Block *b, size_t need_size);
	void          mergeWithNext(Block *b);
	void          freePoolBlock(Block *b);

	Block        *blockFromPayload(void *p) const {
		return reinterpret_cast<Block *>(static_cast<char *>(p) - BLOCK_HEADER_SIZE);
	}
	void         *payloadFromBlock(Block *b) const {
		return reinterpret_cast<char *>(b) + BLOCK_HEADER_SIZE;
	}
	Block        *nextPhys(Block *b) const {
		// size_and_flags の bit [10..] が payload size。sf_size() で取り出す
		// (`& ~FLAG_MASK` は tag bits も拾ってしまうので NG)。
		return reinterpret_cast<Block *>(reinterpret_cast<char *>(b) + BLOCK_HEADER_SIZE + sf_size(b->size_and_flags));
	}
	bool          inPool(const void *p) const {
		auto cp = static_cast<const char *>(p);
		return cp >= pool_buf_ && cp < pool_buf_ + pool_size_;
	}

	void          firePressureIfNeeded(size_t cur_used);

	// --- メンバ ---
	const char         *name_;
	TVPAllocTag         default_tag_;
	char               *pool_buf_     = nullptr; // malloc'd backing
	size_t              pool_size_    = 0;
	Block              *sentinel_end_ = nullptr; // 末尾 dummy (USED, size=0)、coalesce 越境防止

	std::mutex          mu_;
	uint32_t            fl_bitmap_ = 0;
	uint32_t            sl_bitmap_[FL_COUNT] = {};
	Block              *blocks_[FL_COUNT][SL_COUNT] = {{}};
	// freelist/ヘッダ破損を検知したら true (mu_ 保持下で設定)。以後 pool 経路は
	// 使わず fallback へ縮退し、pool 内ポインタの free はリークさせる。
	bool                pool_corrupted_ = false;

	tTVPMemoryAllocatorStatsCollector stats_{
		tTVPMemoryAllocatorStatsCollector::Mode::Sized};

	// PressureCallback (任意スレッドから呼ぶ)。setPressureCallback で更新。
	PressureCallback         pressure_cb_;
	std::mutex               pressure_cb_mu_;
	std::atomic<float>       last_pressure_{-1.0f};

	// pool 内の使用バイト (sentinel/header 込み)。capacity 比較で pressure 判定する用。
	std::atomic<size_t>      pool_used_{0};
	// fallback 計数
	std::atomic<uint64_t>    fallback_alloc_count_{0};
	std::atomic<uint64_t>    fallback_free_count_{0};
	std::atomic<size_t>      fallback_bytes_{0};
};
