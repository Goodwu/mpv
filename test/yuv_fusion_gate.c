#include <assert.h>
#include <math.h>

#include "video/out/yuv_fusion_gate.h"

#if PL_API_VER >= 346
int main(void)
{
    // All non-blur modes remain eligible even with the renderer's inert
    // default blur radius. The helper deliberately has no radius argument.
    for (int background = PL_CLEAR_COLOR; background <= PL_CLEAR_SKIP; background++) {
        for (int border = PL_CLEAR_COLOR; border <= PL_CLEAR_SKIP; border++) {
            assert(mp_yuv_fusion_background_eligible(0, background, border));
            assert(mp_yuv_fusion_background_eligible(-0.0f, background, border));
            const float rounded[] = {0.0001f, -0.0001f, 1.0f, NAN, INFINITY, -INFINITY};
            for (unsigned i = 0; i < sizeof(rounded) / sizeof(rounded[0]); i++)
                assert(!mp_yuv_fusion_background_eligible(rounded[i], background, border));
        }
    }
    // Blur stays ineligible even with full coverage or zero configured radius.
    // Unknown enum values must not accidentally pass a range check.
    const int rejected[] = {-1, PL_CLEAR_MODE_COUNT, 100,
#if PL_API_VER >= 355
                            PL_CLEAR_BLUR,
#endif
    };
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        assert(!mp_yuv_fusion_background_eligible(0, rejected[i], PL_CLEAR_COLOR));
        assert(!mp_yuv_fusion_background_eligible(0, PL_CLEAR_COLOR, rejected[i]));
    }
    return 0;
}

#else
int main(void) { return 77; } // clear-mode API unavailable: skipped
#endif
