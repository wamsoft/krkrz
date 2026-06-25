#pragma once

#include <cstddef>

//------------------------------------------------------
// サウンド用バッファのアロケータ
// FileAllocator と同形 (16-byte header に size + tag を埋め込み、
// sound_free 単独で size を復元) で iTVPMemoryAllocator を覆い、
// TVPAllocTag::Sound に紐づく独立統計を取れるようにする。
//
// 用途は common/sound/ 配下の PCM / リング / DSP 一時バッファ等で、
// audio callback (ma_data_source_read) からは呼び出さないこと
// (decode thread / main thread からのみ)。
//------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

void *sound_malloc(size_t size);
void *sound_calloc(size_t nmemb, size_t size);
void *sound_realloc(void *p, size_t size);
void  sound_free(void *p);

#ifdef __cplusplus
} // extern "C"
#endif

// アロケータ初期化 (TVPInitializeBaseSystems から呼ばれる)。
// FileAllocator と同じタイミングで初期化されることを前提に、
// CLI option (TVPGetCommandLine) は利用可能な段階で呼び出すこと。
void TVPInitializeSoundAllocator();

// テレメトリ取得用 (TVPHeapDump / System.dumpHeap / REPL .mem 経由)。
// 未初期化の場合は nullptr。
class iTVPMemoryAllocator;
iTVPMemoryAllocator *TVPGetSoundAllocator();

// SoundAllocator のプールサイズ取得 (CLI -soundpoolsize 優先、既定 128MB、
// none/off/0 で pool 無効化)。カスタム allocator 実装時に参照可能。
size_t TVPGetSoundAllocatorPoolSize();
