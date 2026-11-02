#ifndef MPV_YUV_FUSION_GATE_H
#define MPV_YUV_FUSION_GATE_H

#include <stdbool.h>
#include <libplacebo/renderer.h>

#if PL_API_VER >= 346

// A configured blur radius is inert unless the corresponding mode uses it.
// Eligibility still requires full target coverage and opaque video elsewhere.
// Reject actual rounding, blur, and unknown clear modes conservatively.
static inline bool mp_yuv_fusion_background_eligible(
    float corner_rounding, enum pl_clear_mode background,
    enum pl_clear_mode border)
{
    return corner_rounding == 0 &&
        (background == PL_CLEAR_COLOR || background == PL_CLEAR_TILES ||
         background == PL_CLEAR_SKIP) &&
        (border == PL_CLEAR_COLOR || border == PL_CLEAR_TILES ||
         border == PL_CLEAR_SKIP);
}

#endif // PL_API_VER >= 346

#endif
