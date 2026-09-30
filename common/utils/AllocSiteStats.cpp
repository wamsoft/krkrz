// AllocSiteStats: 呼び出し元 (スタック) 別の生存確保集計。説明は AllocSiteStats.h。

#include "tjsCommHead.h"
#include "AllocSiteStats.h"
#include "LogIntf.h"

#if defined(KRKRZ_ENABLE_MEMSTAT_DETAIL) && defined(_WIN32)

#include "SysInitIntf.h"            // TVPGetCommandLine
#include "MemoryAllocatorStats.h"   // TVPFormatBytes

#include <windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int      kMaxFrames   = 32;
constexpr uint32_t kMaxSites    = 1u << 18;   // サイト本体 (連番)
constexpr uint32_t kSlotCount   = 1u << 20;   // ハッシュ表 (サイト番号の索引)
constexpr uint32_t kSlotMask    = kSlotCount - 1;

struct Site {
	void                 *frames[kMaxFrames];
	uint32_t              nframes;
	uint32_t              hash_hi;
	std::atomic<int64_t>  live_count;
	std::atomic<int64_t>  live_bytes;
	std::atomic<uint64_t> total_count;
};

// どちらも VirtualAlloc で取る (本 TU の集計対象の確保経路を通さない)。
Site                  *g_sites = nullptr;
std::atomic<uint64_t> *g_slots = nullptr;  // 0 = 空、それ以外 = (hash_hi << 32) | (index + 1)
std::atomic<uint32_t>  g_site_count{0};
std::atomic<uint64_t>  g_overflow{0};
int                    g_depth = 0;         // 0 = 無効

uint64_t hash_frames(void *const *frames, uint32_t n) {
	uint64_t h = 1469598103934665603ULL;
	for (uint32_t i = 0; i < n; ++i) {
		h ^= reinterpret_cast<uintptr_t>(frames[i]);
		h *= 1099511628211ULL;
		h ^= h >> 29;
	}
	return h;
}

} // namespace

namespace TVPAllocSiteStats {

void Initialize()
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-memstatsite"), &val)) return;
	ttstr str(val);
	int depth = 0;
	if (str == TJS_W("yes") || str == TJS_W("on") || str == TJS_W("true")) depth = 16;
	else depth = (int)(tjs_int64)val;
	if (depth <= 0) return;
	if (depth > kMaxFrames) depth = kMaxFrames;

	g_sites = static_cast<Site *>(VirtualAlloc(nullptr, sizeof(Site) * kMaxSites,
	                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	g_slots = static_cast<std::atomic<uint64_t> *>(VirtualAlloc(nullptr, sizeof(uint64_t) * kSlotCount,
	                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	if (!g_sites || !g_slots) {
		TVPLOG_WARNING("AllocSiteStats: table allocation failed; disabled");
		return;
	}
	g_depth = depth;
	TVPLOG_INFO("AllocSiteStats: enabled (depth={}, max sites={})", depth, kMaxSites);
}

bool Enabled() { return g_depth > 0; }

__declspec(noinline) uint32_t Capture()
{
	if (g_depth <= 0) return 0;
	void *frames[kMaxFrames];
	// 1 段目 (本関数) を飛ばす。do_malloc / operator new 等は Dump 側で除く。
	uint32_t n = RtlCaptureStackBackTrace(1, (DWORD)g_depth, frames, nullptr);
	if (n == 0) return 0;
	uint64_t h = hash_frames(frames, n);
	uint32_t hi = static_cast<uint32_t>(h >> 32) | 1u;
	uint32_t slot = static_cast<uint32_t>(h) & kSlotMask;
	uint32_t reserved = 0;  // 自分で確保したサイト番号 + 1 (未使用なら 0)
	for (uint32_t probe = 0; probe < kSlotCount; ++probe, slot = (slot + 1) & kSlotMask) {
		uint64_t v = g_slots[slot].load(std::memory_order_acquire);
		if (v == 0) {
			if (!reserved) {
				uint32_t idx = g_site_count.fetch_add(1, std::memory_order_relaxed);
				if (idx >= kMaxSites) {
					g_overflow.fetch_add(1, std::memory_order_relaxed);
					return 0;
				}
				Site &s = g_sites[idx];
				std::memcpy(s.frames, frames, sizeof(void *) * n);
				s.nframes = n;
				s.hash_hi = hi;
				reserved = idx + 1;
			}
			uint64_t mine = (static_cast<uint64_t>(hi) << 32) | reserved;
			if (g_slots[slot].compare_exchange_strong(v, mine, std::memory_order_acq_rel)) {
				return reserved;
			}
			// 他スレッドが先に埋めた。v に入った値で下の比較をやり直す。
		}
		if (static_cast<uint32_t>(v >> 32) != hi) continue;
		uint32_t id = static_cast<uint32_t>(v);
		const Site &s = g_sites[id - 1];
		if (s.nframes == n && std::memcmp(s.frames, frames, sizeof(void *) * n) == 0) {
			// 自分で確保したサイト番号が無駄になっても害はない (live 0 のまま)
			return id;
		}
	}
	g_overflow.fetch_add(1, std::memory_order_relaxed);
	return 0;
}

void OnAlloc(uint32_t site, size_t size)
{
	if (site == 0 || site > kMaxSites) return;
	Site &s = g_sites[site - 1];
	s.live_count.fetch_add(1, std::memory_order_relaxed);
	s.live_bytes.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed);
	s.total_count.fetch_add(1, std::memory_order_relaxed);
}

void OnFree(uint32_t site, size_t size)
{
	if (site == 0 || site > kMaxSites || !g_sites) return;
	Site &s = g_sites[site - 1];
	s.live_count.fetch_sub(1, std::memory_order_relaxed);
	s.live_bytes.fetch_sub(static_cast<int64_t>(size), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------
namespace {

struct Frame {
	std::string name;
	std::string where;  // file:line (取れたときだけ)
};

// 1 アドレスを論理フレーム列 (インライン展開の内側 → 外側) にする。
class Symbolizer {
public:
	Symbolizer() {
		proc_ = GetCurrentProcess();
		static bool inited = false;
		if (!inited) {
			SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			inited = SymInitialize(proc_, nullptr, TRUE) != FALSE;
			if (!inited) {
				// 既に誰かが初期化済みなら続行できる
				inited = (GetLastError() == ERROR_INVALID_PARAMETER);
			}
		}
	}

	const std::vector<Frame> &resolve(void *addr) {
		auto it = cache_.find(addr);
		if (it != cache_.end()) return it->second;
		std::vector<Frame> &out = cache_[addr];
		// 戻り番地は call の次を指すので 1 引いて引く
		DWORD64 a = reinterpret_cast<DWORD64>(addr) - 1;
		alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 1024];
		SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
		DWORD n = SymAddrIncludeInlineTrace(proc_, a);
		DWORD ctx = 0, idx = 0;
		if (n > 0 && SymQueryInlineTrace(proc_, a, 0, a, a, &ctx, &idx)) {
			for (DWORD i = 0; i < n; ++i) {
				std::memset(buf, 0, sizeof(buf));
				sym->SizeOfStruct = sizeof(SYMBOL_INFO);
				sym->MaxNameLen = 1024;
				DWORD64 disp = 0;
				Frame f;
				if (SymFromInlineContext(proc_, a, ctx + i, &disp, sym)) f.name = sym->Name;
				else f.name = "?";
				IMAGEHLP_LINE64 line = {};
				line.SizeOfStruct = sizeof(line);
				DWORD ldisp = 0;
				if (SymGetLineFromInlineContext(proc_, a, ctx + i, 0, &ldisp, &line)) {
					f.where = shortFile(line.FileName) + ":" + std::to_string(line.LineNumber);
				}
				out.push_back(std::move(f));
			}
		}
		std::memset(buf, 0, sizeof(buf));
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 1024;
		DWORD64 disp = 0;
		Frame f;
		if (SymFromAddr(proc_, a, &disp, sym)) {
			f.name = sym->Name;
			// PDB の無い DLL (プラグイン等) は近くのエクスポート名になるのでモジュール名を添える
			IMAGEHLP_MODULE64 mi = {};
			mi.SizeOfStruct = sizeof(mi);
			if (SymGetModuleInfo64(proc_, a, &mi) && mi.SymType != SymPdb) {
				f.name = std::string(mi.ModuleName) + "!" + f.name + "?";
			}
		} else {
			// シンボルが無い (プラグイン DLL 等) ときはモジュール名 + オフセット
			HMODULE mod = nullptr;
			char path[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                       reinterpret_cast<LPCSTR>(addr), &mod)) {
				GetModuleFileNameA(mod, path, MAX_PATH);
			}
			char tmp[64];
			std::snprintf(tmp, sizeof(tmp), "+0x%llx",
			              (unsigned long long)(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
			f.name = shortFile(path) + tmp;
		}
		IMAGEHLP_LINE64 line = {};
		line.SizeOfStruct = sizeof(line);
		DWORD ldisp = 0;
		if (SymGetLineFromAddr64(proc_, a, &ldisp, &line)) {
			f.where = shortFile(line.FileName) + ":" + std::to_string(line.LineNumber);
		}
		out.push_back(std::move(f));
		return out;
	}

private:
	static std::string shortFile(const char *p) {
		const char *s = p;
		for (const char *q = p; *q; ++q) if (*q == '\\' || *q == '/') s = q + 1;
		return s;
	}
	HANDLE proc_;
	std::unordered_map<void *, std::vector<Frame>> cache_;
};

// アロケータ内部とみなして飛ばすフレーム
bool is_plumbing(const std::string &n) {
	static const char *kPrefixes[] = {
		"operator new", "operator delete", "TVPKrkrz", "TVPAllocSiteStats::",
		"`anonymous namespace'::do_", "std::", "malloc", "calloc", "realloc",
		"_malloc", "_calloc", "_realloc", "TVPGlobalAllocStats::",
	};
	for (const char *p : kPrefixes) {
		if (n.compare(0, std::strlen(p), p) == 0) return true;
	}
	return false;
}

struct Agg {
	int64_t  bytes = 0;
	int64_t  count = 0;
	std::string where;
};

void dump_table(const char *title, std::unordered_map<std::string, Agg> &m, int top, int64_t total)
{
	std::vector<std::pair<const std::string *, Agg *>> v;
	v.reserve(m.size());
	for (auto &kv : m) v.push_back({&kv.first, &kv.second});
	std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second->bytes > b.second->bytes; });
	TVPLOG_INFO("AllocSite {} (top {} / {} groups)", title, std::min<size_t>(top, v.size()), v.size());
	int shown = 0;
	for (auto &e : v) {
		if (shown++ >= top) break;
		const Agg &a = *e.second;
		double pct = total > 0 ? 100.0 * (double)a.bytes / (double)total : 0.0;
		char head[128];
		std::snprintf(head, sizeof(head), "%10s %5.1f%% n=%-9lld avg=%-6lld",
		              TVPFormatBytes((uint64_t)a.bytes).c_str(),
		              pct, (long long)a.count, (long long)(a.count ? a.bytes / a.count : 0));
		if (a.where.empty()) {
			TVPLOG_INFO("  {} {}", head, *e.first);
		} else {
			TVPLOG_INFO("  {} {}  [{}]", head, *e.first, a.where);
		}
	}
}

} // namespace

void Dump(int top, const char *filter)
{
	if (g_depth <= 0) {
		TVPLOG_INFO("AllocSite: disabled (start with -memstatsite=yes on a MEMSTAT_DETAIL build)");
		return;
	}
	if (top <= 0) top = 40;
	// 集計中の確保は site を持つが、表の確定値だけを読むので影響は無視できる。
	uint32_t nsites = std::min(g_site_count.load(std::memory_order_acquire), kMaxSites);
	std::vector<uint32_t> live;
	int64_t total_bytes = 0, total_count = 0;
	for (uint32_t i = 0; i < nsites; ++i) {
		int64_t b = g_sites[i].live_bytes.load(std::memory_order_relaxed);
		if (b <= 0 || g_sites[i].nframes == 0) continue;
		live.push_back(i);
		total_bytes += b;
		total_count += g_sites[i].live_count.load(std::memory_order_relaxed);
	}
	TVPLOG_INFO("AllocSite: sites={} live_sites={} overflow={} depth={} attributed live={} n={}",
	            nsites, live.size(), g_overflow.load(), g_depth,
	            TVPFormatBytes((uint64_t)total_bytes), total_count);

	Symbolizer sym;
	std::unordered_map<std::string, Agg> by_func, by_chain2, by_chain4, by_chain8;
	const bool filtered = filter && *filter;
	for (uint32_t i : live) {
		const Site &s = g_sites[i];
		int64_t b = s.live_bytes.load(std::memory_order_relaxed);
		int64_t c = s.live_count.load(std::memory_order_relaxed);
		std::vector<const Frame *> user;
		for (uint32_t k = 0; k < s.nframes; ++k) {
			for (const Frame &f : sym.resolve(s.frames[k])) {
				if (user.empty() && is_plumbing(f.name)) continue;
				user.push_back(&f);
			}
		}
		if (user.empty()) continue;
		if (filtered) {
			bool hit = false;
			for (const Frame *f : user) {
				if (f->name.find(filter) != std::string::npos) { hit = true; break; }
			}
			if (!hit) continue;
		}
		auto add = [&](std::unordered_map<std::string, Agg> &m, size_t depth) {
			std::string key;
			for (size_t k = 0; k < depth && k < user.size(); ++k) {
				if (k) key += " < ";
				key += user[k]->name;
			}
			Agg &a = m[key];
			a.bytes += b;
			a.count += c;
			if (a.where.empty()) a.where = user[0]->where;
		};
		if (filtered) {
			add(by_chain8, 8);
			continue;
		}
		add(by_func, 1);
		add(by_chain2, 2);
		add(by_chain4, 4);
	}
	if (filtered) {
		std::string title = std::string("by caller chain (8) matching '") + filter + "'";
		dump_table(title.c_str(), by_chain8, top, total_bytes);
		return;
	}
	dump_table("by function", by_func, top, total_bytes);
	dump_table("by caller chain (2)", by_chain2, top, total_bytes);
	dump_table("by caller chain (4)", by_chain4, top, total_bytes);
}

} // namespace TVPAllocSiteStats

#else

namespace TVPAllocSiteStats {
void Dump(int, const char *)
{
	TVPLOG_INFO("AllocSite: not available in this build (needs KRKRZ_ENABLE_MEMSTAT_DETAIL on Windows)");
}
} // namespace TVPAllocSiteStats

#endif
