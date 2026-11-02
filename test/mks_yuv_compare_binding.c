#include <assert.h>
#include <limits.h>
#include "video/out/opengl/mks_yuv_compare.h"
int main(void) {
    assert(mks_yuv_compare_binding_idle(0));
    assert(!mks_yuv_compare_binding_idle(1));
    assert(!mks_yuv_compare_binding_idle(INT_MAX));
    assert(!mks_yuv_compare_binding_idle(-1));
    assert(mks_yuv_compare_context_matches(1, 2, 1, 2));
    assert(!mks_yuv_compare_context_matches(1, 2, 3, 2));
    assert(!mks_yuv_compare_context_matches(1, 2, 1, 3));
    assert(!mks_yuv_compare_context_matches(0, 2, 0, 2));
    assert(!mks_yuv_compare_context_matches(1, 0, 1, 0));
    assert(mks_yuv_compare_restore_allowed(1, false, true, true));
    assert(!mks_yuv_compare_restore_allowed(0, false, true, true));
    assert(!mks_yuv_compare_restore_allowed(1, true, true, true));
    assert(!mks_yuv_compare_restore_allowed(1, false, false, true));
    assert(!mks_yuv_compare_restore_allowed(1, false, true, false));
    return 0;
}
