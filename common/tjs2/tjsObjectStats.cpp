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
std::atomic<uint64_t>                            g_custom_object_count{0};
std::atomic<uint64_t>                            g_custom_object_peak{0};

constexpr size_t kTopN = 10;

} // namespace

void TVPIncrementTJSCustomObjectCount() noexcept {
	uint64_t cur = g_custom_object_count.fetch_add(1, std::memory_order_relaxed) + 1;
	uint64_t pk  = g_custom_object_peak.load(std::memory_order_relaxed);
	while (cur > pk &&
	       !g_custom_object_peak.compare_exchange_weak(pk, cur, std::memory_order_relaxed)) {}
}

void TVPDecrementTJSCustomObjectCount() noexcept {
	g_custom_object_count.fetch_sub(1, std::memory_order_relaxed);
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

	{
		std::lock_guard<std::mutex> lk(g_mutex);
		dict_instances  = g_dicts.size();
		array_instances = g_arrays.size();
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
