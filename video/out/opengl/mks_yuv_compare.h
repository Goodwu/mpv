#ifndef MP_MKS_YUV_COMPARE_H
#define MP_MKS_YUV_COMPARE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
struct ra_ctx;
// Capture never displaces an existing indexed buffer/range binding.
static inline bool mks_yuv_compare_binding_idle(int binding) { return binding == 0; }
static inline bool mks_yuv_compare_context_matches(uintptr_t expected_context,
    uintptr_t expected_surface, uintptr_t current_context, uintptr_t current_surface)
{
    return expected_context && expected_surface &&
           expected_context == current_context && expected_surface == current_surface;
}
struct mks_yuv_compare_roi { int x, y, w, h; };
enum mks_yuv_compare_status {
    MKS_YUV_COMPARE_IDLE, MKS_YUV_COMPARE_ARMED, MKS_YUV_COMPARE_PENDING,
    MKS_YUV_COMPARE_READY, MKS_YUV_COMPARE_FAILED,
};
// Pixel words: invocation count, sampled RGB float bits, output YUV float bits.
// READY data is owned by ctx and remains valid until cancel/arm/teardown.
struct mks_yuv_compare_result {
    uint64_t capture_id, generation;
    struct mks_yuv_compare_roi roi;
    enum mks_yuv_compare_status status;
    const uint32_t *words;
    size_t num_words;
    bool swap_succeeded;
};
static inline bool mks_yuv_compare_restore_allowed(uint64_t id, bool attempted,
                                                   bool live, bool same_thread)
{
    return id && !attempted && live && same_thread;
}
// Owner identity is independent of whether a capture exists or is valid.
bool mks_android_yuv_compare_owner_current(struct ra_ctx *);
// VO owner thread only, outside all libplacebo calls. At most one attempt per
// armed transaction; uses CURRENT live Android handles, never captured handles.
// Capture remains FAILED even if restoration succeeds; caller must not swap/retry.
bool mks_android_yuv_compare_restore_owner(struct ra_ctx *);
// Pure owner validation; does not prove capture success. FAILED transactions
// may still be current for safe cancellation/cleanup.
bool mks_android_yuv_compare_current(struct ra_ctx *, uint64_t capture_id,
                                   uint64_t generation);
bool mks_android_yuv_compare_arm(struct ra_ctx *, uint64_t capture_id,
                               uint64_t generation, struct mks_yuv_compare_roi);
enum mks_yuv_compare_status mks_android_yuv_compare_poll(
    struct ra_ctx *, struct mks_yuv_compare_result *);
void mks_android_yuv_compare_cancel(struct ra_ctx *);
#endif
