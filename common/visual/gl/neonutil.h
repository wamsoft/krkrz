#ifndef __NEON_UTIL_H__
#define __NEON_UTIL_H__

#include <arm_neon.h>

#define B_VEC_IDX 0
#define G_VEC_IDX 1
#define R_VEC_IDX 2
#define A_VEC_IDX 3

#define B_VEC(x) x.val[B_VEC_IDX]
#define G_VEC(x) x.val[G_VEC_IDX]
#define R_VEC(x) x.val[R_VEC_IDX]
#define A_VEC(x) x.val[A_VEC_IDX]

// 使うと若干性能が良くなるっぽく見える
#define NEON_BLEND_PIXEL_PREFETCH(sp, dp, n)                                             \
    {                                                                                    \
        __builtin_prefetch(sp + n, 0, 0);                                                \
        __builtin_prefetch(dp + n, 0, 0);                                                \
    }

#endif