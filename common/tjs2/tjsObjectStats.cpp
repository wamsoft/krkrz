#include "tjsCommHead.h"
#include "tjsObjectStats.h"

// 本 TU 全体は KRKRZ_ENABLE_MEMSTAT_DETAIL 未定義時は無効化される。
// (header 側で no-op inline stub を提供するため、本 TU が空でも問題なし)
#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL

#include "tjsDictionary.h"
#include "tjsArray.h"
#include "tjsObject.h"             // tTJSCustomObject (Symbols / HashSize / TJS_SYMBOL_USING)
#include "tjsVariantString.h"
#include "CharacterSet.h"          // TVPUtf16ToUtf8
#include "LogIntf.h"
#include "MemoryAllocatorStats.h"   // TVPFormatBytes

#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <string>
#include <cstdio>

#include <atomic>

namespace {

std::mutex                                       g_mutex;
std::unordered_set<TJS::tTJSDictionaryObject *>  g_dicts;
std::unordered_set<TJS::tTJSArrayObject *>       g_arrays;
// 生存中の tTJSCustomObject。 dump 時に ClassNames[0] を引いてクラス名別に
// 数えるために覚えておく (「どのクラスが増え続けているか」を名指しするため)。
std::unordered_set<TJS::tTJSCustomObject *>      g_customs;
std::atomic<uint64_t>                            g_custom_object_count{0};
std::atomic<uint64_t>                            g_custom_object_peak{0};

constexpr size_t kTopN = 10;

} // namespace

void TVPIncrementTJSCustomObjectCount(TJS::tTJSCustomObject *obj) noexcept {
	uint64_t cur = g_custom_object_count.fetch_add(1, std::memory_order_relaxed) + 1;
	uint64_t pk  = g_custom_object_peak.load(std::memory_order_relaxed);
	while (cur > pk &&
	       !g_custom_object_peak.compare_exchange_weak(pk, cur, std::memory_order_relaxed)) {}
	if (!obj) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_customs.insert(obj);
}

void TVPDecrementTJSCustomObjectCount(TJS::tTJSCustomObject *obj) noexcept {
	g_custom_object_count.fetch_sub(1, std::memory_order_relaxed);
	if (!obj) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_customs.erase(obj);
}

void TVPRegisterTJSDictionary(TJS::tTJSDictionaryObject *obj) noexcept {
	if (!obj) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_dicts.insert(obj);
}

void TVPUnregisterTJSDictionary(TJS::tTJSDictionaryObject *obj) noexcept {
	if (!obj) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_dicts.erase(obj);
}

void TVPRegisterTJSArray(TJS::tTJSArrayObject *obj) noexcept {
	if (!obj) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_arrays.insert(obj);
}

void TVPUnregisterTJSArray(TJS::tTJSArrayObject *obj) noexcept {
	if (!obj) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	g_arrays.erase(obj);
}

namespace {

// Dict の先頭 K 個の key を "|" 連結した fingerprint を作る。
// 同じ key 構造を持つ Dict (= 同じパターン) を集計するためのキー。
std::string make_dict_fingerprint(TJS::tTJSDictionaryObject *d, int max_keys) {
	std::string fp;
	int got = 0;
	for (tjs_int h = 0; h < d->HashSize && got < max_keys; ++h) {
		for (TJS::tTJSCustomObject::tTJSSymbolData *sym = &d->Symbols[h];
		     sym; sym = sym->Next)
		{
			if (!(sym->SymFlags & TJS_SYMBOL_USING)) continue;
			if (!sym->Name) continue;
			std::string utf8;
			TVPUtf16ToUtf8(utf8, (const tjs_char *)*sym->Name);
			if (utf8.size() > 24) utf8.resize(24);
			if (!fp.empty()) fp += "|";
			fp += utf8;
			++got;
			if (got >= max_keys) break;
		}
	}
	if (got == 0) fp = "(empty)";
	return fp;
}

// entries 数 → bin index。bin ラベルは下記の kBinNames と同期。
int dict_bin_index(tjs_int n) {
	if (n <= 0)  return 0;
	if (n <= 3)  return 1;
	if (n <= 10) return 2;
	if (n <= 50) return 3;
	if (n <= 200) return 4;
	return 5;
}
constexpr int kNumBins = 6;
const char *const kBinNames[kNumBins] = {
	"=0", "1-3", "4-10", "11-50", "51-200", ">200"
};

} // namespace

namespace {

// 文字列ヒープ (tTJSVariantString のセル) の生存数・ブロック充填率・同じ内容の重複。
struct StrGroup {
	uint64_t count = 0;
	uint64_t long_bytes = 0;
};
struct StrHeapCtx {
	std::vector<uint32_t> per_block;
	uint64_t live = 0, refs = 0, long_count = 0, long_bytes = 0;
	std::unordered_map<std::u16string, StrGroup> groups;
};

void visit_string(void *p, tjs_uint block, const TJS::tTJSVariantString *vs) {
	auto &c = *static_cast<StrHeapCtx *>(p);
	if (block >= c.per_block.size()) c.per_block.resize(block + 1);
	++c.per_block[block];
	++c.live;
	c.refs += (uint64_t)vs->RefCount + 1;
	uint64_t lb = 0;
	if (vs->LongString) {
		++c.long_count;
		// TJSVS_malloc は確保長 (文字数) をバッファ直前に埋めている
		lb = ((const size_t *)vs->LongString)[-1] * sizeof(tjs_char) + sizeof(size_t);
		c.long_bytes += lb;
	}
	const tjs_char *str = vs->LongString ? vs->LongString : vs->ShortString;
	auto &g = c.groups[std::u16string((const char16_t *)str, (size_t)vs->Length)];
	++g.count;
	g.long_bytes += lb;
}

void dump_string_heap() {
	StrHeapCtx c;
	tjs_uint nblocks = 0, per = 0;
	TJS::TJSVisitStringHeap(visit_string, &c, &nblocks, &per);
	const uint64_t cell = sizeof(TJS::tTJSVariantString);
	char fill[32];
	std::snprintf(fill, sizeof(fill), "%.1f%%",
	              nblocks ? 100.0 * (double)c.live / ((double)nblocks * per) : 0.0);
	TVPLOG_INFO("TJSObjectStats: string heap: blocks={} ({}) live cells={} ({} full) refs={} long={} ({}) distinct={}",
	            (unsigned long long)nblocks, TVPFormatBytes((uint64_t)nblocks * per * cell),
	            (unsigned long long)c.live, fill,
	            (unsigned long long)c.refs, (unsigned long long)c.long_count,
	            TVPFormatBytes(c.long_bytes), (unsigned long long)c.groups.size());
	// ブロック充填率の分布 (空に近いブロックが多ければ断片化)
	uint64_t occ[11] = {};
	c.per_block.resize(nblocks);
	for (uint32_t n : c.per_block) ++occ[per ? (size_t)n * 10 / per : 0];
	TVPLOG_INFO("TJSObjectStats: string heap block fill (0-9%..100%): {} {} {} {} {} {} {} {} {} {} {}",
	            occ[0], occ[1], occ[2], occ[3], occ[4], occ[5], occ[6], occ[7], occ[8], occ[9], occ[10]);
	// 重複の多い内容 (同じ文字列が別セルで何個あるか)
	uint64_t dup_cells = 0, dup_bytes = 0;
	std::vector<std::pair<uint64_t, const std::pair<const std::u16string, StrGroup> *>> dups;
	for (auto const &kv : c.groups) {
		if (kv.second.count < 2) continue;
		uint64_t extra = kv.second.count - 1;
		uint64_t per_long = kv.second.long_bytes / kv.second.count;
		uint64_t waste = extra * (cell + per_long);
		dup_cells += extra;
		dup_bytes += waste;
		dups.emplace_back(waste, &kv);
	}
	TVPLOG_INFO("TJSObjectStats: string heap duplicates: extra cells={} (~{} incl. long buffers)",
	            (unsigned long long)dup_cells, TVPFormatBytes(dup_bytes));
	size_t dn = std::min<size_t>(20, dups.size());
	std::partial_sort(dups.begin(), dups.begin() + dn, dups.end(),
	                  [](auto const &a, auto const &b) { return a.first > b.first; });
	for (size_t i = 0; i < dn; ++i) {
		std::string utf8;
		TVPUtf16ToUtf8(utf8, (const tjs_char *)dups[i].second->first.c_str());
		if (utf8.size() > 48) utf8.resize(48);
		TVPLOG_INFO("TJSObjectStats:   dup[{}] copies={} waste={} [{}]",
		            (unsigned long long)i, (unsigned long long)dups[i].second->second.count,
		            TVPFormatBytes(dups[i].first), utf8);
	}
}

} // namespace

void TVPDumpTJSObjectStats() noexcept {
	// snapshot + size 集計 (lock 内で entry 数 / bin / fingerprint まで集める)。
	std::vector<std::pair<tjs_int, TJS::tTJSDictionaryObject *>> dict_sizes;
	size_t dict_total_entries = 0;
	size_t array_instances    = 0;
	size_t dict_instances     = 0;

	// bin 別の count + fingerprint 頻度。
	struct BinStats {
		size_t count = 0;
		std::unordered_map<std::string, size_t> fingerprints;
	};
	BinStats bins[kNumBins];

	// クラス名別の生存数。 ClassNames[0] が最派生クラス名。
	// (Dictionary / Array / 素の Object はクラス未適用なので "(no class)")
	// クラス別の生存数とメンバ表 (Symbols 配列 + 連鎖ノード) の大きさ。
	struct ClassStats {
		size_t   live    = 0;
		uint64_t members = 0;   // Count の合計
		uint64_t slots   = 0;   // HashSize の合計 (Symbols 配列の要素数)
		uint64_t chains  = 0;   // 連鎖ノード (new tTJSSymbolData) の数
	};
	std::unordered_map<std::string, ClassStats> per_class;
	ClassStats all_tables;

	{
		std::lock_guard<std::mutex> lk(g_mutex);
		dict_instances  = g_dicts.size();
		array_instances = g_arrays.size();
		for (auto *o : g_customs) {
			const std::vector<ttstr> &names = o->GetClassNames();
			std::string key;
			if (names.empty() || names[0].IsEmpty()) key = "(no class)";
			else TVPUtf16ToUtf8(key, names[0].c_str());
			ClassStats &cs = per_class[key];
			++cs.live;
			uint64_t chains = 0;
			for (tjs_int h = 0; h < o->HashSize; ++h)
				for (auto *sym = o->Symbols[h].Next; sym; sym = sym->Next) ++chains;
			uint64_t members = (o->Count > 0 ? (uint64_t)o->Count : 0);
			cs.members += members;
			cs.slots   += (uint64_t)o->HashSize;
			cs.chains  += chains;
			all_tables.members += members;
			all_tables.slots   += (uint64_t)o->HashSize;
			all_tables.chains  += chains;
		}
		dict_sizes.reserve(dict_instances);
		for (auto *d : g_dicts) {
			tjs_int c = d->Count;
			dict_sizes.emplace_back(c, d);
			dict_total_entries += (c > 0 ? (size_t)c : 0);

			int b = dict_bin_index(c);
			++bins[b].count;
			// fingerprint は先頭 3 key で集計 (パターン識別用)
			++bins[b].fingerprints[make_dict_fingerprint(d, 3)];
		}
	}

	// summary
	uint64_t total_objects = g_custom_object_count.load(std::memory_order_relaxed);
	uint64_t peak_objects  = g_custom_object_peak.load(std::memory_order_relaxed);
	TVPLOG_INFO("TJSObjectStats: CustomObject total instances={} peak={}",
	            (unsigned long long)total_objects,
	            (unsigned long long)peak_objects);
	TVPLOG_INFO("TJSObjectStats: Dictionary instances={} total_entries={}",
	            (unsigned long long)dict_instances,
	            (unsigned long long)dict_total_entries);
	TVPLOG_INFO("TJSObjectStats: Array      instances={}",
	            (unsigned long long)array_instances);

	// クラス名別の生存数 上位 (増え続けているクラスを名指しするため)。
	// 2 回 dump して差分を見ると、 どのクラスが解放されずに溜まっているかが判る。
	// あわせてメンバ表の大きさ (Symbols 配列 + 連鎖ノード、各 sizeof(tTJSSymbolData)) を出す。
	{
		const uint64_t symsz = sizeof(TJS::tTJSCustomObject::tTJSSymbolData);
		TVPLOG_INFO("TJSObjectStats: member tables: members={} slots={} ({}) chain nodes={} ({})",
		            (unsigned long long)all_tables.members,
		            (unsigned long long)all_tables.slots,
		            TVPFormatBytes(all_tables.slots * symsz),
		            (unsigned long long)all_tables.chains,
		            TVPFormatBytes(all_tables.chains * symsz));
		std::vector<std::pair<const ClassStats *, const std::string *>> cls;
		cls.reserve(per_class.size());
		for (auto const &kv : per_class) cls.emplace_back(&kv.second, &kv.first);
		size_t cn = std::min<size_t>(20, cls.size());
		std::partial_sort(cls.begin(), cls.begin() + cn, cls.end(),
		                  [](auto const &a, auto const &b) { return a.first->live > b.first->live; });
		TVPLOG_INFO("TJSObjectStats: per-class live (top {} of {} classes)",
		            (unsigned long long)cn, (unsigned long long)cls.size());
		for (size_t i = 0; i < cn; ++i) {
			const ClassStats &c = *cls[i].first;
			TVPLOG_INFO("TJSObjectStats:   class[{}] live={} name={} members/inst={} table={} (slots {} + chain {})",
			            (unsigned long long)i, (unsigned long long)c.live, *cls[i].second,
			            (unsigned long long)(c.live ? c.members / c.live : 0),
			            TVPFormatBytes((c.slots + c.chains) * symsz),
			            TVPFormatBytes(c.slots * symsz), TVPFormatBytes(c.chains * symsz));
		}
		// メンバ表の大きい順 (生存数が少なくても表が大きいクラスを拾う)
		std::partial_sort(cls.begin(), cls.begin() + cn, cls.end(),
		                  [](auto const &a, auto const &b) {
		                      return a.first->slots + a.first->chains > b.first->slots + b.first->chains; });
		TVPLOG_INFO("TJSObjectStats: per-class member table bytes (top {})", (unsigned long long)cn);
		for (size_t i = 0; i < cn; ++i) {
			const ClassStats &c = *cls[i].first;
			TVPLOG_INFO("TJSObjectStats:   table[{}] {} live={} name={} members/inst={}",
			            (unsigned long long)i, TVPFormatBytes((c.slots + c.chains) * symsz),
			            (unsigned long long)c.live, *cls[i].second,
			            (unsigned long long)(c.live ? c.members / c.live : 0));
		}
	}

	dump_string_heap();

	if (dict_instances == 0) return;

	// Top-N Dictionary by Count
	size_t n = std::min(kTopN, dict_sizes.size());
	std::partial_sort(dict_sizes.begin(), dict_sizes.begin() + n,
	                  dict_sizes.end(),
	                  [](auto const &a, auto const &b) {
	                      return a.first > b.first;
	                  });

	// 各 Dictionary について sample key を K 件まで抽出する。Symbols[] を線形走査、
	// TJS_SYMBOL_USING フラグが立っているスロットの Name を UTF-8 に変換して連結。
	constexpr int kSampleKeys = 5;
	constexpr size_t kMaxSampleLen = 200; // 1 行で長すぎないように
	for (size_t i = 0; i < n; ++i) {
		if (dict_sizes[i].first <= 0) break; // 0 件以下はスキップ
		auto *d = dict_sizes[i].second;
		char addr[32];
		std::snprintf(addr, sizeof(addr), "%p", (void *)d);

		std::string sample;
		int collected = 0;
		// d->Symbols[] / d->HashSize / d->HashMask は tTJSCustomObject の public member
		for (tjs_int h = 0; h < d->HashSize && collected < kSampleKeys; ++h) {
			for (TJS::tTJSCustomObject::tTJSSymbolData *sym = &d->Symbols[h];
			     sym; sym = sym->Next)
			{
				if (!(sym->SymFlags & TJS_SYMBOL_USING)) continue;
				if (!sym->Name) continue;
				std::string utf8;
				TVPUtf16ToUtf8(utf8, (const tjs_char *)*sym->Name);
				if (utf8.size() > 40) utf8.resize(40); // 1 key 40 byte 程度に切り詰め
				if (!sample.empty()) sample += ", ";
				sample += "\"";
				sample += utf8;
				sample += "\"";
				++collected;
				if (sample.size() > kMaxSampleLen) {
					sample += " ...";
					collected = kSampleKeys; // 抜ける
					break;
				}
				if (collected >= kSampleKeys) break;
			}
		}

		if (sample.empty()) {
			TVPLOG_INFO("TJSObjectStats:   Dict[{}] entries={} ptr={}",
			            (unsigned long long)i,
			            (long long)dict_sizes[i].first,
			            addr);
		} else {
			TVPLOG_INFO("TJSObjectStats:   Dict[{}] entries={} ptr={} sample_keys=[{}]",
			            (unsigned long long)i,
			            (long long)dict_sizes[i].first,
			            addr,
			            sample);
		}
	}

	// Dict entries 数別ヒストグラム + 各 bin の最頻 fingerprint pattern 上位 3 件。
	// 「小サイズ Dict が大量に増えているが top-N に出ない」ケースを可視化する。
	for (int b = 0; b < kNumBins; ++b) {
		if (bins[b].count == 0) continue;
		TVPLOG_INFO("TJSObjectStats:   Dict bin entries={}: count={}",
		            kBinNames[b], (unsigned long long)bins[b].count);
		// fingerprint を頻度順に並べて上位 3 件
		std::vector<std::pair<size_t, std::string>> fps;
		fps.reserve(bins[b].fingerprints.size());
		for (auto const &kv : bins[b].fingerprints) {
			fps.emplace_back(kv.second, kv.first);
		}
		size_t fn = std::min(size_t(3), fps.size());
		std::partial_sort(fps.begin(), fps.begin() + fn, fps.end(),
		                  [](auto const &a, auto const &b) {
		                      return a.first > b.first;
		                  });
		for (size_t i = 0; i < fn; ++i) {
			TVPLOG_INFO("TJSObjectStats:     fp[{}] count={} keys=[{}]",
			            (unsigned long long)i,
			            (unsigned long long)fps[i].first,
			            fps[i].second);
		}
	}
}

#endif // KRKRZ_ENABLE_MEMSTAT_DETAIL
