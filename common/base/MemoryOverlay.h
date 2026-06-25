#pragma once

// メモリ状態リアルタイム監視オーバレイ用の状態保持 + サンプラ。
// SDL3 ビルド限定で画面右上に折れ線グラフを表示する機能の OS 共通部分。
// 描画は sdl3/visual/MemoryOverlayRender.cpp が本ヘッダの GetSnapshot()
// を呼んで画面に描画する。
//
// SetEnabled(true) でサンプラ thread を起動し、4Hz で stats を収集して
// 256 件のリングバッファに保持する。SetEnabled(false) で停止。
// 背景スレッドは Initialize / Finalize で起動・停止する。
// (Enabled = false の場合は無駄な収集をしないので OFF 時はほぼ無コスト)

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct TVPMemoryOverlaySample {
	uint64_t tick_ms              = 0;
	size_t   file_used            = 0; // FileAllocator current_used
	size_t   file_peak            = 0; // FileAllocator peak_used (resetPeak でクリア可)
	size_t   bitmap_used          = 0; // BitmapAllocator current_used (Sized mode 化済み)
	size_t   bitmap_peak          = 0; // BitmapAllocator peak_used (resetPeak でクリア可)
	size_t   sound_used           = 0; // SoundAllocator current_used (Sized mode)
	size_t   sound_peak           = 0; // SoundAllocator peak_used (resetPeak でクリア可)
	size_t   process_rss          = 0;
	uint64_t file_alloc_count     = 0; // 累積 (rate は呼び出し側で delta 計算)
	uint64_t bitmap_alloc_count   = 0;
	uint64_t sound_alloc_count    = 0;
	// キャッシュエントリ件数 (file 層 = StorageCache / decode 層 = TVPGraphicCache)。
	// pinned 数は内訳。non-pinned = total - pinned。
	size_t   file_cache_count     = 0;
	size_t   file_cache_pinned    = 0;
	size_t   image_cache_count    = 0;
	size_t   image_cache_pinned   = 0;
	// GlobalAllocStats (TVPGlobalAllocStats) からの値。tracking 未活性なら全 0。
	// pool_cap = 0 は pool 無効化モード (= -krkrzpoolsize=none)。
	// fallback_count > 0 は pool 容量超過の累積回数。
	uint64_t krkrz_live           = 0;
	uint64_t krkrz_pool_used      = 0;
	uint64_t krkrz_pool_cap       = 0;
	uint64_t krkrz_fallback_count = 0;
	uint64_t sdl_live             = 0;
	uint64_t sdl_pool_used        = 0;
	uint64_t sdl_pool_cap         = 0;
	uint64_t sdl_fallback_count   = 0;
	// システムアロケータ情報 (iTVPSystemAllocatorInfo 経由)
	// コンソール機等のプラットフォーム固有実装ではより正確な値が入る。
	// 一般 OS ではシステムの空き物理メモリが近似値として入る。
	// 取得不可 (SIZE_MAX) は 0 にマップ。
	uint64_t sys_total_free       = 0; // 空き領域合計 (GetTotalFreeSize 相当)
	uint64_t sys_allocatable      = 0; // 確保可能最大サイズ (GetAllocatableSize 相当)
	uint64_t sys_avail_physical   = 0; // システム空き物理メモリ
};

namespace TVPMemoryOverlay {

constexpr size_t kMaxSamples = 256;
constexpr int    kSampleIntervalMs = 250; // 4Hz

void Initialize();
void Finalize();

void SetEnabled(bool enabled);
bool IsEnabled();

// 現在のリングバッファ内容を新しい順で取得。Enabled=false なら空。
// 戻り値の vector は最大 kMaxSamples 件。
void GetSnapshot(std::vector<TVPMemoryOverlaySample> &out);

} // namespace TVPMemoryOverlay
