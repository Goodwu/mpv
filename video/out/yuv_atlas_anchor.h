#ifndef MPV_YUV_ATLAS_ANCHOR_H
#define MPV_YUV_ATLAS_ANCHOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "yuv_compare_vo.h"

#define MP_YUV_ATLAS_MAX_PIXELS 4194304u

static inline bool mp_yuv_atlas_config(const char *value, bool legacy,
                                       bool *enabled)
{
    return mp_yuv_compare_repeat_config(value, enabled) && (!*enabled || !legacy);
}

static inline bool mp_yuv_atlas_word_char(char c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// This conservative source guard complements the backend's actual descriptor
// check. It does not turn a textual scan into proof of driver compilation.
static inline bool mp_yuv_atlas_uninstrumented_source(const char *source)
{
    if (!source || !*source || strstr(source, "mks_") || strstr(source, "MKS_"))
        return false;
    for (const char *p = source; (p = strstr(p, "buffer")); p += 6) {
        if ((p == source || !mp_yuv_atlas_word_char(p[-1])) &&
            !mp_yuv_atlas_word_char(p[6]))
            return false;
    }
    return true;
}

// Legal RGBA/FLOAT decoding of finite binary16 numerical values. These are
// binary32 bit patterns, not the physical bytes stored in the RGB16F target.
static inline bool mp_yuv_atlas_half_expanded(uint32_t bits)
{
    uint32_t mag = bits & 0x7fffffffu;
    if (!mag)
        return true;
    unsigned exponent = mag >> 23;
    if (exponent >= 113 && exponent <= 142)
        return !(mag & 0x1fffu);
    if (exponent >= 103 && exponent <= 112)
        return !(mag & ((1u << (126 - exponent)) - 1));
    return false;
}

static inline bool mp_yuv_atlas_data_valid(const uint32_t *before,
    const uint32_t *middle, const uint32_t *after, const uint32_t *atlas,
    size_t pixels, size_t rgba_words, size_t atlas_words,
    size_t *before_middle, size_t *before_after, uint32_t *mask)
{
    if (!before || !middle || !after || !atlas || !pixels ||
        pixels > MP_YUV_ATLAS_MAX_PIXELS || rgba_words != pixels * 4 ||
        atlas_words != pixels * 3 || !before_middle || !before_after || !mask)
        return false;
    *before_middle = *before_after = 0;
    *mask = 0;
    for (size_t i = 0; i < rgba_words; i++) {
        if (!mp_yuv_atlas_half_expanded(before[i]) ||
            !mp_yuv_atlas_half_expanded(middle[i]) ||
            !mp_yuv_atlas_half_expanded(after[i]))
            return false;
        *before_middle += before[i] != middle[i];
        *before_after += before[i] != after[i];
    }
    for (size_t i = 0; i < atlas_words; i += 3) {
        if (atlas[i + 2] != 1 ||
            (atlas[i] & 0x7f800000u) == 0x7f800000u ||
            (atlas[i + 1] & 0x7f800000u) == 0x7f800000u)
            return false;
    }
    *mask = (*before_after ? 1u : 0u) | (*before_middle ? 2u : 0u);
    return true;
}
#endif
