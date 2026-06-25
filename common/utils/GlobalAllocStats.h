#pragma once

// グローバル new/delete + TJS_malloc + SDL3 内部 alloc を一元集計するための
// 観測 + プールベース allocator。doc/legacy/GlobalAllocationStats.md の案 A
// (operator new override) を「mimalloc 化を伴わない最小スコープ」で実装し、
// さらに BitmapPool/FilePool 同様の事前確保プール (TLSF) を被せて、
// 環境間メモリ確保パターンの再現性検証 + 上限超過の検知ができる形にしたもの。
//
// 動作モード:
//   pre-init (Initialize 未呼出の状態)
//     - operator new / delete / TJS_malloc / Sdl* wrapper はすべて素の
//       std::malloc / std::free / std::calloc / std::realloc に直行する
//     - header 付与なし、stats なし、pool なし。CRT default と同じ振る舞い
//     - 起動極初期 (static initializer / SDL_Init / kirikiri config 読み出し
//       時の小規模 alloc 等) は本モードで通る
//   post-init (Initialize 呼出後)
//     - 全 alloc に 16 byte header + magic を付与
//     - pool が紐付いていれば pool 経由 (TLSF + 内部 fallback)
//     - pool なしなら素 std::malloc + magic header (= stats だけ取る)
//     - free 時は header magic で経路を識別 (pre-init 由来のポインタは
//       magic mismatch で素 std::free に流れる)
//
// 捕捉範囲 (post-init 時):
//   Krkrz collector
//     - 本体 exe 内の `::operator new` / `new[]` / nothrow 版 + 対応 delete
//     - tjsConfig.h で TJS_malloc/TJS_free/TJS_realloc を redirect した分
//     - 静的リンクされている vcpkg ライブラリのうち、`new` 経由 (C++) のもの
//   Sdl collector
//     - SDL_SetMemoryFunctions で差し替えた SDL_malloc/calloc/realloc/free
//
// 捕捉外 (= 既存の per-allocator stats / OS RSS で見る):
//   - libpng / libjpeg / miniaudio / ICU 等の C ライブラリの素 malloc
//   - プラグイン DLL 内の new/malloc (独立 CRT)
//   - ANGLE (libEGL.dll / libGLESv2.dll) 内部
//   - HeapAlloc/VirtualAlloc 直叩き (TLSF プールの backing 等)

#include <cstddef>
#include <cstdint>
#include <string>

namespace TVPGlobalAllocStats {

// tag 別 / サイズビン別の breakdown は固定上限を使う。
// kMaxTags >= TVPAllocTag::_Count を満たしておけば enum 拡張に追従できる。
constexpr int kMaxTags = 16;
constexpr int kSizeHistBins = 8;  // <128 / <1K / <16K / <256K / <4M / <64M / <1G / >=1G

struct TagStats {
	uint64_t alloc_count     = 0;
	uint64_t total_allocated = 0;
	uint64_t free_count      = 0;
	uint64_t total_freed     = 0;
	uint64_t current_used    = 0;
};

struct Snapshot {
	uint64_t alloc_count = 0;     // 累積 alloc 回数 (post-init のみ)
	uint64_t alloc_bytes = 0;     // 累積 alloc バイト
	uint64_t free_count  = 0;
	uint64_t free_bytes  = 0;
	uint64_t live_bytes  = 0;     // alloc - free (現時点の生存)
	uint64_t peak_bytes  = 0;     // live のピーク
	// pool 関連 (pool 未紐付け時はすべて 0)
	uint64_t pool_capacity = 0;   // TVPPooledAllocator::capacity() 申告値
	uint64_t pool_used     = 0;   // pool 内 (TLSF block 単位、header 込み) 使用量
	uint64_t pool_peak     = 0;   // pool 内ピーク (Stats.peak_used)
	uint64_t fallback_count = 0;  // pool 枯渇 → system malloc fallback の回数
	uint64_t fallback_bytes = 0;  // fallback 経由で現在 outstanding なバイト数
	// サイズビン別 alloc 回数 (doc/legacy/MemoryInspection.md §3.2 と同じビン区分)
	uint64_t alloc_size_hist[kSizeHistBins] = {};
	// tag 別 (TVPAllocTag enum value を index に使う)
	TagStats tag_stats[kMaxTags] = {};
};

// 観測 API
Snapshot GetKrkrzStats();
Snapshot GetSdlStats();

void ResetKrkrzPeak();
void ResetSdlPeak();

// REPL :mem 用 1 行サマリ
std::string Summarize();
// TVPHeapDump 用 INFO ログ出力 (Krkrz / Sdl 各 1 行 + pool 状態)
void Dump();

// プール初期化。本関数を呼び出すまで全 alloc は素 malloc/free 直行
// (オーバーヘッドゼロ、stats なし、pool なし)。本関数で:
//   1. TVPGetCommandLine から `-krkrzpoolsize` / `-sdlpoolsize` (MB、
//      none/off/0 で無効化、未指定なら既定値 256/64) を読み出す
//   2. pool を構築 (cap=0 なら pool なしで stats のみ)
//   3. tracking flag を on にする — 以降の alloc は header + 経路振り分け
//
// 多重呼び出しは ignore (warning ログのみ)。
//
// 呼び出しタイミング: TVPGetCommandLine が使えるようになった直後、
// すなわち Application オブジェクトが組み上がって AppPath() が返せる状態。
// SDL3 版は app->InitPath() 完了後、WINVER 版は `Application = new ...`
// 直後あたり。これより前の alloc (SDL_Init / static initializer 等) は
// pre-init モードで素 malloc を通る。
void Initialize();

// SDL_SetMemoryFunctions に渡す wrapper。SDL_AppInit 冒頭で SDL_Init より前に
// 1 回だけ仕込む。pre-init 時は素 std::malloc にパススルーするので、
// SDL_SetMemoryFunctions の設置は Initialize より前で構わない。
void *SdlMalloc(size_t size);
void *SdlCalloc(size_t nmemb, size_t size);
void *SdlRealloc(void *p, size_t size);
void  SdlFree(void *p);

} // namespace TVPGlobalAllocStats

// TJS_malloc 系の redirect 先 (extern "C" linkage で tjsConfig.h の
// マクロから呼べる形)。実装は GlobalAllocStats.cpp。
#ifdef __cplusplus
extern "C" {
#endif
void *TVPKrkrzMalloc(size_t size);
void *TVPKrkrzCalloc(size_t nmemb, size_t size);
void *TVPKrkrzRealloc(void *p, size_t size);
void  TVPKrkrzFree(void *p);
#ifdef __cplusplus
}
#endif

// Krkrz Allocator のプールサイズ取得 (CLI -krkrzpoolsize 優先)。
// カスタム allocator 実装時に参照可能。
size_t TVPGetKrkrzAllocatorPoolSize();
