/*
 * sound_alloc_hook.h
 *
 * Injected via -include / /FI into every libogg and libvorbis translation unit.
 * Replaces the four allocation macros defined in <ogg/os_types.h> with calls
 * into the krkrz SoundAllocator (sound_malloc / sound_calloc / sound_realloc /
 * sound_free).
 *
 * Mechanism:
 *   1. We #include <ogg/os_types.h> first so that libogg's own
 *      `#define _ogg_malloc malloc` etc. are processed and the include guard
 *      `_OS_TYPES_H` is set.
 *   2. We #undef the four macros and redefine them to forward to sound_*.
 *   3. Subsequent `#include <ogg/ogg.h>` etc. in the actual .c file re-include
 *      os_types.h but it is a no-op due to the include guard, so our override
 *      remains in effect.
 *
 * This file is C only — never include it from C++ translation units.
 */

#ifndef KRKRZ_SOUND_ALLOC_HOOK_H
#define KRKRZ_SOUND_ALLOC_HOOK_H

#include <ogg/os_types.h>
#include "sound_alloc.h"

#undef _ogg_malloc
#undef _ogg_calloc
#undef _ogg_realloc
#undef _ogg_free

#define _ogg_malloc(s)    sound_malloc(s)
#define _ogg_calloc(n,s)  sound_calloc((n),(s))
#define _ogg_realloc(p,s) sound_realloc((p),(s))
#define _ogg_free(p)      sound_free(p)

#endif /* KRKRZ_SOUND_ALLOC_HOOK_H */
