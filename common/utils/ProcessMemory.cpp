#include "tjsCommHead.h"
#include "ProcessMemory.h"
#include "MemoryAllocatorStats.h" // TVPFormatBytes
#include "LogIntf.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/mach_init.h>
#include <sys/resource.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <cstdio>
#include <cstring>
#include <cstdlib>
#endif

// プラットフォーム層 (nx/src, ps5/src) が登録する取得関数。
static tTVPProcessMemoryProvider TVPProcessMemoryProviderFunc = nullptr;

void TVPSetProcessMemoryProvider(tTVPProcessMemoryProvider provider)
{
	TVPProcessMemoryProviderFunc = provider;
}

TVPProcessMemoryInfo TVPGetProcessMemoryInfo()
{
	TVPProcessMemoryInfo info;

	// プラットフォーム層の実装があればそれを使う (組み機はここ経由)
	if (TVPProcessMemoryProviderFunc) {
		TVPProcessMemoryInfo tmp;
		if (TVPProcessMemoryProviderFunc(tmp)) return tmp;
	}

#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	if (::GetProcessMemoryInfo(::GetCurrentProcess(),
	                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
	                           sizeof(pmc))) {
		info.rss      = static_cast<size_t>(pmc.WorkingSetSize);
		info.peak_rss = static_cast<size_t>(pmc.PeakWorkingSetSize);
		info.vsize    = static_cast<size_t>(pmc.PrivateUsage);
	}
#elif defined(__APPLE__)
	mach_task_basic_info_data_t bi{};
	mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	              reinterpret_cast<task_info_t>(&bi), &cnt) == KERN_SUCCESS) {
		info.rss      = static_cast<size_t>(bi.resident_size);
		info.peak_rss = static_cast<size_t>(bi.resident_size_max);
		info.vsize    = static_cast<size_t>(bi.virtual_size);
	}
#elif defined(__linux__) || defined(__ANDROID__)
	// /proc/self/status から VmRSS / VmHWM (peak rss) / VmSize を読む。
	// 単位は kB なので 1024 倍する。
	if (FILE *fp = std::fopen("/proc/self/status", "r")) {
		char line[256];
		while (std::fgets(line, sizeof(line), fp)) {
			unsigned long kb = 0;
			if (std::sscanf(line, "VmRSS: %lu kB", &kb) == 1) {
				info.rss = static_cast<size_t>(kb) * 1024;
			} else if (std::sscanf(line, "VmHWM: %lu kB", &kb) == 1) {
				info.peak_rss = static_cast<size_t>(kb) * 1024;
			} else if (std::sscanf(line, "VmSize: %lu kB", &kb) == 1) {
				info.vsize = static_cast<size_t>(kb) * 1024;
			}
		}
		std::fclose(fp);
	}
#endif
	return info;
}

void TVPDumpProcessMemoryInfo()
{
	TVPProcessMemoryInfo info = TVPGetProcessMemoryInfo();
	TVPLOG_INFO("Process memory: rss={} peak_rss={} vsize={}",
	            TVPFormatBytes(info.rss),
	            TVPFormatBytes(info.peak_rss),
	            TVPFormatBytes(info.vsize));
}

std::string TVPSummarizeProcessMemory()
{
	TVPProcessMemoryInfo info = TVPGetProcessMemoryInfo();
	std::string out("Process: rss=");
	out += TVPFormatBytes(info.rss);
	out += " peak=";
	out += TVPFormatBytes(info.peak_rss);
	out += " vsize=";
	out += TVPFormatBytes(info.vsize);
	return out;
}
