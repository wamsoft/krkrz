/*
 * sound_alloc.h
 *
 * Pure-C declarations for the krkrz SoundAllocator front-end.
 * Included from -include / /FI hooks injected into libogg/libvorbis
 * compilation units, so this header MUST stay C-compatible (no C++).
 *
 * Implementation lives in krkrz/common/base/SoundAllocator.cpp.
 */

#ifndef KRKRZ_SOUND_ALLOC_H
#define KRKRZ_SOUND_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *sound_malloc(size_t size);
void *sound_calloc(size_t nmemb, size_t size);
void *sound_realloc(void *p, size_t size);
void  sound_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* KRKRZ_SOUND_ALLOC_H */
