#pragma once

// プロセス全体のメモリ使用量を OS から取得するためのインタフェース
// (doc/legacy/MemoryBudgetNegotiation.md §12.1 / doc/legacy/GlobalAllocationStats.md 案 C)。
// iTVPMemoryAllocator ベースの per-allocator stats では捕捉できない部分
// (libpng / miniaudio / SDL3 等の C ライブラリ、プラグイン DLL、TJS 素 new
// 等) を含む全 heap + VirtualAlloc の OS 視点の使用量を見るための補助。
//
// 失敗時 (未対応 OS や API 取得失敗) は SIZE_MAX を返す。

#include <cstddef>
#include <cstdint>

struct TVPProcessMemoryInfo {
	size_t rss      = SIZE_MAX; // Resident Set Size (物理常駐サイズ)
	size_t peak_rss = SIZE_MAX; // ピーク RSS
	size_t vsize    = SIZE_MAX; // Virtual Memory Size (commit 済み + reserved)
};

// 現在のプロセスのメモリ情報を取得。
TVPProcessMemoryInfo TVPGetProcessMemoryInfo();

// ログに 1 行で出力するヘルパ。TVPHeapDump や周期ダンプから呼ぶ。
void TVPDumpProcessMemoryInfo();

// REPL 等への 1 行サマリ用文字列を返す。
#include <string>
std::string TVPSummarizeProcessMemory();
