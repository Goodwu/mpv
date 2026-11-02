/* Default-off, low-rate internal call counters for Android thermal rounds.
 * These count pipeline work, never SurfaceFlinger/display presentations.
 * The property is sampled once per instance; enable before creating a VO.
 */
#ifndef MPV_MKS_THERMAL_H
#define MPV_MKS_THERMAL_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "common/msg.h"
#include "osdep/timer.h"

#if HAVE_ANDROID
#include <sys/system_properties.h>
#include <unistd.h>
#endif

struct mks_thermal_trace {
    bool enabled;
    struct mp_log *log;
    const char *kind;
    const void *instance;
    int64_t start_ns, next_ns;
    uint64_t queue_frames, queue_resets;
    uint64_t draw, redraw, display_synced, submit, flip;
    uint64_t lease_copy, lease_reuse, latch;
    bool defer_publish_flush;
    int flush_policy, flush_override;
    double nominal_fps;
    uint64_t fps_fallback;
    uint64_t publish_flush, publish_deferred, retire_flush;
    uint64_t fence_create_failed, reap_stuck, latch_timeout, stale_drop;
    uint64_t final_pass, swap, swap_ok;
    uint64_t frame_id;
    double pts;
    int w, h;
};

static inline void mks_thermal_report(struct mks_thermal_trace *t, bool final)
{
    if (!t->enabled)
        return;
    int64_t now = mp_time_ns();
    if (!final && now < t->next_ns)
        return;
    t->next_ns = now + INT64_C(10000000000);
    long pid = 0;
#if HAVE_ANDROID
    pid = (long)getpid();
#endif
    // Explicit opt-in diagnostics must remain visible with release error-only
    // logging. No readback, GPU timer, flush or per-frame formatting is used.
    mp_msg(t->log, MSGL_ERR,
        "MKS-THERMAL kind=%s instance=%p pid=%ld mono_ns=%"PRId64
        " start_ns=%"PRId64" final=%d queue_frames=%"PRIu64
        " queue_resets=%"PRIu64" draw=%"PRIu64" redraw=%"PRIu64
        " display_synced=%"PRIu64" submit=%"PRIu64" flip=%"PRIu64
        " lease_copy=%"PRIu64" lease_reuse=%"PRIu64" latch=%"PRIu64
        " defer_publish_flush=%d publish_flush=%"PRIu64
        " flush_req=%d flush_ovr=%d nominal_fps=%.6f fps_fallback=%"PRIu64
        " publish_deferred=%"PRIu64" retire_flush=%"PRIu64
        " fence_create_failed=%"PRIu64" reap_stuck=%"PRIu64
        " latch_timeout=%"PRIu64" stale_drop=%"PRIu64
        " final_pass=%"PRIu64" swap=%"PRIu64" swap_ok=%"PRIu64
        " frame_id=%"PRIu64" pts=%.9f w=%d h=%d\n",
        t->kind, t->instance, pid, now, t->start_ns, final,
        t->queue_frames, t->queue_resets, t->draw, t->redraw,
        t->display_synced, t->submit, t->flip, t->lease_copy, t->lease_reuse,
        t->latch, t->defer_publish_flush, t->publish_flush,
        t->flush_policy, t->flush_override, t->nominal_fps, t->fps_fallback,
        t->publish_deferred, t->retire_flush,
        t->fence_create_failed, t->reap_stuck,
        t->latch_timeout, t->stale_drop,
        t->final_pass, t->swap, t->swap_ok, t->frame_id, t->pts,
        t->w, t->h);
}

static inline void mks_thermal_init(struct mks_thermal_trace *t,
                                  struct mp_log *log, const char *kind,
                                  const void *instance)
{
#if HAVE_ANDROID
    char value[PROP_VALUE_MAX];
    t->enabled = __system_property_get("debug.mks.thermal_trace", value) > 0 &&
                 strcmp(value, "1") == 0;
#endif
    if (!t->enabled)
        return;
    t->log = log;
    t->kind = kind;
    t->instance = instance;
    t->start_ns = mp_time_ns();
    mks_thermal_report(t, false);
}

#endif
