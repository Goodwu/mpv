#ifndef MPV_YUV_COMPARE_VO_H
#define MPV_YUV_COMPARE_VO_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

// An absent property retains the default-off diagnostic. Malformed explicit
// values must not silently select a different capture mode.
static inline bool mp_yuv_compare_repeat_config(const char *value, bool *repeat)
{
    if (!value || !repeat)
        return false;
    *repeat = !strcmp(value, "1");
    return !value[0] || !strcmp(value, "0") || *repeat;
}

static inline bool mp_yuv_compare_tap_config(const char *value, bool repeat,
                                            bool *tap)
{
    return mp_yuv_compare_repeat_config(value, tap) && (!*tap || repeat);
}

static inline bool mp_yuv_compare_invariant_config(const char *value, bool repeat,
                                                  bool tap, bool *invariant)
{
    return mp_yuv_compare_repeat_config(value, invariant) &&
           (!*invariant || (repeat && tap));
}

static inline bool mp_yuv_compare_oracle_config(const char *value, const char *mode,
    bool repeat, bool tap, bool invariant, bool *oracle, bool *use_a)
{
    if (!mp_yuv_compare_repeat_config(value, oracle) ||
        !mp_yuv_compare_repeat_config(mode, use_a))
        return false;
    return *oracle ? (mode[0] && repeat && tap && !invariant) : !*use_a;
}

static inline bool mp_yuv_compare_probe_config(const char *value, bool oracle,
                                               bool *probe)
{
    return mp_yuv_compare_repeat_config(value, probe) && (!*probe || oracle);
}

static inline bool mp_yuv_compare_single_config(const char *value, bool oracle,
    bool probe, bool repeat, bool tap, bool invariant, bool *single)
{
    return mp_yuv_compare_repeat_config(value, single) &&
           (!*single || (oracle && probe && repeat && tap && !invariant));
}

static inline bool mp_yuv_compare_quad_config(const char *value, const char *mode,
    bool single, bool use_a, bool *quad, bool *reverse)
{
    if (!mp_yuv_compare_repeat_config(value, quad) ||
        !mp_yuv_compare_repeat_config(mode, reverse))
        return false;
    return *quad ? (mode[0] && single && !use_a) : !*reverse;
}

// Compare complete records, including padding and every attribute. This is a
// check of the submitted host payload, not evidence of GPU vertex fetching.
static inline bool mp_yuv_compare_quad_payloads_valid(const uint8_t *baseline,
    const uint8_t *candidate, size_t stride, size_t bytes, bool reverse)
{
    if (!baseline || !candidate || !stride || stride > 1024 || bytes != stride * 4)
        return false;
    for (size_t i = 0; i < 4; i++) {
        size_t source = reverse ? 3 - i : i;
        if (memcmp(candidate + i * stride, baseline + source * stride, stride))
            return false;
    }
    return true;
}

static inline bool mp_yuv_compare_quad_positions_valid(const uint8_t *vertices,
    size_t stride, size_t bytes, size_t offset)
{
    if (!vertices || stride < 8 || stride > 1024 || bytes != stride * 4 ||
        offset > stride - 8 || offset % 4 || stride % 4)
        return false;
    unsigned corners[4], seen = 0;
    for (size_t i = 0; i < 4; i++) {
        unsigned corner = 0;
        for (size_t c = 0; c < 2; c++) {
            uint32_t bits;
            memcpy(&bits, vertices + i * stride + offset + c * 4, 4);
            if (bits != 0x3f800000u && bits != 0xbf800000u)
                return false;
            if (bits == 0x3f800000u)
                corner |= 1u << c;
        }
        if (seen & (1u << corner))
            return false;
        seen |= 1u << corner;
        corners[i] = corner;
    }
    return seen == 15 && (corners[1] ^ corners[2]) == 3;
}

static inline bool mp_yuv_compare_barriers_valid_count(unsigned expected,
    unsigned count, const bool *ok,
    const uint64_t *before, const uint64_t *after, uint64_t render_epoch)
{
    if ((expected != 1 && expected != 2) || count != expected || !ok || !before || !after)
        return false;
    for (unsigned i = 0; i < expected; i++) {
        if (!ok[i] || before[i] != render_epoch || after[i] != render_epoch)
            return false;
    }
    return true;
}

static inline bool mp_yuv_compare_barriers_valid(unsigned count, const bool *ok,
    const uint64_t *before, const uint64_t *after, uint64_t render_epoch)
{
    return mp_yuv_compare_barriers_valid_count(2, count, ok, before, after, render_epoch);
}

static inline bool mp_yuv_compare_identifier_valid(const char *name, size_t capacity)
{
    if (!name || !capacity || !memchr(name, 0, capacity) || !name[0])
        return false;
    for (size_t i = 0; name[i]; i++) {
        char c = name[i];
        if (c != '_' && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(i && c >= '0' && c <= '9'))
            return false;
    }
    return true;
}

static inline bool mp_yuv_compare_invariant_vertex_valid(const char *candidate,
    const char *baseline, const char *uv, size_t uv_capacity)
{
    if (!candidate || !baseline ||
        !mp_yuv_compare_identifier_valid(uv, uv_capacity))
        return false;
    const char *main = strstr(baseline, "void main() {");
    if (!main || strstr(main + 1, "void main() {") || strstr(baseline, "invariant "))
        return false;
    char declarations[128];
    int n = snprintf(declarations, sizeof(declarations),
        "invariant gl_Position;\ninvariant %s;\n", uv);
    if (n < 0 || (size_t)n >= sizeof(declarations))
        return false;
    size_t prefix = main - baseline, inserted = n;
    if (strlen(candidate) != strlen(baseline) + inserted)
        return false;
    return !strncmp(candidate, baseline, prefix) &&
           !strncmp(candidate + prefix, declarations, inserted) &&
           !strcmp(candidate + prefix + inserted, main);
}

static inline bool mp_yuv_compare_draws_valid(bool repeat, unsigned repetitions,
                                             const bool *draw_ok,
                                             const uint64_t *program_tokens)
{
    unsigned expected = repeat ? 2 : 1;
    if (repetitions != expected || !draw_ok || !program_tokens)
        return false;
    for (unsigned i = 0; i < expected * 2; i++) {
        if (!draw_ok[i] || !program_tokens[i])
            return false;
    }
    return !repeat || (program_tokens[0] == program_tokens[2] &&
                       program_tokens[1] == program_tokens[3]);
}

static inline bool mp_yuv_compare_epochs_valid(unsigned repetitions,
                                              const uint64_t *before,
                                              const uint64_t *after,
                                              uint64_t render_epoch)
{
    if ((repetitions != 1 && repetitions != 2) || !before || !after)
        return false;
    // Include the VO's pre-render epoch so an error during program preparation
    // cannot be hidden by otherwise internally consistent draw receipts.
    for (unsigned i = 0; i < repetitions * 2; i++) {
        if (before[i] != render_epoch || after[i] != render_epoch)
            return false;
    }
    return true;
}

struct mp_yuv_compare_abort_plan {
    bool submit_frame, abort_no_gl, fatal;
};

// Context ownership and capture identity are intentionally separate. A stale
// capture under the owned context can end its frame normally without swapping.
static inline struct mp_yuv_compare_abort_plan mp_yuv_compare_abort_plan(
    bool owner_current, bool owner_restored, bool frame_open)
{
    bool safe_owner = owner_current || owner_restored;
    return (struct mp_yuv_compare_abort_plan) {
        .submit_frame = frame_open && safe_owner,
        .abort_no_gl = frame_open && !safe_owner,
        .fatal = !safe_owner,
    };
}

static inline bool mp_yuv_compare_target_matches(double source, double current,
                                                 double target)
{
    return isfinite(source) && isfinite(current) && isfinite(target) && target >= 0 &&
           fabs(source - target) <= 0.000002 && fabs(current - target) <= 0.000002;
}

static inline bool mp_yuv_compare_words_valid_layout(const uint32_t *words,
                                               size_t count, size_t pixels,
                                               unsigned repetitions,
                                               unsigned words_per_pixel,
                                               size_t *bad_word)
{
    if (!words || !pixels || (repetitions != 1 && repetitions != 2) ||
        (words_per_pixel != 14 && words_per_pixel != 26) ||
        (words_per_pixel == 26 && repetitions != 2) ||
        pixels > SIZE_MAX / (words_per_pixel * repetitions) ||
        count != pixels * words_per_pixel * repetitions)
        return false;
    for (size_t i = 0; i < count; i += words_per_pixel) {
        if (words[i] != 1 || words[i + 10] != 1 ||
            (words_per_pixel == 26 &&
             (words[i + 19] != 1 || words[i + 25] != 1))) {
            if (bad_word) *bad_word = i;
            return false;
        }
        for (size_t j = 1; j < words_per_pixel; j++) {
            if (j != 10 && j != 19 && j != 25 &&
                (words[i+j] & 0x7f800000u) == 0x7f800000u) {
                if (bad_word) *bad_word = i+j;
                return false;
            }
        }
    }
    return true;
}

static inline bool mp_yuv_compare_words_valid(const uint32_t *words,
                                               size_t count, size_t pixels,
                                               unsigned repetitions,
                                               size_t *bad_word)
{
    return mp_yuv_compare_words_valid_layout(words, count, pixels, repetitions,
                                             14, bad_word);
}

static inline bool mp_yuv_compare_oracle_uv_valid(const uint32_t *words,
    size_t count, size_t pixels, size_t *bad_word)
{
    if (!words || !pixels || pixels > SIZE_MAX / 52 || count != pixels * 52)
        return false;
    for (size_t i = 0; i < count; i += 26) {
        for (size_t c = 0; c < 2; c++) {
            if (words[i + 14 + c] != words[i + 20 + c]) {
                if (bad_word) *bad_word = i + 14 + c;
                return false;
            }
        }
    }
    return true;
}

// Schema 6 separates complete execution evidence from valid oracle input.
// A dependency failure can be retained as diagnostic-invalid, but missing
// coverage, malformed fields or nonfinite actual samples cannot be retained.
static inline bool mp_yuv_compare_android_words_valid(const uint32_t *words,
    size_t count, size_t pixels, size_t *bad_word)
{
    if (!words || !pixels || pixels > SIZE_MAX / 7 || count != pixels * 7)
        return false;
    for (size_t i = 0; i < count; i += 7) {
        if (words[i] != 1) {
            if (bad_word) *bad_word = i;
            return false;
        }
        for (size_t j = 1; j < 7; j++) {
            if ((words[i+j] & 0x7f800000u) == 0x7f800000u) {
                if (bad_word) *bad_word = i+j;
                return false;
            }
        }
    }
    return true;
}

static inline bool mp_yuv_compare_probe_words_valid_count(unsigned repetitions,
    const uint32_t *base,
    size_t base_count, const uint32_t *probe, size_t probe_count, size_t pixels,
    bool use_a, uint32_t *validation_mask, size_t *bad_base, size_t *bad_probe)
{
    if (validation_mask) *validation_mask = 0;
    if (!validation_mask || !base || !probe || !pixels ||
        (repetitions != 1 && repetitions != 2) ||
        pixels > SIZE_MAX / (26 * repetitions) ||
        base_count != pixels * 26 * repetitions ||
        probe_count != pixels * 10 * repetitions)
        return false;
    uint32_t mask = 0;
    for (size_t pixel = 0; pixel < pixels * repetitions; pixel++) {
        size_t b = pixel * 26, p = pixel * 10;
        if (base[b] != 1 || base[b+10] != 1 || base[b+25] != 1 || base[b+19] > 1) {
            if (bad_base) *bad_base = b;
            return false;
        }
        for (size_t j = 1; j < 26; j++) {
            if (j != 10 && j != 19 && j != 25 &&
                (base[b+j] & 0x7f800000u) == 0x7f800000u) {
                if (bad_base) *bad_base = b+j;
                return false;
            }
        }
        if (probe[p] != 1 || probe[p+1] != 1 || probe[p+2] != 1) {
            if (bad_probe) *bad_probe = p;
            return false;
        }
        uint32_t reason = probe[p+7], dep = probe[p+8], selected = probe[p+9];
        if (reason > 15 || dep > 1 || selected > 2) {
            if (bad_probe) *bad_probe = p+7;
            return false;
        }
        bool consistent;
        if (reason & 1) {
            // The shader returns before reading A when outside its ROI.
            consistent = reason == 1 && !probe[p+3] && !probe[p+4] &&
                         !probe[p+5] && !probe[p+6];
        } else {
            uint32_t expected = (probe[p+3] != 1 ? 2u : 0u) |
                                (probe[p+4] != 1 ? 4u : 0u) |
                (((probe[p+5] & 0x7f800000u) == 0x7f800000u ||
                  (probe[p+6] & 0x7f800000u) == 0x7f800000u) ? 8u : 0u);
            consistent = reason == expected;
        }
        if (reason || !dep) mask |= 1u; // GPU dependency invalid
        if (!consistent) mask |= 2u; // reason disagrees with observed inputs
        if (dep != (reason == 0)) mask |= 4u;
        if (selected != (dep ? (use_a ? 1u : 0u) : 2u)) mask |= 8u;
        if (base[b+19] != dep) mask |= 16u;
        // The observed GPU values and CPU's final A payload are distinct
        // evidence. A clean reason code alone cannot prove they agree.
        if (!(reason & 1) &&
            (probe[p+5] != base[b+20] || probe[p+6] != base[b+21]))
            mask |= 32u;
        if (selected == 1 &&
            (base[b+14] != probe[p+5] || base[b+15] != probe[p+6]))
            mask |= 64u;
    }
    *validation_mask = mask;
    return true;
}

static inline bool mp_yuv_compare_probe_words_valid(const uint32_t *base,
    size_t base_count, const uint32_t *probe, size_t probe_count, size_t pixels,
    bool use_a, uint32_t *validation_mask, size_t *bad_base, size_t *bad_probe)
{
    return mp_yuv_compare_probe_words_valid_count(2, base, base_count, probe,
        probe_count, pixels, use_a, validation_mask, bad_base, bad_probe);
}
#endif
