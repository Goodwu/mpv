/* Explicit opt-in Android GPU elapsed diagnostics. No flush, finish or waits.
 * Each instance is owned by its GL thread/context. Results are sampled only
 * after availability; active external elapsed queries are never nested.
 */
#ifndef MPV_MKS_GPU_TIMER_H
#define MPV_MKS_GPU_TIMER_H
#include <EGL/egl.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "video/out/opengl/common.h"
#include "osdep/timer.h"
#include "common/msg.h"
#define MKS_GPU_RING 16
#define MKS_GPU_ELAPSED 0x88BF
#define MKS_GPU_DISJOINT 0x8FBB
#define MKS_GPU_CURRENT_QUERY 0x8865
#define MKS_GPU_COUNTER_BITS 0x8864
struct mks_gpu_timer {
    bool checked, enabled;
    GL *gl;
    EGLContext context;
    struct mp_log *log;
    const char *kind;
    void (GLAPIENTRY *get_query_iv)(GLenum, GLenum, GLint *);
    GLuint ids[MKS_GPU_RING];
    bool pending[MKS_GPU_RING];
    uint64_t seq[MKS_GPU_RING];
    double pts[MKS_GPU_RING];
    int64_t issue_ns[MKS_GPU_RING];
    int active;
    char samples[600];
    size_t sample_used;
    unsigned sample_count;
    uint64_t attempts, issued, results, disjoint_drop, ring_full, nested_skip;
    uint64_t context_skip, pending_drop, begin_failed, sum_ns, min_ns, max_ns;
    int64_t start_ns, report_ns;
};
static inline void mks_gpu_timer_samples(struct mks_gpu_timer *t)
{
    if (!t->sample_count) return;
    mp_msg(t->log, MSGL_ERR, "MKS-GPU-SAMPLES kind=%s instance=%p pid=%ld start_ns=%"PRId64
           " count=%u data=%s\n", t->kind, (void *)t, (long)getpid(),
           t->start_ns, t->sample_count, t->samples);
    t->sample_used=0; t->sample_count=0; t->samples[0]=0;
}
static inline void mks_gpu_timer_report(struct mks_gpu_timer *t, bool final)
{
    if (!t->enabled) return;
    int64_t now = mp_time_ns();
    if (!final && now < t->report_ns) return;
    t->report_ns = now + INT64_C(10000000000);
    mp_msg(t->log, MSGL_ERR, "MKS-GPU kind=%s instance=%p pid=%ld start_ns=%"PRId64
        " mono_ns=%"PRId64" final=%d attempts=%"PRIu64" issued=%"PRIu64
        " results=%"PRIu64" sum_ns=%"PRIu64" min_ns=%"PRIu64" max_ns=%"PRIu64
        " disjoint_drop=%"PRIu64" ring_full=%"PRIu64" nested_skip=%"PRIu64
        " context_skip=%"PRIu64" pending_drop=%"PRIu64" begin_failed=%"PRIu64"\n",
        t->kind, (void *)t, (long)getpid(), t->start_ns, now, final,
        t->attempts, t->issued, t->results, t->sum_ns,
        t->results ? t->min_ns : 0, t->max_ns, t->disjoint_drop,
        t->ring_full, t->nested_skip, t->context_skip, t->pending_drop, t->begin_failed);
}
static inline void mks_gpu_timer_init(struct mks_gpu_timer *t, GL *gl,
                                      struct mp_log *log, const char *kind)
{
    if (t->checked) return;
    t->checked = true;
    t->active = -1;
    char value[PROP_VALUE_MAX];
    if (__system_property_get("debug.mks.gpu_timing", value) <= 0)
        return;
    // GL_GPU_DISJOINT_EXT is a read-and-clear context-wide interval flag.
    // Only one category may own it in a round. Renderer timers must be
    // disabled externally in input/yuv rounds, not merely hidden in logs.
    const char *mode = strcmp(kind, "input-copy") == 0 ? "input" : "yuv";
    if (strcmp(value, mode)) return;
    t->gl = gl; t->log = log; t->kind = kind;
    t->context = eglGetCurrentContext();
    t->get_query_iv = (void *)eglGetProcAddress("glGetQueryivEXT");
    if (t->context == EGL_NO_CONTEXT || !t->get_query_iv ||
        !gl->GenQueries || !gl->DeleteQueries || !gl->BeginQuery ||
        !gl->EndQuery || !gl->GetQueryObjectuiv || !gl->GetQueryObjectui64v ||
        !gl->extensions || !strstr(gl->extensions, "GL_EXT_disjoint_timer_query")) {
        mp_msg(log, MSGL_ERR, "MKS-GPU kind=%s supported=0 reason=entrypoints-or-extension\n", kind);
        return;
    }
    GLint bits = 0; t->get_query_iv(MKS_GPU_ELAPSED, MKS_GPU_COUNTER_BITS, &bits);
    if (bits <= 0) {
        mp_msg(log, MSGL_ERR, "MKS-GPU kind=%s supported=0 reason=counter-bits\n", kind);
        return;
    }
    gl->GenQueries(MKS_GPU_RING, t->ids);
    for (int i=0; i<MKS_GPU_RING; i++) {
        if (!t->ids[i]) {
            gl->DeleteQueries(MKS_GPU_RING, t->ids);
            memset(t->ids, 0, sizeof(t->ids));
            mp_msg(log, MSGL_ERR, "MKS-GPU kind=%s supported=0 reason=query-allocation\n", kind);
            return;
        }
    }
    t->enabled = true; t->min_ns = UINT64_MAX; t->start_ns = mp_time_ns();
    mp_msg(log, MSGL_ERR, "MKS-GPU kind=%s supported=1 bits=%d ring=%d scope=elapsed-draw\n", kind, bits, MKS_GPU_RING);
}
static inline bool mks_gpu_timer_context(struct mks_gpu_timer *t)
{
    if (!t->enabled) return false;
    if (eglGetCurrentContext() != t->context) { t->context_skip++; return false; }
    return true;
}
static inline void mks_gpu_timer_poll(struct mks_gpu_timer *t)
{
    GL *gl = t->gl;
    // A disjoint interval invalidates every in-flight result. Query IDs are
    // safe to reuse after EndQuery; never synchronously drain them.
    GLint disjoint = 0; gl->GetIntegerv(MKS_GPU_DISJOINT, &disjoint);
    if (disjoint) {
        for (int i=0; i<MKS_GPU_RING; i++) {
            if (t->pending[i]) { t->disjoint_drop++; t->pending[i]=false; }
        }
        return;
    }
    for (int i=0; i<MKS_GPU_RING; i++) {
        if (!t->pending[i]) continue;
        GLuint ready=0; gl->GetQueryObjectuiv(t->ids[i], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue;
        GLuint64 ns=0; gl->GetQueryObjectui64v(t->ids[i], GL_QUERY_RESULT, &ns);
        // DVFS can disjoint between the initial check and availability read.
        gl->GetIntegerv(MKS_GPU_DISJOINT, &disjoint);
        if (disjoint) {
            for (int j=0; j<MKS_GPU_RING; j++) {
                if (t->pending[j]) { t->disjoint_drop++; t->pending[j]=false; }
            }
            return;
        }
        t->pending[i]=false; t->results++; t->sum_ns+=ns;
        if (ns<t->min_ns) t->min_ns=ns;
        if (ns>t->max_ns) t->max_ns=ns;
        // Bounded batches preserve every result as seq:PTS:issue_mono_ns:elapsed_ns.
        // Flush before capacity exhaustion, never truncate a sample.
        char sample[96];
        int len=snprintf(sample, sizeof(sample), "%s%"PRIu64":%.9f:%"PRId64":%"PRIu64,
                         t->sample_count ? "," : "", t->seq[i], t->pts[i],
                         t->issue_ns[i], (uint64_t)ns);
        if (len <= 0 || (size_t)len >= sizeof(sample)) {
            t->pending_drop++; continue;
        }
        if (t->sample_used + len >= sizeof(t->samples)) {
            mks_gpu_timer_samples(t);
            // Remove the previous batch's separator.
            if (sample[0] == ',') { memmove(sample, sample+1, len); len--; }
        }
        memcpy(t->samples+t->sample_used, sample, len+1);
        t->sample_used+=len; t->sample_count++;
        if (t->sample_count >= 8) mks_gpu_timer_samples(t);
    }
}
static inline bool mks_gpu_timer_begin(struct mks_gpu_timer *t, GL *gl,
                                       struct mp_log *log, const char *kind, double pts)
{
    mks_gpu_timer_init(t, gl, log, kind);
    if (!mks_gpu_timer_context(t)) return false;
    t->attempts++;
    GLint current=0; t->get_query_iv(MKS_GPU_ELAPSED, MKS_GPU_CURRENT_QUERY, &current);
    if (current || t->active >= 0) { t->nested_skip++; return false; }
    mks_gpu_timer_poll(t);
    mks_gpu_timer_report(t, false);
    for (int i=0; i<MKS_GPU_RING; i++) {
        if (t->pending[i]) continue;
        t->active=i; t->seq[i]=t->attempts; t->pts[i]=pts;
        t->issue_ns[i]=mp_time_ns();
        gl->BeginQuery(MKS_GPU_ELAPSED, t->ids[i]);
        GLint started=0;
        t->get_query_iv(MKS_GPU_ELAPSED, MKS_GPU_CURRENT_QUERY, &started);
        if ((GLuint)started != t->ids[i]) {
            t->active=-1; t->begin_failed++;
            return false;
        }
        t->issued++;
        return true;
    }
    t->ring_full++; return false;
}
static inline void mks_gpu_timer_end(struct mks_gpu_timer *t, bool active)
{
    if (!active) return;
    t->gl->EndQuery(MKS_GPU_ELAPSED);
    t->pending[t->active]=true; t->active=-1;
}
static inline void mks_gpu_timer_uninit(struct mks_gpu_timer *t)
{
    if (!mks_gpu_timer_context(t)) {
        if (t->enabled) {
            mp_msg(t->log, MSGL_ERR, "MKS-GPU kind=%s incomplete=1 reason=uninit-context-mismatch\n", t->kind);
            mks_gpu_timer_samples(t);
            mks_gpu_timer_report(t, true);
        }
        return;
    }
    mks_gpu_timer_poll(t);
    for (int i=0; i<MKS_GPU_RING; i++) if (t->pending[i]) t->pending_drop++;
    mks_gpu_timer_samples(t);
    mks_gpu_timer_report(t, true);
    t->gl->DeleteQueries(MKS_GPU_RING, t->ids);
    t->enabled=false;
}
#endif
