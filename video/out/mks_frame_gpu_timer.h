/* M2' frame-scoped GPU elapsed timing owner.
 * Design: docs/plans/lg-p84-frame-gpu-timing-design-20261108.md (r7).
 * Uses only fundamental C types — production wiring bridges mpv's GL.
 */
#ifndef MPV_MKS_FRAME_GPU_TIMER_H
#define MPV_MKS_FRAME_GPU_TIMER_H
#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif
#ifndef __ANDROID__
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef __ANDROID__
#include <EGL/egl.h>
#endif
#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define MKS_FGT_SLOTS 64
#define MKS_FGT_MAX_SEGS 8
#define MKS_FGT_TICKETS 40
#define MKS_FGT_HOL_AHEAD 32
#define MKS_FGT_MAX_COPIES 128
#define MKS_FGT_ELAPSED 0x88BF
#define MKS_FGT_DISJOINT 0x8FBB
#define MKS_FGT_CURRENT_QUERY 0x8865
#define MKS_FGT_COUNTER_BITS 0x8864
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_QUERY_RESULT 0x8866

enum mks_fgt_mode {
    MKS_FGT_MODE_OFF, MKS_FGT_MODE_FRAME, MKS_FGT_MODE_SPAN, MKS_FGT_MODE_COUNT,
};
enum mks_fgt_kind {
    MKS_FGT_K_RENDER_PRE, MKS_FGT_K_COPY, MKS_FGT_K_RENDER_POST,
    MKS_FGT_K_RENDER_TAIL, MKS_FGT_K_SPAN, MKS_FGT_K_FINAL,
};
enum mks_fgt_tstate {
    MKS_FGT_TS_NONE, MKS_FGT_TS_ISSUED, MKS_FGT_TS_RESOLVED,
    MKS_FGT_TS_DISJOINT, MKS_FGT_TS_PENDING_DROP, MKS_FGT_TS_HOL_DROP,
    MKS_FGT_TS_CTX_LOST, MKS_FGT_TS_NOT_TIMED,
};
enum mks_fgt_measure {
    MKS_FGT_M_NONE, MKS_FGT_M_OPEN, MKS_FGT_M_DRAW_SEALED, MKS_FGT_M_SWAP_SEALED,
};
enum mks_fgt_submit {
    MKS_FGT_SUB_UNSET, MKS_FGT_SUB_NOT_CALLED, MKS_FGT_SUB_OK, MKS_FGT_SUB_FAIL,
};
enum mks_fgt_present {
    MKS_FGT_PR_UNSET, MKS_FGT_PR_SWAP_OK, MKS_FGT_PR_SWAP_FAIL,
    MKS_FGT_PR_SKIPPED, MKS_FGT_PR_NO_SWAP, MKS_FGT_PR_UNSET_UNINIT,
};
enum mks_fgt_reclaim {
    MKS_FGT_RC_PENDING, MKS_FGT_RC_NORMAL, MKS_FGT_RC_HOL, MKS_FGT_RC_UNINIT,
};
enum mks_fgt_route { MKS_FGT_ROUTE_FUSED, MKS_FGT_ROUTE_SPLIT, MKS_FGT_ROUTE_INVALID };
enum mks_fgt_susp { MKS_FGT_SUSP_NONE, MKS_FGT_SUSP_ACQ, MKS_FGT_SUSP_REL };
enum mks_fgt_foreign {
    MKS_FGT_F_COMPARE = 1 << 0, MKS_FGT_F_ATLAS = 1 << 1,
    MKS_FGT_F_UNWRAPPED = 1 << 2, MKS_FGT_F_REINIT = 1 << 3,
    MKS_FGT_F_FOREIGN_Q = 1 << 4, MKS_FGT_F_NESTED = 1 << 5,
    MKS_FGT_F_WRONG_THREAD = 1 << 6, MKS_FGT_F_SCREENSHOT = 1 << 7,
};

struct mks_fgt_gl {
    void (*gen_queries)(int n, unsigned int *ids);
    void (*delete_queries)(int n, const unsigned int *ids);
    void (*begin_query)(unsigned int target, unsigned int name);
    void (*end_query)(unsigned int target);
    void (*get_integerv)(unsigned int pname, int *params);
    void (*get_query_object_uiv)(unsigned int name, unsigned int pname, unsigned int *params);
    void (*get_query_object_ui64v)(unsigned int name, unsigned int pname, unsigned long long *params);
    void *(*get_current_context)(void);
    void (*get_query_iv)(unsigned int target, unsigned int pname, int *params);
};
enum mks_fgt_slot { MKS_FGT_S_FREE, MKS_FGT_S_NORMAL, MKS_FGT_S_QUARANTINE, MKS_FGT_S_CTXLOST };
#ifdef __ANDROID__
extern struct mks_fgt_owner *mks_fgt_global;
static inline void mks_fgt_set_global(struct mks_fgt_owner *o) { mks_fgt_global = o; }
static inline struct mks_fgt_owner *mks_fgt_get_global(void) { return mks_fgt_global; }
#else
// Non-Android: no frame timing owner. Only accessors are needed;
// the real functions below are safe because enabled is always false.
__attribute__((unused)) static struct mks_fgt_owner *mks_fgt_global;
static inline void mks_fgt_set_global(struct mks_fgt_owner *o) { (void)o; }
static inline struct mks_fgt_owner *mks_fgt_get_global(void) { return NULL; }
#endif

struct mks_fgt_seg {
    int kind;
    int render_call;
    bool issued;
    int tstate;
    int64_t cpu_issue_ns;
    int slot;
    unsigned int query;
    uint64_t elapsed_ns;
};
struct mks_fgt_copy_rec {
    int c_seq;
    int t_seq;
    double pts;
    int src_w, src_h, lease_w, lease_h;
    bool target_copy;
    char timer[28];
    int seg_ref;
};
struct mks_fgt_ticket {
    int t_seq;
    double pts;
    int frame_id;
    int64_t issue_mono_ns;
    int measure, submit, present, reclaim;
    char present_reason[28];
    unsigned int foreign;
    int acquires, releases, copies_ok, reuse_served, render_calls;
    unsigned int render_ok_mask;
    int mix_frames, hw_frames, route;
    bool seed, acquire_fail, redraw, repeat, still, interpolated;
    bool draw_exit_pending;
    struct mks_fgt_seg segs[MKS_FGT_MAX_SEGS];
    int num_segs;
};
struct mks_fgt_quarantine {
    unsigned int query;
    int slot;
};
#define MKS_FGT_QUARANTINE_MAX 32
struct mks_fgt_owner {
    bool checked, initialized;
    bool enabled, armed, halted;
    int mode;
    struct mp_log *log;
    void *context;
    struct mks_fgt_gl fgt;
    char session[48];
    uint64_t nonce;
    int64_t init_mono_ns;
    unsigned int ids[MKS_FGT_SLOTS];
    int slot_state[MKS_FGT_SLOTS];
    struct mks_fgt_ticket tickets[MKS_FGT_TICKETS];
    int t_next;
    int active;
    int render_entry;
    int susp;
    bool carried;
    int active_seg;
    int active_slot;
    int emit_head;
    uint64_t bind, rel_result, rel_disjoint_normal, rel_late,
             rel_disjoint_quarantine, rel_uninit_normal, rel_uninit_quarantine,
             rel_uninit_ctxlost, leak_ctxlost, leak_quarantine;
    int busy_normal, busy_quarantine, busy_ctxlost;
    uint64_t gl_deleted, gl_leaked, uninit_end_count;
    uint64_t refused_ctx_lost, refused_no_ticket, refused_after_seal,
             refused_foreign_query, refused_ring_full, refused_copy_outside,
             refused_r2_span, refused_other;
    uint64_t unticketed_render, unticketed_acquire, unticketed_copy,
             unticketed_flip, unticketed_swap, draw_no_ticket, copy_unclosed;
    uint64_t copy_calls, pre_draw_fail, draw_submitted, draw_ok, draw_err;
    uint64_t late_discarded, poll_skipped_halted, hol_tickets, block_void;
    uint64_t overlay_uploads, mapper_reinit, latch_timeouts, reap_waits;
    uint64_t screenshots, invariant_violation, ticket_emitted, copy_emitted;
    char invariant[28];
    struct mks_fgt_copy_rec copies[MKS_FGT_MAX_COPIES];
    int copy_next;   /* ring slots used (bounded diagnostic snapshot) */
    int copy_count;  /* total copies ever recorded; C-line c_seq source */
    struct mks_fgt_quarantine quarantine[MKS_FGT_QUARANTINE_MAX];
    int quarantine_count;
    void (*emit)(void *ud, const char *line);
    void *emit_ud;
};
static inline void mks_fgt_line(struct mks_fgt_owner *o, const char *fmt, ...)
{
    if (!o->emit) return;
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    o->emit(o->emit_ud, line);
}
static inline const char *mks_fgt_mode_name(int mode)
{
    switch (mode) {
    case MKS_FGT_MODE_FRAME: return "frame";
    case MKS_FGT_MODE_SPAN: return "frame-span";
    case MKS_FGT_MODE_COUNT: return "frame-count";
    default: return "off";
    }
}
static inline const char *mks_fgt_kind_name(int kind)
{
    switch (kind) {
    case MKS_FGT_K_RENDER_PRE: return "render.pre";
    case MKS_FGT_K_COPY: return "copy";
    case MKS_FGT_K_RENDER_POST: return "render.post";
    case MKS_FGT_K_RENDER_TAIL: return "render.tail";
    case MKS_FGT_K_SPAN: return "span";
    case MKS_FGT_K_FINAL: return "final";
    default: return "unknown";
    }
}
static inline const char *mks_fgt_tstate_name(int ts)
{
    switch (ts) {
    case MKS_FGT_TS_ISSUED: return "issued";
    case MKS_FGT_TS_RESOLVED: return "resolved";
    case MKS_FGT_TS_DISJOINT: return "disjoint";
    case MKS_FGT_TS_PENDING_DROP: return "pending_drop";
    case MKS_FGT_TS_HOL_DROP: return "hol_drop";
    case MKS_FGT_TS_CTX_LOST: return "ctx_lost";
    case MKS_FGT_TS_NOT_TIMED: return "not_timed";
    default: return "none";
    }
}
static inline void mks_fgt_invariant(struct mks_fgt_owner *o, const char *src)
{
    if (!o->invariant[0]) { snprintf(o->invariant, sizeof(o->invariant), "%s", src); o->invariant_violation++; }
    o->enabled = false;
}
static inline bool mks_fgt_ctx_ok(struct mks_fgt_owner *o) { return o->fgt.get_current_context() == o->context; }
static inline bool mks_fgt_can_end(struct mks_fgt_owner *o, struct mks_fgt_seg *s)
{
    if (o->halted || s->slot < 0 || !s->issued) return false;
    if (!mks_fgt_ctx_ok(o)) return false;
    int current = 0;
    o->fgt.get_query_iv(MKS_FGT_ELAPSED, MKS_FGT_CURRENT_QUERY, &current);
    return current != 0 && (unsigned int)current == s->query;
}
static inline struct mks_fgt_ticket *mks_fgt_active(struct mks_fgt_owner *o)
{ return o->active >= 0 ? &o->tickets[o->active] : NULL; }
static inline void mks_fgt_halt(struct mks_fgt_owner *o)
{
    if (o->halted) return;
    for (int i = 0; i < MKS_FGT_TICKETS; i++) {
        struct mks_fgt_ticket *tk = &o->tickets[i];
        for (int s = 0; s < tk->num_segs; s++) {
            struct mks_fgt_seg *sg = &tk->segs[s];
            if (!sg->issued || sg->tstate == MKS_FGT_TS_CTX_LOST) continue;
            if (sg->tstate == MKS_FGT_TS_ISSUED) {
                sg->tstate = MKS_FGT_TS_CTX_LOST;
                if (sg->slot >= 0 && o->slot_state[sg->slot] == MKS_FGT_S_NORMAL) {
                    o->slot_state[sg->slot] = MKS_FGT_S_CTXLOST; o->busy_normal--; o->busy_ctxlost++;
                }
            }
        }
    }
    o->halted = true; o->active_seg = -1; mks_fgt_invariant(o, "ctx");
}
static inline void mks_fgt_disjoint_all(struct mks_fgt_owner *o)
{
    for (int i = 0; i < MKS_FGT_TICKETS; i++) {
        struct mks_fgt_ticket *tk = &o->tickets[i];
        for (int s = 0; s < tk->num_segs; s++) {
            struct mks_fgt_seg *sg = &tk->segs[s];
            if (!sg->issued) continue;
            if (sg->tstate != MKS_FGT_TS_ISSUED && sg->tstate != MKS_FGT_TS_HOL_DROP) continue;
            sg->tstate = MKS_FGT_TS_DISJOINT;
            if (sg->slot >= 0 && o->slot_state[sg->slot] == MKS_FGT_S_NORMAL) {
                o->slot_state[sg->slot] = MKS_FGT_S_FREE; o->rel_disjoint_normal++; o->busy_normal--;
            } else if (sg->slot >= 0 && o->slot_state[sg->slot] == MKS_FGT_S_QUARANTINE) {
                o->slot_state[sg->slot] = MKS_FGT_S_FREE; o->rel_disjoint_quarantine++; o->busy_quarantine--;
            }
        }
    }
}
static inline bool mks_fgt_ticket_settled(struct mks_fgt_ticket *tk)
{
    if (tk->present == MKS_FGT_PR_UNSET) return false;
    for (int i = 0; i < tk->num_segs; i++) {
        int ts = tk->segs[i].tstate;
        if (tk->segs[i].issued && ts != MKS_FGT_TS_RESOLVED && ts != MKS_FGT_TS_DISJOINT &&
            ts != MKS_FGT_TS_PENDING_DROP && ts != MKS_FGT_TS_HOL_DROP && ts != MKS_FGT_TS_CTX_LOST)
            return false;
    }
    return true;
}
static inline void mks_fgt_emit_ticket(struct mks_fgt_owner *o, struct mks_fgt_ticket *tk)
{
    mks_fgt_line(o, "MKS-FGPU-T session=%s t=%d pts=%.9f frame=%d measure=%d submit=%d present=%d reason=%s reclaim=%d foreign=%u acq=%d rel=%d copies_ok=%d reuse=%d renders=%d okmask=%u mix=%d hw=%d route=%d seed=%d afail=%d segs=%d",
        o->session, tk->t_seq, tk->pts, tk->frame_id, tk->measure, tk->submit, tk->present,
        tk->present_reason, tk->reclaim, tk->foreign, tk->acquires, tk->releases,
        tk->copies_ok, tk->reuse_served, tk->render_calls, tk->render_ok_mask,
        tk->mix_frames, tk->hw_frames, tk->route, tk->seed?1:0, tk->acquire_fail?1:0, tk->num_segs);
    for (int i = 0; i < tk->num_segs; i++) {
        struct mks_fgt_seg *s = &tk->segs[i];
        mks_fgt_line(o, "MKS-FGPU-G session=%s t=%d g=%d kind=%s call=%d issued=%d tstate=%s cpu=%"PRId64" elapsed=%"PRIu64,
            o->session, tk->t_seq, i, mks_fgt_kind_name(s->kind), s->render_call,
            s->issued?1:0, mks_fgt_tstate_name(s->tstate), s->cpu_issue_ns, s->elapsed_ns);
    }
        o->ticket_emitted++;
    // B3: recycle the slot so ticket_open can reuse it.
    tk->t_seq = -1;
}
static inline void mks_fgt_emit_ready(struct mks_fgt_owner *o)
{
    for (;;) {
        int emitted = -1;
        for (int i = 0; i < MKS_FGT_TICKETS; i++) {
            struct mks_fgt_ticket *tk = &o->tickets[i];
            if (tk->t_seq < 1 || tk->reclaim != MKS_FGT_RC_PENDING) continue;
            if (tk->t_seq != o->emit_head) continue;
            if (!mks_fgt_ticket_settled(tk)) break;
            tk->reclaim = MKS_FGT_RC_NORMAL; mks_fgt_emit_ticket(o, tk); o->emit_head++; emitted = i; break;
        }
        if (emitted < 0) break;
    }
    for (;;) {
        bool progressed = false;
        for (int i = 0; i < MKS_FGT_TICKETS; i++) {
            struct mks_fgt_ticket *tk = &o->tickets[i];
            if (tk->t_seq < 1 || tk->reclaim != MKS_FGT_RC_PENDING) continue;
            if (tk->present == MKS_FGT_PR_UNSET) continue;
            if (o->t_next - 1 - tk->t_seq <= MKS_FGT_HOL_AHEAD) continue;
            for (int s = 0; s < tk->num_segs; s++) {
                if (tk->segs[s].issued && tk->segs[s].tstate == MKS_FGT_TS_ISSUED) {
                    tk->segs[s].tstate = MKS_FGT_TS_HOL_DROP;
                    // Quarantine: move slot from normal to quarantine so poll
                    // can still harvest late results (which are discarded).
                    if (tk->segs[s].slot >= 0 && o->slot_state[tk->segs[s].slot] == MKS_FGT_S_NORMAL) {
                        o->slot_state[tk->segs[s].slot] = MKS_FGT_S_QUARANTINE;
                        o->busy_normal--; o->busy_quarantine++;
                    }
                }
            }
            int hol_seq = tk->t_seq;
            tk->reclaim = MKS_FGT_RC_HOL; o->hol_tickets++; mks_fgt_emit_ticket(o, tk);
            if (hol_seq >= o->emit_head) o->emit_head = hol_seq + 1;
            progressed = true;
        }
        if (!progressed) break;
    }
}
static inline void mks_fgt_poll(struct mks_fgt_owner *o)
{
    if (!o->enabled) return;
    if (o->halted) { o->poll_skipped_halted++; return; }
    if (!mks_fgt_ctx_ok(o)) { mks_fgt_halt(o); return; }
    int disjoint = 0;
    o->fgt.get_integerv(MKS_FGT_DISJOINT, &disjoint);
    if (disjoint) { mks_fgt_disjoint_all(o); mks_fgt_emit_ready(o); return; }
    for (int i = 0; i < MKS_FGT_TICKETS; i++) {
        struct mks_fgt_ticket *tk = &o->tickets[i];
        for (int s = 0; s < tk->num_segs; s++) {
            struct mks_fgt_seg *sg = &tk->segs[s];
            if (!sg->issued || (sg->tstate != MKS_FGT_TS_ISSUED && sg->tstate != MKS_FGT_TS_HOL_DROP)) continue;
            unsigned int ready = 0;
            o->fgt.get_query_object_uiv(sg->query, GL_QUERY_RESULT_AVAILABLE, &ready);
            if (!ready) continue;
            unsigned long long ns = 0;
            o->fgt.get_query_object_ui64v(sg->query, GL_QUERY_RESULT, &ns);
            int d2 = 0;
            o->fgt.get_integerv(MKS_FGT_DISJOINT, &d2);
            if (d2) { mks_fgt_disjoint_all(o); mks_fgt_emit_ready(o); return; }
            sg->tstate = MKS_FGT_TS_RESOLVED; sg->elapsed_ns = ns;
            if (sg->slot >= 0 && o->slot_state[sg->slot] == MKS_FGT_S_NORMAL) {
                o->slot_state[sg->slot] = MKS_FGT_S_FREE; o->rel_result++; o->busy_normal--;
            } else if (sg->slot >= 0 && o->slot_state[sg->slot] == MKS_FGT_S_QUARANTINE) {
                o->slot_state[sg->slot] = MKS_FGT_S_FREE; o->rel_late++; o->busy_quarantine--;
                o->late_discarded++; sg->elapsed_ns = 0;
            }
        }
    }
    mks_fgt_emit_ready(o);


    // Quarantine harvest: check standalone tracker for late-available slots.
    for (int q = o->quarantine_count - 1; q >= 0; q--) {
        unsigned int ready = 0;
        o->fgt.get_query_object_uiv(o->quarantine[q].query,
                                    GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue;
        o->slot_state[o->quarantine[q].slot] = MKS_FGT_S_FREE;
        o->rel_late++; o->busy_quarantine--; o->late_discarded++;
        o->quarantine[q] = o->quarantine[o->quarantine_count - 1];
        o->quarantine_count--;
    }}
static inline bool mks_fgt_begin_seg(struct mks_fgt_owner *o, int kind, int render_call)
{
    if (!o->enabled || !o->armed) { o->refused_other++; return false; }
    if (o->halted) { o->refused_ctx_lost++; return false; }
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) { o->refused_no_ticket++; return false; }
    if (tk->measure >= MKS_FGT_M_DRAW_SEALED && kind != MKS_FGT_K_FINAL) { o->refused_after_seal++; return false; }
    if (tk->measure == MKS_FGT_M_SWAP_SEALED) { o->refused_after_seal++; return false; }
    if (o->active_seg >= 0) { o->refused_other++; mks_fgt_invariant(o, "nested"); return false; }
    if (!mks_fgt_ctx_ok(o)) { mks_fgt_halt(o); o->refused_ctx_lost++; return false; }
    if (o->mode != MKS_FGT_MODE_COUNT) {
        int current = 0;
        o->fgt.get_query_iv(MKS_FGT_ELAPSED, MKS_FGT_CURRENT_QUERY, &current);
        if (current) { o->refused_foreign_query++; tk->foreign |= MKS_FGT_F_FOREIGN_Q; return false; }
    }
    if (tk->num_segs >= MKS_FGT_MAX_SEGS) { o->refused_ring_full++; return false; }
    struct mks_fgt_seg *s = &tk->segs[tk->num_segs];
    memset(s, 0, sizeof(*s));
    s->kind = kind; s->render_call = render_call; s->cpu_issue_ns = mp_time_ns(); s->slot = -1;
    if (o->mode == MKS_FGT_MODE_COUNT) { s->tstate = MKS_FGT_TS_NOT_TIMED; s->issued = false; }
    else {
        int slot = -1;
        for (int i = 0; i < MKS_FGT_SLOTS; i++) if (o->slot_state[i] == MKS_FGT_S_FREE) { slot = i; break; }
        if (slot < 0) { o->refused_ring_full++; return false; }
        o->fgt.begin_query(MKS_FGT_ELAPSED, o->ids[slot]);
        int started = 0;
        o->fgt.get_query_iv(MKS_FGT_ELAPSED, MKS_FGT_CURRENT_QUERY, &started);
        if ((unsigned int)started != o->ids[slot]) { o->refused_other++; return false; }
        s->slot = slot; s->issued = true; s->tstate = MKS_FGT_TS_ISSUED; s->query = o->ids[slot];
        o->slot_state[slot] = MKS_FGT_S_NORMAL; o->busy_normal++; o->bind++;
    }
    tk->num_segs++; o->active_seg = tk->num_segs - 1; o->active_slot = s->slot;
    return true;
}
static inline void mks_fgt_close_active(struct mks_fgt_owner *o)
{
    if (o->active_seg < 0) return;
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) { o->active_seg = -1; return; }
    struct mks_fgt_seg *s = &tk->segs[o->active_seg];
    if (o->mode == MKS_FGT_MODE_COUNT) { s->tstate = MKS_FGT_TS_NOT_TIMED; o->active_seg = -1; return; }
    if (mks_fgt_can_end(o, s)) { o->fgt.end_query(MKS_FGT_ELAPSED); o->active_seg = -1; }
    else { mks_fgt_halt(o); }
}
static inline bool mks_fgt_ticket_open(struct mks_fgt_owner *o, double pts, int frame_id, int mix_frames, int hw_frames, int route)
{
    if (!o->enabled || !o->armed) return false;
    if (o->halted) { o->refused_ctx_lost++; return false; }
    mks_fgt_poll(o);
    if (o->halted) { o->refused_ctx_lost++; return false; }
    for (int i = 0; i < MKS_FGT_TICKETS; i++) {
        struct mks_fgt_ticket *p = &o->tickets[i];
        if (p->t_seq >= 1 && p->t_seq == o->t_next - 1 && p->reclaim == MKS_FGT_RC_PENDING && p->present == MKS_FGT_PR_UNSET) {
            p->present = MKS_FGT_PR_NO_SWAP; snprintf(p->present_reason, sizeof(p->present_reason), "no-flip");
            mks_fgt_invariant(o, "no-flip");
        }
    }
    int idx = -1;
    for (int i = 0; i < MKS_FGT_TICKETS; i++) if (o->tickets[i].t_seq < 1) { idx = i; break; }
    if (idx < 0) { o->block_void++; return false; }
    struct mks_fgt_ticket *tk = &o->tickets[idx];
    memset(tk, 0, sizeof(*tk));
    tk->t_seq = o->t_next++; tk->pts = pts; tk->frame_id = frame_id;
    tk->measure = MKS_FGT_M_OPEN; tk->submit = MKS_FGT_SUB_UNSET;
    tk->present = MKS_FGT_PR_UNSET; tk->reclaim = MKS_FGT_RC_PENDING;
    tk->mix_frames = mix_frames; tk->hw_frames = hw_frames; tk->route = route;
    tk->issue_mono_ns = mp_time_ns(); o->active = idx;
    return true;
}
static inline void mks_fgt_draw_seal(struct mks_fgt_owner *o, bool frame_pending)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) { o->draw_no_ticket++; return; }
    if (tk->measure == MKS_FGT_M_OPEN) { mks_fgt_close_active(o); tk->measure = MKS_FGT_M_DRAW_SEALED; }
    tk->draw_exit_pending = frame_pending;
}
static inline void mks_fgt_render_begin(struct mks_fgt_owner *o, int k)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) { o->unticketed_render++; return; }
    if (o->susp != MKS_FGT_SUSP_NONE) { tk->foreign |= MKS_FGT_F_NESTED; }
    tk->render_calls++; tk->render_ok_mask <<= 1; o->render_entry = k;
    if (o->mode == MKS_FGT_MODE_SPAN) { if (o->active_seg >= 0) return; mks_fgt_begin_seg(o, MKS_FGT_K_SPAN, k); return; }
    if (o->active_seg >= 0) return;
    mks_fgt_begin_seg(o, k == 1 ? MKS_FGT_K_RENDER_PRE : MKS_FGT_K_RENDER_TAIL, k);
}
static inline void mks_fgt_render_end(struct mks_fgt_owner *o, bool ok)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) { o->render_entry = 0; return; }
    if (o->susp != MKS_FGT_SUSP_NONE) { tk->foreign |= MKS_FGT_F_NESTED; o->susp = MKS_FGT_SUSP_NONE; }
    if (ok) tk->render_ok_mask |= 1;
    if (o->active_seg >= 0) mks_fgt_close_active(o);
    o->render_entry = 0;
}
static inline void mks_fgt_suspend(struct mks_fgt_owner *o, int who)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (o->susp != MKS_FGT_SUSP_NONE) { if (tk) tk->foreign |= MKS_FGT_F_NESTED; return; }
    if (tk && o->render_entry != 0) { if (o->active_seg >= 0) mks_fgt_close_active(o); }
    else if (tk && o->active_seg >= 0) { tk->foreign |= MKS_FGT_F_UNWRAPPED; }
    if (tk) {
        if (who == MKS_FGT_SUSP_ACQ) tk->acquires++;
        else if (who == MKS_FGT_SUSP_REL) tk->releases++;
    }
    o->susp = who;
}
static inline void mks_fgt_resume(struct mks_fgt_owner *o, int who)
{
    if (o->susp != who) { struct mks_fgt_ticket *tk = mks_fgt_active(o); if (tk) tk->foreign |= MKS_FGT_F_NESTED; return; }
    o->susp = MKS_FGT_SUSP_NONE;
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) return;
    if (o->carried) { o->carried = false; return; }
    if (o->render_entry != 0 && o->active_seg >= 0) {
        struct mks_fgt_seg *s = &tk->segs[o->active_seg];
        if (s->kind == MKS_FGT_K_COPY) { o->copy_unclosed++; mks_fgt_close_active(o); }
    }
    if (o->render_entry != 0 && o->active_seg < 0) mks_fgt_begin_seg(o, MKS_FGT_K_RENDER_POST, o->render_entry);
}
static inline bool mks_fgt_copy_begin(struct mks_fgt_owner *o, int *seg_idx)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    o->copy_calls++; if (seg_idx) *seg_idx = -1;
    if (!tk) { o->unticketed_copy++; return false; }
    if (o->susp != MKS_FGT_SUSP_ACQ) { o->refused_copy_outside++; return false; }
    if (o->halted) { o->refused_ctx_lost++; return false; }
    if (o->mode == MKS_FGT_MODE_SPAN) {
        if (o->active_seg >= 0) return true;
        if (!mks_fgt_begin_seg(o, MKS_FGT_K_SPAN, o->render_entry)) return false;
        o->carried = true; if (seg_idx) *seg_idx = o->active_seg; return true;
    }
    if (!mks_fgt_begin_seg(o, MKS_FGT_K_COPY, o->render_entry)) return false;
    if (seg_idx) *seg_idx = o->active_seg; return true;
}
static inline void mks_fgt_copy_end(struct mks_fgt_owner *o)
{
    if (o->mode == MKS_FGT_MODE_SPAN) return;
    if (o->active_seg < 0) return;
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    struct mks_fgt_seg *s = &tk->segs[o->active_seg];
    if (s->kind != MKS_FGT_K_COPY) return;
    mks_fgt_close_active(o);
}
static inline void mks_fgt_final_begin(struct mks_fgt_owner *o) { mks_fgt_begin_seg(o, MKS_FGT_K_FINAL, 0); }
static inline void mks_fgt_final_end(struct mks_fgt_owner *o)
{
    if (o->active_seg < 0) return;
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    struct mks_fgt_seg *s = &tk->segs[o->active_seg];
    if (s->kind != MKS_FGT_K_FINAL) return;
    mks_fgt_close_active(o);
}
static inline void mks_fgt_swap_issue(struct mks_fgt_owner *o)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) { o->unticketed_swap++; return; }
    if (tk->measure < MKS_FGT_M_SWAP_SEALED) tk->measure = MKS_FGT_M_SWAP_SEALED;
}
static inline void mks_fgt_swap_result(struct mks_fgt_owner *o, bool ok, int skipped_delta, bool capture_foreign)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) return;
    if (tk->submit == MKS_FGT_SUB_UNSET) tk->submit = MKS_FGT_SUB_NOT_CALLED;
    if (tk->present != MKS_FGT_PR_UNSET) return;
    if (capture_foreign) { tk->foreign |= MKS_FGT_F_COMPARE; tk->present = MKS_FGT_PR_SKIPPED;
        snprintf(tk->present_reason, sizeof(tk->present_reason), "capture"); return; }
    if (skipped_delta > 0) { tk->present = MKS_FGT_PR_SKIPPED;
        snprintf(tk->present_reason, sizeof(tk->present_reason), "unfresh"); return; }
    tk->present = ok ? MKS_FGT_PR_SWAP_OK : MKS_FGT_PR_SWAP_FAIL;
}
static inline void mks_fgt_flip_close(struct mks_fgt_owner *o, bool entry_pending, bool guard, bool swap_ran)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) { if (swap_ran) o->unticketed_swap++; return; }
    if (tk->measure < MKS_FGT_M_SWAP_SEALED) tk->measure = MKS_FGT_M_SWAP_SEALED;
    if (tk->submit == MKS_FGT_SUB_UNSET) tk->submit = MKS_FGT_SUB_NOT_CALLED;
    if (tk->present != MKS_FGT_PR_UNSET) return;
    if (tk->submit == MKS_FGT_SUB_FAIL && !swap_ran) { tk->present = MKS_FGT_PR_NO_SWAP;
        snprintf(tk->present_reason, sizeof(tk->present_reason), "submit_fail"); return; }
    if (tk->submit == MKS_FGT_SUB_NOT_CALLED && !entry_pending && guard) {
        tk->present = MKS_FGT_PR_NO_SWAP; snprintf(tk->present_reason, sizeof(tk->present_reason), "guard_unpending"); return; }
    tk->present = MKS_FGT_PR_NO_SWAP; snprintf(tk->present_reason, sizeof(tk->present_reason), "unexpected");
    mks_fgt_invariant(o, "flip-path");
}
static inline void mks_fgt_draw_exit(struct mks_fgt_owner *o, bool frame_pending)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) return; mks_fgt_draw_seal(o, frame_pending);
}
static inline void mks_fgt_submit_result(struct mks_fgt_owner *o, bool ok)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (!tk) return;
    if (tk->submit == MKS_FGT_SUB_UNSET) tk->submit = ok ? MKS_FGT_SUB_OK : MKS_FGT_SUB_FAIL;
}
static inline void mks_fgt_bypass(struct mks_fgt_owner *o, int foreign_bit, const char *src, bool ticket_foreign)
{
    if (foreign_bit == MKS_FGT_F_SCREENSHOT) o->screenshots++;
    mks_fgt_invariant(o, src);
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (tk && ticket_foreign) tk->foreign |= foreign_bit;
}
static inline void mks_fgt_reinit_notice(struct mks_fgt_owner *o, bool in_ticket)
{
    o->mapper_reinit++;
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    if (tk && in_ticket) tk->foreign |= MKS_FGT_F_REINIT;
}
static inline void mks_fgt_overlay_upload(struct mks_fgt_owner *o) { o->overlay_uploads++; }
static inline void mks_fgt_copy_record(struct mks_fgt_owner *o, double pts, int sw, int sh, int lw, int lh, bool target_copy, const char *timer, int seg_idx, bool published)
{
    struct mks_fgt_ticket *tk = mks_fgt_active(o);
    int c_seq = ++o->copy_count;
    int t_seq = tk ? tk->t_seq : 0;
    /* Ring is a bounded diagnostic snapshot; accounting and the C line are
     * unconditional so long rounds keep per-ticket copies_ok and reconcilable
     * C-line counts after the ring fills. */
    if (o->copy_next < MKS_FGT_MAX_COPIES) {
        struct mks_fgt_copy_rec *c = &o->copies[o->copy_next++];
        memset(c, 0, sizeof(*c));
        c->c_seq = c_seq; c->t_seq = t_seq; c->pts = pts;
        c->src_w = sw; c->src_h = sh; c->lease_w = lw; c->lease_h = lh;
        c->target_copy = target_copy; snprintf(c->timer, sizeof(c->timer), "%s", timer); c->seg_ref = seg_idx;
    }
    if (tk && seg_idx >= 0 && published) tk->copies_ok++;
    mks_fgt_line(o, "MKS-FGPU-C session=%s c=%d t=%d pts=%.9f src=%dx%d lease=%dx%d tc=%d timer=%s seg=%d",
        o->session, c_seq, t_seq, pts, sw, sh, lw, lh, target_copy?1:0, timer, seg_idx);
    o->copy_emitted++;
}
static inline void mks_fgt_summary(struct mks_fgt_owner *o)
{
    mks_fgt_line(o, "MKS-FGPU-S session=%s through_t=%d through_c=%d tickets=%"PRIu64" copies=%"PRIu64" mode=%s armed=%d halted=%d bind=%"PRIu64" rel_result=%"PRIu64" rel_dn=%"PRIu64" rel_late=%"PRIu64" rel_dq=%"PRIu64" rel_un=%"PRIu64" rel_uq=%"PRIu64" rel_uc=%"PRIu64" leak_c=%"PRIu64" leak_q=%"PRIu64" busy_n=%d busy_q=%d busy_c=%d gl_del=%"PRIu64" gl_leak=%"PRIu64" invariant=%"PRIu64,
        o->session, o->emit_head, o->copy_count, o->ticket_emitted, o->copy_emitted,
        mks_fgt_mode_name(o->mode), o->armed?1:0, o->halted?1:0,
        o->bind, o->rel_result, o->rel_disjoint_normal, o->rel_late, o->rel_disjoint_quarantine,
        o->rel_uninit_normal, o->rel_uninit_quarantine, o->rel_uninit_ctxlost,
        o->leak_ctxlost, o->leak_quarantine, o->busy_normal, o->busy_quarantine, o->busy_ctxlost,
        o->gl_deleted, o->gl_leaked, o->invariant_violation);
}
static inline void mks_fgt_init(struct mks_fgt_owner *o, struct mp_log *log, const char *mode_str,
                                void (*emit)(void *, const char *), void *emit_ud, void *(*get_context)(void))
{
    if (o->checked) return;
    o->checked = true; o->initialized = false;
    o->nonce = (uint64_t)(uintptr_t)o; o->init_mono_ns = mp_time_ns();
    snprintf(o->session, sizeof(o->session), "%ld:%"PRIu64":%"PRId64, (long)getpid(), o->nonce, o->init_mono_ns);
    if (emit) { o->emit = emit; o->emit_ud = emit_ud; }
    // If emit is NULL, preserve any existing callback (set before lazy init) o->fgt.get_current_context = get_context;
    o->active = -1; o->active_seg = -1; o->active_slot = -1; o->emit_head = 1; o->t_next = 1;
    for (int i = 0; i < MKS_FGT_TICKETS; i++) o->tickets[i].t_seq = -1;
    if (!strcmp(mode_str, "frame")) o->mode = MKS_FGT_MODE_FRAME;
    else if (!strcmp(mode_str, "frame-span")) o->mode = MKS_FGT_MODE_SPAN;
    else if (!strcmp(mode_str, "frame-count")) o->mode = MKS_FGT_MODE_COUNT;
    else o->mode = MKS_FGT_MODE_OFF;
    if (o->mode == MKS_FGT_MODE_OFF) return;
    o->log = log; o->context = get_context();
    o->enabled = o->fgt.gen_queries && o->fgt.delete_queries && o->fgt.begin_query &&
                 o->fgt.end_query && o->fgt.get_integerv && o->fgt.get_query_object_uiv &&
                 o->fgt.get_query_object_ui64v && o->fgt.get_current_context && o->context != NULL &&
                 (o->mode == MKS_FGT_MODE_COUNT || o->fgt.get_query_iv);
    if (!o->enabled) { o->armed = false; return; }
    o->armed = true;
    if (o->mode != MKS_FGT_MODE_COUNT) {
        o->fgt.gen_queries(MKS_FGT_SLOTS, o->ids);
        for (int i = 0; i < MKS_FGT_SLOTS; i++) if (!o->ids[i]) { o->enabled = false; o->armed = false; }
        if (!o->enabled) memset(o->ids, 0, sizeof(o->ids));
    }
    o->initialized = true;
    mks_fgt_line(o, "MKS-FGPU-INIT session=%s mode=%s armed=%d reason=%s prop=%s mono_ns=%"PRId64,
        o->session, mks_fgt_mode_name(o->mode), o->armed?1:0, o->armed?"ok":"s1-pending", mode_str, o->init_mono_ns);
}
static inline void mks_fgt_uninit(struct mks_fgt_owner *o)
{
    if (mks_fgt_get_global() == o) mks_fgt_set_global(NULL);
    if (!o->checked || !o->initialized) return;
    bool ctx = mks_fgt_ctx_ok(o);
    if (!ctx) mks_fgt_halt(o);
    else if (o->mode == MKS_FGT_MODE_COUNT) {
        // count mode: no GL query calls needed at uninit
        o->active_seg = -1;
    } else {
        mks_fgt_poll(o);
        int current = 0;
        o->fgt.get_query_iv(MKS_FGT_ELAPSED, MKS_FGT_CURRENT_QUERY, &current);
        if (current) { bool ours = false;
            for (int i = 0; i < MKS_FGT_SLOTS; i++) if (o->ids[i] == (unsigned int)current) { ours = true; break; }
            if (ours) { o->fgt.end_query(MKS_FGT_ELAPSED); o->uninit_end_count++; } }
        o->active_seg = -1;
    }
    for (int i = 0; i < MKS_FGT_TICKETS; i++) {
        struct mks_fgt_ticket *tk = &o->tickets[i];
        if (tk->t_seq < 1) continue;
        bool was_pending = tk->reclaim == MKS_FGT_RC_PENDING;
        for (int s = 0; s < tk->num_segs; s++) {
            struct mks_fgt_seg *sg = &tk->segs[s];
            if (sg->issued && sg->tstate == MKS_FGT_TS_ISSUED)
                sg->tstate = ctx ? MKS_FGT_TS_PENDING_DROP : MKS_FGT_TS_CTX_LOST;
        }
        if (was_pending) {
            if (tk->present == MKS_FGT_PR_UNSET) tk->present = MKS_FGT_PR_UNSET_UNINIT;
            tk->reclaim = MKS_FGT_RC_UNINIT; mks_fgt_emit_ticket(o, tk);
        }
    }
    if (o->mode == MKS_FGT_MODE_COUNT) {
        // count mode: no GL queries were generated, skip delete and accounting.
        o->busy_normal = o->busy_quarantine = o->busy_ctxlost = 0;
    } else if (ctx) {
        o->rel_uninit_normal += o->busy_normal; o->rel_uninit_quarantine += o->busy_quarantine;
        o->rel_uninit_ctxlost += o->busy_ctxlost;
        o->busy_normal = o->busy_quarantine = o->busy_ctxlost = 0;
        o->fgt.delete_queries(MKS_FGT_SLOTS, o->ids); o->gl_deleted = MKS_FGT_SLOTS;
    } else {
        o->leak_ctxlost += o->busy_ctxlost; o->leak_quarantine += o->busy_quarantine;
        o->gl_leaked = MKS_FGT_SLOTS;
    }
    mks_fgt_summary(o);
    mks_fgt_line(o, "MKS-FGPU-FINAL session=%s complete=%d incomplete=%s",
        o->session, ctx && !o->halted ? 1 : 0, o->halted ? "ctx" : (ctx ? "none" : "ctx-uninit"));
    o->initialized = false;
    mks_fgt_set_global(NULL);
}

// Lazy init guard: call this before any enabled/armed check. On the first
// call, reads the mode property and initializes (by which time the EGL
// context is current because the caller is already doing GL work).
#ifdef __ANDROID__
static inline bool mks_fgt_ready(struct mks_fgt_owner *o)
{
    if (!o->checked) {
        o->fgt.gen_queries = (void *)eglGetProcAddress("glGenQueries");
        o->fgt.delete_queries = (void *)eglGetProcAddress("glDeleteQueries");
        o->fgt.begin_query = (void *)eglGetProcAddress("glBeginQuery");
        o->fgt.end_query = (void *)eglGetProcAddress("glEndQuery");
        o->fgt.get_integerv = (void *)eglGetProcAddress("glGetIntegerv");
        o->fgt.get_query_iv = (void *)eglGetProcAddress("glGetQueryivEXT");
        o->fgt.get_query_object_uiv = (void *)eglGetProcAddress("glGetQueryObjectuivEXT");
        o->fgt.get_query_object_ui64v = (void *)eglGetProcAddress("glGetQueryObjectui64vEXT");
        o->fgt.get_current_context = (void *)eglGetCurrentContext;
        char mode[PROP_VALUE_MAX + 1] = {0};
        __system_property_get("debug.mks.gpu_timing", mode);
        mks_fgt_init(o, NULL, mode, NULL, NULL, eglGetCurrentContext);
    }
    return o->enabled && o->armed;
}
#else
static inline bool mks_fgt_ready(struct mks_fgt_owner *o)
{
    return o->enabled && o->armed;
}
#endif
#endif
