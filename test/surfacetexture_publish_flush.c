#include <assert.h>
#include <math.h>

#include "video/out/surfacetexture_flush.h"

int main(void)
{
    const double allowed[] = {30000.0 / 1001.0, 29.97, 30.0, 30.0009};
    const double denied[] = {NAN, INFINITY, -INFINITY, 0, -30, 24, 25, 50,
                             60, 29.9, 30.0011};
    for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        assert(mp_st_publish_fps_allowed(allowed[i]));
        assert(mp_st_defer_publish(MP_ST_PUBLISH_DEFER_30FPS, allowed[i], -1));
        assert(!mp_st_defer_publish(MP_ST_PUBLISH_IMMEDIATE, allowed[i], -1));
        assert(!mp_st_defer_publish(2, allowed[i], -1));
        assert(!mp_st_defer_publish(MP_ST_PUBLISH_DEFER_30FPS, allowed[i], 0));
    }
    for (unsigned i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        assert(!mp_st_publish_fps_allowed(denied[i]));
        assert(!mp_st_defer_publish(MP_ST_PUBLISH_DEFER_30FPS, denied[i], -1));
        assert(!mp_st_defer_publish(MP_ST_PUBLISH_IMMEDIATE, denied[i], -1));
        // Only the explicit diagnostic override can bypass frame metadata.
        assert(mp_st_defer_publish(MP_ST_PUBLISH_IMMEDIATE, denied[i], 1));
    }
    assert(!mp_st_defer_publish(MP_ST_PUBLISH_IMMEDIATE, 30, -2));
    return 0;
}
