#ifndef MPV_SURFACETEXTURE_FLUSH_H
#define MPV_SURFACETEXTURE_FLUSH_H

#include <math.h>
#include <stdbool.h>

enum mp_surfacetexture_publish_flush {
    MP_ST_PUBLISH_IMMEDIATE = 0,
    MP_ST_PUBLISH_DEFER_30FPS = 1,
};

// This is an encoded-frame metadata gate, not a measured submission-rate gate.
// Unknown and unvalidated frame rates retain the original immediate flush.
static inline bool mp_st_publish_fps_allowed(double fps)
{
    return isfinite(fps) && fps > 0.0 &&
        (fabs(fps - 30000.0 / 1001.0) <= 1e-3 || fabs(fps - 30.0) <= 1e-3);
}

// override=-1 is normal owned-option operation. 0/1 are explicit diagnostics,
// sampled only at mapper creation; they are never a product configuration.
static inline bool mp_st_defer_publish(int policy, double fps, int override)
{
    if (override == 0 || override == 1)
        return override == 1;
    return policy == MP_ST_PUBLISH_DEFER_30FPS && mp_st_publish_fps_allowed(fps);
}

#endif
