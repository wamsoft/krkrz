#include "tjsCommHead.h"
#include "SystemAllocatorInfo.h"
#include "MemoryAllocatorStats.h" // TVPFormatBytes
#include "ProcessMemory.h"        // TVPGetProcessMemoryInfo
#include "LogIntf.h"
#include "Application.h"          // Application / tTVPApplication::GetSystemAllocatorInfo

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/sysinfo.h>
#endif

// ----------------------------------------------------------------
// プラットフォーム別: システム全体の物理メモリ情報取得
// ----------------------------------------------------------------

namespace {

struct SystemMemoryInfo {
	size_t total_physical = SIZE_MAX;
	size_t avail_physical = SIZE_MAX;
};

SystemMemoryInfo GetSystemMemory()
{
	SystemMemoryInfo info;

#if defined(_WIN32)
	MEMORYSTATUSEX ms{};
	ms.dwLength = sizeof(ms);
	if (::GlobalMemoryStatusEx(&ms)) {
		info.total_physical = static_cast<size_t>(ms.ullTotalPhys);
		info.avail_physical = static_cast<size_t>(ms.ullAvailPhys);
	}

#elif defined(__APPLE__)
	// macOS: 合計物理メモリ
	int mib[2] = { CTL_HW, HW_MEMSIZE };
	uint64_t physical_memory = 0;
	size_t length = sizeof(physical_memory);
	if (sysctl(mib, 2, &physical_memory, &length, nullptr, 0) == 0) {
		info.total_physical = static_cast<size_t>(physical_memory);
	}

	// macOS: 空き物理メモリ (vm_statistics64 から)
	vm_size_t page_size = 0;
	if (host_page_size(mach_host_self(), &page_size) == KERN_SUCCESS && page_size > 0) {
		vm_statistics64_data_t vm_stat{};
		mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
		if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
		                      reinterpret_cast<host_info64_t>(&vm_stat), &count) == KERN_SUCCESS) {
			// free_count + inactive_count + speculative_count を「利用可能」として計算
			uint64_t free_pages = vm_stat.free_count + vm_stat.inactive_count + vm_stat.speculative_count;
			info.avail_physical = static_cast<size_t>(free_pages * page_size);
		}
	}

#elif defined(__linux__) || defined(__ANDROID__)
	struct sysinfo si{};
	if (sysinfo(&si) == 0) {
		info.total_physical = static_cast<size_t>(si.totalram) * si.mem_unit;
		// Linux では freeram + bufferram + cached が実質的な空きだが、
		// sysinfo では cached が取れないので MemAvailable を /proc/meminfo から読む
		info.avail_physical = static_cast<size_t>(si.freeram) * si.mem_unit;
	}
	// /proc/meminfo の MemAvailable があればそちらを優先
	if (FILE* fp = std::fopen("/proc/meminfo", "r")) {
		char line[256];
		while (std::fgets(line, sizeof(line), fp)) {
			unsigned long kb = 0;
			if (std::sscanf(line, "MemAvailable: %lu kB", &kb) == 1) {
				info.avail_physical = static_cast<size_t>(kb) * 1024;
				break;
			}
		}
		std::fclose(fp);
	}
#endif

	return info;
}

} // anonymous namespace

// ----------------------------------------------------------------
// tTVPDefaultSystemAllocatorInfo 実装
// ----------------------------------------------------------------

size_t tTVPDefaultSystemAllocatorInfo::GetTotalFreeSize() const noexcept
{
	// 一般的なデスクトップ OS では malloc ヒープ内の空きを正確に知ることは難しい。
	// 代替として、システムの空き物理メモリを返す。
	// Switch 等のプラットフォーム固有実装ではより正確な値を返せる。
	SystemMemoryInfo sys = GetSystemMemory();
	return sys.avail_physical;
}

size_t tTVPDefaultSystemAllocatorInfo::GetAllocatableSize() const noexcept
{
	// 一般的なデスクトップ OS ではフラグメンテーションを考慮した
	// 確保可能最大サイズを知ることは難しい。
	// 保守的に、空き物理メモリの 80% を返す (仮想メモリがあっても
	// 実用上はスワップが発生するとパフォーマンスに影響)。
	SystemMemoryInfo sys = GetSystemMemory();
	if (sys.avail_physical == SIZE_MAX) {
		return SIZE_MAX;
	}
	return (sys.avail_physical * 80) / 100;
}

void tTVPDefaultSystemAllocatorInfo::Dump() const
{
	TVPSystemAllocatorStats stats = GetStats();
	
	TVPLOG_INFO("SystemAllocatorInfo:");
	
	// システムメモリ
	TVPLOG_INFO("  System: total_physical={} avail_physical={}",
	            TVPFormatBytes(stats.system_total_physical),
	            TVPFormatBytes(stats.system_avail_physical));
	
	// プロセスメモリ
	TVPLOG_INFO("  Process: rss={} peak_rss={} vsize={}",
	            TVPFormatBytes(stats.process_rss),
	            TVPFormatBytes(stats.process_peak_rss),
	            TVPFormatBytes(stats.process_vsize));
	
	// Switch 互換情報
	TVPLOG_INFO("  Allocator: total_free={} allocatable={}",
	            TVPFormatBytes(stats.total_free_size),
	            TVPFormatBytes(stats.allocatable_size));
}

TVPSystemAllocatorStats tTVPDefaultSystemAllocatorInfo::GetStats() const
{
	TVPSystemAllocatorStats stats;
	
	// システムメモリ
	SystemMemoryInfo sys = GetSystemMemory();
	stats.system_total_physical = sys.total_physical;
	stats.system_avail_physical = sys.avail_physical;
	
	// プロセスメモリ
	TVPProcessMemoryInfo pmi = TVPGetProcessMemoryInfo();
	stats.process_rss      = pmi.rss;
	stats.process_peak_rss = pmi.peak_rss;
	stats.process_vsize    = pmi.vsize;
	
	// Switch 互換値
	stats.total_free_size  = GetTotalFreeSize();
	stats.allocatable_size = GetAllocatableSize();
	
	// 一般 OS では total_size / used_size / peak_used_size は取得困難
	// (SIZE_MAX = 未対応のまま)
	
	return stats;
}

std::string tTVPDefaultSystemAllocatorInfo::GetSummary() const
{
	TVPSystemAllocatorStats stats = GetStats();
	
	std::string out("SysAlloc: free=");
	out += TVPFormatBytes(stats.total_free_size);
	out += " allocatable=";
	out += TVPFormatBytes(stats.allocatable_size);
	out += " | Process: rss=";
	out += TVPFormatBytes(stats.process_rss);
	out += " vsize=";
	out += TVPFormatBytes(stats.process_vsize);
	
	return out;
}

// ----------------------------------------------------------------
// グローバルアクセス関数
// ----------------------------------------------------------------

// デフォルト実装の共有シングルトン。
// Application 基底 (tTVPApplication::GetSystemAllocatorInfo) と、
// Application 未初期化時の TVPGetSystemAllocatorInfo フォールバックで
// 共用する (2 重に singleton を持たない)。
iTVPSystemAllocatorInfo* TVPGetDefaultSystemAllocatorInfo()
{
	static tTVPDefaultSystemAllocatorInfo instance;
	return &instance;
}

// システムアロケータ情報の公開アクセサ。
// Application が生きていれば必ず Application 経由で取る。これにより
// プラットフォーム派生 (NXApplication 等) の override が
// TJS / REPL / MemoryOverlay 等すべての利用箇所に反映される。
// Application 初期化前 / 終了後はデフォルト実装にフォールバック。
iTVPSystemAllocatorInfo* TVPGetSystemAllocatorInfo()
{
	if (Application) {
		if (auto *info = Application->GetSystemAllocatorInfo()) {
			return info;
		}
	}
	return TVPGetDefaultSystemAllocatorInfo();
}

void TVPDumpSystemAllocatorInfo()
{
	if (auto *info = TVPGetSystemAllocatorInfo()) {
		info->Dump();
	}
}

std::string TVPSummarizeSystemAllocatorInfo()
{
	if (auto *info = TVPGetSystemAllocatorInfo()) {
		return info->GetSummary();
	}
	return "SystemAllocatorInfo: (not available)";
}
