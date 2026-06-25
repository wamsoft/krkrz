/*
 * sound_alloc_stub.c
 *
 * krmovie.dll は libvorbis / libogg を直接リンクするが、これらは krkrz 本体の
 * SoundAllocator (sound_malloc / sound_calloc / sound_realloc / sound_free) へ
 * alloc を redirect するフック付きでビルドされている
 * (external/sound-codecs/hooks/sound_alloc_hook.h)。
 *
 * krmovie.dll は本体の SoundAllocator インフラ (TLSF プール / TVPAllocTag 統計、
 * common/base/SoundAllocator.cpp) を持たないリンク単位なので、ここでは標準
 * malloc/free にそのまま落とすだけのスタブを提供してリンクを満たす。
 *
 * 本体 (krkrz exe) 側は SoundAllocator.cpp の実装が使われ、このスタブとは別の
 * リンク単位なのでシンボル衝突しない。シグネチャは sound_alloc.h と同一。
 */
#include <stdlib.h>

void *sound_malloc(size_t size)               { return malloc(size); }
void *sound_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *sound_realloc(void *p, size_t size)     { return realloc(p, size); }
void  sound_free(void *p)                     { free(p); }
