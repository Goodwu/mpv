#ifndef MPV_YUV_ATLAS_READ_H
#define MPV_YUV_ATLAS_READ_H
#include "yuv_atlas_anchor.h"

enum mp_yuv_atlas_read_phase {
    MP_ATLAS_OFF, MP_ATLAS_SEED, MP_ATLAS_SEED_PENDING,
    MP_ATLAS_T1_REDRAW, MP_ATLAS_T1_PENDING, MP_ATLAS_WAIT_ACK,
    MP_ATLAS_WAIT_T2, MP_ATLAS_T2_PENDING, MP_ATLAS_DONE, MP_ATLAS_REJECTED,
};

static inline bool mp_yuv_atlas_nonce_valid(const char *nonce)
{
    if (!nonce || strlen(nonce) != 32)
        return false;
    for (size_t i = 0; i < 32; i++) {
        if (!(nonce[i] >= '0' && nonce[i] <= '9') &&
            !(nonce[i] >= 'a' && nonce[i] <= 'f'))
            return false;
    }
    return true;
}

static inline bool mp_yuv_atlas_read_config(const char *value, bool anchor,
    double first, double second, const char *nonce, bool *enabled)
{
    return mp_yuv_compare_repeat_config(value, enabled) && (!*enabled ||
        (anchor && isfinite(first) && first >= 0 && isfinite(second) && second >= 0 &&
         fabs(first - second) > 0.000002 && mp_yuv_atlas_nonce_valid(nonce)));
}

static inline bool mp_yuv_atlas_ack_matches(const char *ack, const char *nonce,
                                            uint64_t cache_id)
{
    if (!ack || !cache_id || !mp_yuv_atlas_nonce_valid(nonce))
        return false;
    char expected[64];
    int n = snprintf(expected, sizeof(expected), "%s:%llu", nonce,
                     (unsigned long long)cache_id);
    return n > 0 && (size_t)n < sizeof(expected) && !strcmp(ack, expected);
}

// Structural coverage/finite/half-representability errors refuse the capture.
// Covered numerical differences are retained only as diagnostic-invalid.
static inline bool mp_yuv_atlas_read_data_valid(const uint32_t *a0,
    const uint32_t *uv, const uint32_t *half, const uint32_t *seed,
    size_t pixels, size_t rgba_words, size_t probe_words, size_t atlas_words,
    size_t *bad_uv, size_t *bad_half, uint32_t *mask)
{
    if (!a0 || !uv || !half || !seed || !pixels || pixels > MP_YUV_ATLAS_MAX_PIXELS ||
        rgba_words != pixels * 4 || probe_words != pixels * 4 || atlas_words != pixels * 3 ||
        !bad_uv || !bad_half || !mask)
        return false;
    *bad_uv = *bad_half = 0;
    *mask = 0;
    for (size_t i = 0; i < pixels; i++) {
        size_t p = i * 4, s = i * 3;
        if (uv[p] != 1 || uv[p + 3] != 1 || half[p] != 1 || seed[s + 2] != 1 ||
            (uv[p + 1] & 0x7f800000u) == 0x7f800000u ||
            (uv[p + 2] & 0x7f800000u) == 0x7f800000u ||
            (seed[s] & 0x7f800000u) == 0x7f800000u ||
            (seed[s + 1] & 0x7f800000u) == 0x7f800000u)
            return false;
        bool differs = false;
        for (size_t c = 0; c < 4; c++) {
            if (!mp_yuv_atlas_half_expanded(a0[p + c]))
                return false;
            if (c < 3) {
                if (!mp_yuv_atlas_half_expanded(half[p + c + 1]))
                    return false;
                differs |= half[p + c + 1] != a0[p + c];
            }
        }
        *bad_uv += uv[p + 1] != seed[s] || uv[p + 2] != seed[s + 1];
        *bad_half += differs;
    }
    *mask = (*bad_uv ? 1u : 0u) | (*bad_half ? 2u : 0u);
    return true;
}
#endif
