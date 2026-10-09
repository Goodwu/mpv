/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <string.h>

#include "video/out/android_common.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "common/msg.h"
#include "context.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// MKS YUV compile-time gate matrix (one block; every gate is
// #ifndef-overridable for lab builds). Feature gates carry accepted,
// user-visible behavior and default ON; diagnostic gates are evidence
// instrumentation and default OFF. With a diagnostic gate at 0 the compiled
// object must stay byte-identical to a build with that code physically
// deleted (verification: compile both with the exact product command and
// compare the object sha256), so every diagnostic statement must sit inside
// its gate with zero ungated state or calls.
//
//   gate                                  default  class       controls
//   MKS_YUV_HOOK                          1        feature     window vtable hook install (android_init, diag-armed) / restore (android_uninit or window replacement) + queue/dequeue wrappers (pure passthrough on their own)
//   MKS_YUV_P1_INJECT                     1        feature     queue-time color metadata injection (rides the hook wrappers, hence requires MKS_YUV_HOOK)
//   MKS_YUV_P0_OBSERVE                    0        diagnostic  buffer-head geometry notes + dedup table, first-5 swap begin/return logs, first-10 draw GetError checks, first-draw log
//   MKS_YUV_DIAG_MODE_BT2020_NCL          1        feature     final-pass shader mode (#ifndef-overridable, defined with the shader sources below)
//   MKS_YUV_DIAG_MODE_NEUTRAL_PASSTHROUGH 0        feature     fallback shader mode of the same switch (#ifndef-overridable)
//   MKS_ST_DIAG_READBACK                  0        diagnostic  (hwdec_surfacetexture.c only) per-frame lease readback evidence
// ---------------------------------------------------------------------------
#ifndef MKS_YUV_HOOK
#define MKS_YUV_HOOK 1
#endif
#ifndef MKS_YUV_P1_INJECT
#define MKS_YUV_P1_INJECT 1
#endif
#ifndef MKS_YUV_P0_OBSERVE
#define MKS_YUV_P0_OBSERVE 0
#endif
// The P1 injection rides the queue wrapper; there is no injection without it.
#if MKS_YUV_P1_INJECT && !MKS_YUV_HOOK
#error "MKS_YUV_P1_INJECT=1 requires MKS_YUV_HOOK=1 (the injection rides the queue wrapper)"
#endif
//
// setMetaData failure policy (P1 queue path; every line MP_ERR, no
// addresses, and a failure NEVER blocks the queue — the queued frame is
// unaffected). Narrowed claim (V2 review memo): a failed setMetaData only
// means THIS buffer received no fresh injection; its gralloc metadata page
// may still carry metadata from an earlier write (the page persists across
// EGL buffer reuse), so nothing is claimed either way about the buffer's
// effective HDR recognition:
//   1. setMetaData dlsym miss at arm     -> injection off for the arm, one MP_ERR.
//   2. buffer head magic mismatch        -> injection disarmed permanently
//                                           (fail-closed), one MP_ERR.
//   3. setMetaData rc != 0               -> first failure logs, then every
//                                           32nd failure logs with the
//                                           cumulative count.
//   4. success after a failure           -> one recovery log.
// These error-path lines are feature-gate code (MKS_YUV_HOOK /
// MKS_YUV_P1_INJECT), not diagnostic instrumentation: each is emitted only
// when the failure actually happens, so with every diagnostic gate at 0 they
// are silent and the off identity (byte-identical object vs the
// physically-deleted diagnostic build) is unaffected.
//
// Swap-path error policy (ungated; runs on every swap of this context):
//   1. eglSwapBuffers returning EGL_FALSE -> first failure logs, then every
//      32nd (same cadence as the setMetaData policy above), cumulative
//      count, no addresses; the swap path itself never changes (the
//      ra_swapchain swap_buffers contract is void).
//   2. an armed branch reaching the swap with the final pass ready=false ->
//      the swap still runs (a skipped swap stalls the presentation pipeline)
//      and the state logs with the same cadence.
// Like the policy above, these lines only emit when the failure or abnormal
// state actually happens, so the off identity is unaffected.

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    // Cumulative eglSwapBuffers failures (see the swap-path error policy in
    // the gate matrix at the top of this file); drives the log cadence only.
    int swap_failures;
    // MKS YUV diag (lab-gated, default off; see the YUV-DIAG block below).
    // All GL objects live in this per-context priv (never file statics), so
    // two contexts — e.g. an un-armed follow-up context after a teardown —
    // each own exactly their own objects: a priv with ready=false runs no
    // final pass and hands no FBO to the libplacebo swapchain.
    bool yuv_diag;
    struct {
        // ready=true only while THIS context has a complete offscreen
        // attachment AND a linked program. A resize whose FBO completeness
        // check fails latches ready=false with w/h zeroed (conservative
        // invalidation; the rolled-back texture name STAYS in tex — see
        // yuv_diag_ensure_offscreen, R3 fix): no final pass runs for this or
        // any later swap until the next successful resize re-arms it.
        // ready=false never means "partially usable" — the final pass must
        // not touch a dead attachment even if the names still look valid.
        bool ready;      // offscreen + program usable for THIS context
        int fbo;         // stable FBO name handed to the libplacebo swapchain
        int w, h;
        GLuint tex;
        GLuint program;
        GLint tex_loc;
        // glIsEnabled resolved once for the final-pass state save/restore
        // (mpv's GL struct has no IsEnabled entry point). NULL = skip the
        // enable-bit save/restore (logged once at setup, never silent); the
        // queryable state is still restored.
        unsigned char (GLAPIENTRY *is_enabled)(GLenum);
        // Swaps committed while the branch is armed with ready=false (see the
        // swap-path error policy in the gate matrix at the top of this file).
        int notready_swaps;
#if MKS_YUV_P0_OBSERVE
        // b: bounded first-frame observation (no queueBuffer hook, no
        // glFinish): the first final-pass draw begin/result + GL error, the
        // first N swap begin/return + EGL error, and the running submit
        // count. Diagnostic-only state (MKS_YUV_P0_OBSERVE); counters live
        // in this per-context struct.
        int swap_count;       // total swap commits since context creation
        int draw_err_logged;  // draws whose GL error was checked (bounded)
        bool first_draw_logged;
#endif
    } yuv;
};

// ---------------------------------------------------------------------------
// MKS YUV diag (phase3-decision, lab-gated, default off).
//
// An 8-bit NV12 output diagnostic branch for the LG-H870DS display-chain
// question: the libplacebo render chain is kept fully intact, but decoupled
// from the window — it renders into a high-precision RGBA16F offscreen FBO
// (the pl_opengl swapchain is pointed at it via mks_yuv_diag_swapchain_fbo),
// and a final pass in android_swap_buffers copies that offscreen into the YUV
// default framebuffer with a GL_EXT_YUV_target `layout(yuv)` output.
//
// Selected mechanism (of the two options in the task card): (b) — the
// libplacebo swapchain is re-bound to a self-built RGB offscreen FBO at
// creation time (gpu_next/context.c), and the final pass runs at swap time.
// This is the least invasive option: vo_gpu_next.c needs zero changes, the
// whole mpv/libplacebo processing chain (target-trc/target-prim overrides,
// tone mapping, dither) behaves exactly as on the RGB path, and the only
// difference is the target FBO identity plus one extra pass before
// eglSwapBuffers. Verification method: Gate-1b (context/surface/swap without
// GL errors, buffer actually YUV) and Gate-2 (pixel math: the shader packs
// 16+219*p exactly once; the offscreen must carry the expected PQ values).
//
// This diagnostic round deliberately does NOT do a BT.2020 color matrix:
// the shader is the neutral-gray passthrough mode (p = offscreen R channel,
// Cb = Cr = 0.5) to isolate the CSC ambiguity (decision B2).
//
// Fail-closed: a YUV context/surface creation failure, a missing
// GL_EXT_YUV_target, an incomplete offscreen FBO or a failed program
// link all fail the whole vo open through the existing error path. There
// is no fallback to any other config.
//
// Runtime switch: the vendor plugin export mkvendor_yuv_diag_enabled()
// (same dlsym pattern as E1; missing symbol = off). With the switch off,
// every code path below is inert and behavior is bit-identical to before.
// All log lines use the YUV-DIAG prefix and never contain addresses.
//
// Offscreen/window range contract (V2 P1-1): the diag define pairing must be
// CONVERT_SURFACE_TRANSFER=pq + MPV_OUTPUT_LEVELS=full, so gpu-next writes
// FULL-range normalized PQ into the RGBA16F offscreen while the config49
// NV12 window surface is LIMITED; the final pass performs the single
// 16+219p limited packing. Any other pairing double-compresses (offscreen
// limited x shader limited => black/white ends at 30/217 instead of 16/235);
// the lab hard-rejects it before arming.
// ---------------------------------------------------------------------------

// Runtime switch: resolves the vendor export once per process (E1 pattern:
// RTLD_DEFAULT first, then the vendor plugin sonames; a failed lookup stays a
// permanent silent no-op with one MP_DBG line). Missing symbol = off.
static bool yuv_diag_enabled(struct ra_ctx *ctx)
{
    typedef int (*mk_yuv_diag_fn)(void);
    static mk_yuv_diag_fn enabled_fn;
    static bool resolved;
    if (!resolved) {
        resolved = true;
        enabled_fn = (mk_yuv_diag_fn)dlsym(RTLD_DEFAULT,
                                           "mkvendor_yuv_diag_enabled");
        if (!enabled_fn) {
            void *lib = dlopen("libmedia_kit_android_dataspace_vendor.so",
                               RTLD_NOW);
            if (!lib)
                lib = dlopen("libmedia_kit_dataspace_vendor.so", RTLD_NOW);
            if (lib)
                enabled_fn = (mk_yuv_diag_fn)dlsym(
                    lib, "mkvendor_yuv_diag_enabled");
        }
        if (!enabled_fn)
            MP_DBG(ctx, "YUV-DIAG: vendor switch unavailable; off\n");
    }
    return enabled_fn && enabled_fn() != 0;
}

// Final-pass shader sources (ESSL3). The fragment shader requires
// GL_EXT_YUV_target: `layout(yuv) out` writes normalized values that the
// driver quantizes to the surface's 8-bit NV12 storage with NO automatic
// range/gamma or color-matrix handling, so the FULL single limited-range
// packing (matrix + 16..235/128..240 mapping) is done here, in the shader,
// exactly once.
//
// Mode switch (exactly one must be 1; #ifndef-overridable for lab builds,
// same as every gate in the matrix at the top of this file):
// - MKS_YUV_DIAG_MODE_BT2020_NCL (current, color round): BT.2020
//   non-constant-luminance YCbCr from the offscreen's FULL-range normalized
//   PQ-encoded RGB. NCL = the matrix runs in the encoded (PQ) domain, no
//   linearization — matching BT.2020's table for PQ systems.
// - MKS_YUV_DIAG_MODE_NEUTRAL_PASSTHROUGH (diagnostic fallback, decision B2
//   round): neutral-gray single-channel passthrough (p -> 16+219p,
//   Cb=Cr=128); kept for diagnostic retreat.
#ifndef MKS_YUV_DIAG_MODE_BT2020_NCL
#define MKS_YUV_DIAG_MODE_BT2020_NCL 1
#endif
#ifndef MKS_YUV_DIAG_MODE_NEUTRAL_PASSTHROUGH
#define MKS_YUV_DIAG_MODE_NEUTRAL_PASSTHROUGH 0
#endif
static const char yuv_diag_vs[] =
    "#version 300 es\n"
    "// Fullscreen triangle from gl_VertexID; no VBO/attribs needed.\n"
    "void main() {\n"
    "    float x = float((gl_VertexID & 1) << 2) - 1.0;\n"
    "    float y = float((gl_VertexID & 2) << 1) - 1.0;\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "}\n";
static const char yuv_diag_fs[] =
    "#version 300 es\n"
    "#extension GL_EXT_YUV_target : require\n"
    "precision highp float;\n"
    "uniform highp sampler2D mksSrc;\n"
    "layout(yuv) out highp vec3 mksColor;\n"
    "void main() {\n"
    "    // Offscreen contract: FULL-range normalized PQ-encoded RGB.\n"
    "    vec3 rgb = texelFetch(mksSrc, ivec2(gl_FragCoord.xy), 0).rgb;\n"
#if MKS_YUV_DIAG_MODE_BT2020_NCL
    "    // BT.2020 NCL, ITU-R BT.2020 Table 4:\n"
    "    //   Kr = 0.2627, Kb = 0.0593, Kg = 1 - Kr - Kb = 0.6780.\n"
    "    // NCL: the matrix runs in the encoded (PQ) domain, no EOTF. With\n"
    "    // Cb/Cr scaled to +-0.5:\n"
    "    //   Y'  = Kr*R + Kg*G + Kb*B\n"
    "    //   Cb  = (B - Y') * 0.5 / (1 - Kb)\n"
    "    //   Cr  = (R - Y') * 0.5 / (1 - Kr)\n"
    "    // FULL-range BT.2020 NCL YCbCr written directly (no limited\n"
    "    // packing). layout(yuv) writes normalized values; the driver only\n"
    "    // quantizes to the NV12 planes.\n"
    "    float y = 0.2627 * rgb.r + 0.6780 * rgb.g + 0.0593 * rgb.b;\n"
    "    float cb = (rgb.b - y) * 0.5 / (1.0 - 0.0593);\n"
    "    float cr = (rgb.r - y) * 0.5 / (1.0 - 0.2627);\n"
    "    // FULL-range BT.2020 NCL YCbCr: metadata range=FULL(1) tells the\n"
    "    // display to decode full-range PQ directly, no 16-235 expansion.\n"
    "    mksColor = vec3(y, cb + 0.5, cr + 0.5);\n"
#elif MKS_YUV_DIAG_MODE_NEUTRAL_PASSTHROUGH
    "    // Neutral-gray passthrough round (diagnostic fallback, decision\n"
    "    // B2): single limited Y packing, Cb=Cr=128. For neutral input the\n"
    "    // BT.2020 NCL branch above degenerates to exactly this: R=G=B=p\n"
    "    // gives y=p, cb=cr=0.\n"
    "    float p = texelFetch(mksSrc, ivec2(gl_FragCoord.xy), 0).r;\n"
    "    mksColor = vec3(16.0 / 255.0 + (219.0 / 255.0) * p, 0.5, 0.5);\n"
#else
    "#error \"YUV-DIAG: no final-pass mode selected \"\n"
#endif
    "}\n";

static void yuv_diag_log_shader_log(struct ra_ctx *ctx, const char *what,
                                    GLuint obj,
                                    void (GLAPIENTRY *get_log)(GLuint, GLsizei,
                                                               GLsizei *, GLchar *))
{
    char log[512] = {0};
    GLint len = 0;
    get_log(obj, sizeof(log) - 1, &len, log);
    MP_MSG(ctx, MSGL_FATAL, "YUV-DIAG: %s failed; log: %s\n", what, log);
}

// Create/resize the offscreen RGB target. The FBO name is kept stable across
// resizes (only the attached texture is recreated) so the libplacebo
// swapchain, which wrapped it at creation time, never sees a dangling FBO.
//
// Resize transaction consistency (V3 review fix): the OLD texture stays
// alive until the NEW attachment proves complete, so a failed resize rolls
// the FBO back to the old texture instead of leaving the stable FBO with a
// dead attachment while ready stays true (the A -> B-fail -> A
// counterexample: stale w/h would then early-return the recovery resize as
// "already configured" and the final pass would keep running against a
// deleted texture). On a completeness failure the branch is conservatively
// invalidated (ready=false, w/h zeroed) until the next successful resize;
// the rolled-back old texture stays attached for libplacebo's renders and
// its GL name STAYS in p->yuv.tex (R3 fix: never orphaned) so the recovery
// resize's success path or uninit deletes it exactly once.
static bool yuv_diag_ensure_offscreen(struct ra_ctx *ctx, int w, int h)
{
    struct priv *p = ctx->priv;
    struct GL *gl = &p->gl;
    if (w <= 1 || h <= 1) {
        // (a) fail-closed: a 1x1 (or non-positive) size is a placeholder (it
        // appears after a failed configure — possibly a RESULT of that
        // failure rather than its cause; the c1/c2 fixes address the causes).
        // Do not create the offscreen on a placeholder and do not continue
        // this round; no global size hardcoding, resize lifecycle untouched.
        MP_MSG(ctx, MSGL_FATAL, "YUV-DIAG: placeholder size (%dx%d); not "
               "creating the offscreen, terminating (fail-closed)\n", w, h);
        return false;
    }
    if (p->yuv.fbo && p->yuv.w == w && p->yuv.h == h)
        return true;

    // Keep the previous texture alive until the new attachment is complete.
    GLuint old_tex = p->yuv.tex;

    if (!p->yuv.fbo) {
        GLuint fbo = 0;
        gl->GenFramebuffers(1, &fbo);
        p->yuv.fbo = (int)fbo;
    }

    GLuint tex = 0;
    gl->GenTextures(1, &tex);
    gl->BindTexture(GL_TEXTURE_2D, tex);
    // RGBA16F: the probed renderable float format on this device (existing
    // rgba16f probe evidence). Holds the FULL-range normalized PQ intermediates
    // losslessly (see the offscreen/window range contract above).
    gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA,
                   GL_HALF_FLOAT, NULL);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    gl->BindFramebuffer(GL_FRAMEBUFFER, (GLuint)p->yuv.fbo);
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex, 0);
    GLenum status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        MP_MSG(ctx, MSGL_FATAL, "YUV-DIAG: offscreen FBO incomplete "
               "(status=0x%x, size %dx%d)\n", status, w, h);
        // Rollback: the new texture is gone and the old texture (if this
        // resize had a live one to fall back to) goes back on the stable FBO
        // so the swapchain-wrapped name keeps a complete attachment for
        // libplacebo's renders.
        gl->DeleteTextures(1, &tex);
        if (old_tex) {
            gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, old_tex, 0);
        }
        gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
        // Conservative invalidation: this and every later swap run no final
        // pass until the next SUCCESSFUL resize. Zeroing w/h also keeps the
        // same-size early return above from short-circuiting the recovery
        // resize with stale geometry.
        // R3 fix: the rolled-back texture name STAYS in p->yuv.tex (only
        // w/h/ready are invalidated). It is still attached to the stable FBO
        // above; keeping the name is what lets the recovery resize's success
        // path and uninit delete it exactly once. Zeroing it here would
        // orphan the GL name and leak it for the context lifetime.
        p->yuv.tex = old_tex;
        p->yuv.w = 0;
        p->yuv.h = 0;
        p->yuv.ready = false;
        return false;
    }
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);

    if (old_tex)
        gl->DeleteTextures(1, &old_tex);
    p->yuv.tex = tex;
    p->yuv.w = w;
    p->yuv.h = h;
    // Re-arm on success: the program is created once at setup and only
    // destroyed at uninit, so a complete offscreen plus an existing program
    // is exactly the ready=true contract. During setup (program not yet
    // linked) this stays false; yuv_diag_setup sets it after the program
    // check.
    p->yuv.ready = p->yuv.program != 0;
    MP_ERR(ctx, "YUV-DIAG: offscreen RGB FBO ready (%dx%d, rgba16f)\n", w, h);
    return true;
}

// Compile/link the final pass. Any failure is a hard open failure.
static bool yuv_diag_ensure_program(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct GL *gl = &p->gl;
    if (p->yuv.program)
        return true;

    GLuint vs = gl->CreateShader(GL_VERTEX_SHADER);
    const char *vs_src = yuv_diag_vs;
    gl->ShaderSource(vs, 1, &vs_src, NULL);
    gl->CompileShader(vs);
    GLint ok = 0;
    gl->GetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        yuv_diag_log_shader_log(ctx, "YUV-DIAG vertex shader compile", vs,
                                gl->GetShaderInfoLog);
        gl->DeleteShader(vs);
        return false;
    }

    GLuint fs = gl->CreateShader(GL_FRAGMENT_SHADER);
    const char *fs_src = yuv_diag_fs;
    gl->ShaderSource(fs, 1, &fs_src, NULL);
    gl->CompileShader(fs);
    ok = 0;
    gl->GetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        yuv_diag_log_shader_log(ctx, "YUV-DIAG fragment shader compile", fs,
                                gl->GetShaderInfoLog);
        gl->DeleteShader(vs);
        gl->DeleteShader(fs);
        return false;
    }

    GLuint prog = gl->CreateProgram();
    gl->AttachShader(prog, vs);
    gl->AttachShader(prog, fs);
    gl->LinkProgram(prog);
    gl->DeleteShader(vs);
    gl->DeleteShader(fs);
    ok = 0;
    gl->GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        yuv_diag_log_shader_log(ctx, "YUV-DIAG program link", prog,
                                gl->GetProgramInfoLog);
        gl->DeleteProgram(prog);
        return false;
    }

    p->yuv.program = prog;
    p->yuv.tex_loc = gl->GetUniformLocation(prog, "mksSrc");
    MP_ERR(ctx, "YUV-DIAG: final pass program ready\n");
    return true;
}

// Final pass: copy the high-precision RGB offscreen into the YUV default
// framebuffer (the drawable) with the layout(yuv) shader output. Runs with
// the mpv EGL context current, immediately before eglSwapBuffers. The texel
// 1:1 copy (both targets are GL framebuffers with the same origin convention,
// and the offscreen was wrapped with the same `flipped` flag the default
// framebuffer would get) preserves exactly the orientation libplacebo
// intended for the window; Gate-2 pixel checks can rely on it.
//
// All GL state the pass touches is saved before and restored after (the read
// and draw framebuffer bindings each saved and restored separately, viewport,
// program, active texture unit, and the blend/depth/scissor enable bits via a
// resolved glIsEnabled; mpv's GL struct has no IsEnabled entry).
static void yuv_diag_final_pass(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct GL *gl = &p->gl;
    // Gated on this context's own diag state only: an un-armed context has
    // ready=false and runs no final pass.
    if (!p->yuv_diag || !p->yuv.ready)
        return;

    GLint prev_read_fb = 0, prev_draw_fb = 0, prev_program = 0,
          prev_active_tex = 0, prev_tex_2d = 0, prev_blend = -1,
          prev_depth = -1, prev_scissor = -1;
    GLint prev_viewport[4] = {0};
    // V3 review fix: read and draw bindings are saved separately. Binding
    // GL_FRAMEBUFFER below retargets both, so restoring a single
    // GL_FRAMEBUFFER_BINDING would drop a caller's split read/draw state
    // (e.g. the source side of a pending glBlitFramebuffer).
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fb);
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fb);
    gl->GetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex_2d);
    gl->GetIntegerv(GL_VIEWPORT, prev_viewport);
    if (p->yuv.is_enabled) {
        prev_blend = p->yuv.is_enabled(GL_BLEND);
        prev_depth = p->yuv.is_enabled(GL_DEPTH_TEST);
        prev_scissor = p->yuv.is_enabled(GL_SCISSOR_TEST);
    }

#if MKS_YUV_P0_OBSERVE
    // b: bounded first-frame observation — the first draw reaching this point
    // means a libplacebo frame was rendered into the offscreen and submitted
    // through the swap path.
    if (!p->yuv.first_draw_logged) {
        p->yuv.first_draw_logged = true;
        MP_ERR(ctx, "YUV-DIAG: first final-pass draw begin (libplacebo frame "
               "reached the swap path)\n");
    }
#endif

    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->Viewport(0, 0, p->yuv.w, p->yuv.h);
    gl->Disable(GL_BLEND);
    gl->Disable(GL_DEPTH_TEST);
    gl->Disable(GL_SCISSOR_TEST);
    gl->UseProgram(p->yuv.program);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, p->yuv.tex);
    gl->Uniform1i(p->yuv.tex_loc, 0);
    gl->DrawArrays(GL_TRIANGLES, 0, 3);

#if MKS_YUV_P0_OBSERVE
    // b: bounded GL error observation — GetError also clears the flag, so it
    // is only read (and logged) for the first 10 draws; never a per-frame
    // habit, no glFinish.
    if (p->yuv.draw_err_logged < 10) {
        p->yuv.draw_err_logged++;
        GLenum err = gl->GetError();
        MP_ERR(ctx, "YUV-DIAG: final-pass draw #%d done (gl error=0x%x)\n",
               p->yuv.draw_err_logged, err);
    }
#endif

    // Restore everything the pass touched. The enable bits are only restored
    // when glIsEnabled resolved (a NULL resolution is logged once at setup;
    // skipping them here is deliberate, not silent).
    gl->UseProgram(prev_program);
    gl->BindTexture(GL_TEXTURE_2D, (GLuint)prev_tex_2d);
    gl->ActiveTexture((GLenum)prev_active_tex);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fb);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fb);
    gl->Viewport(prev_viewport[0], prev_viewport[1],
                 prev_viewport[2], prev_viewport[3]);
    if (p->yuv.is_enabled) {
        if (prev_blend)
            gl->Enable(GL_BLEND);
        else
            gl->Disable(GL_BLEND);
        if (prev_depth)
            gl->Enable(GL_DEPTH_TEST);
        else
            gl->Disable(GL_DEPTH_TEST);
        if (prev_scissor)
            gl->Enable(GL_SCISSOR_TEST);
        else
            gl->Disable(GL_SCISSOR_TEST);
    }
}

// Public hook for gpu_next/context.c: the FBO the libplacebo swapchain must
// render into. Guarded on the context actually being this Android GL context
// so a stray caller can never reinterpret an unrelated priv; 0 = YUV diag
// branch inactive (default framebuffer).
int mks_yuv_diag_swapchain_fbo(struct ra_ctx *ctx)
{
    extern const struct ra_ctx_fns ra_ctx_android;
    if (ctx->fns != &ra_ctx_android)
        return 0;
    struct priv *p = ctx->priv;
    return p->yuv_diag ? p->yuv.fbo : 0;
}

static void yuv_diag_uninit(struct ra_ctx *ctx)
{
    // Destroy exactly this context's own objects (per-priv, never shared).
    struct priv *p = ctx->priv;
    struct GL *gl = &p->gl;
    if (p->yuv.program)
        gl->DeleteProgram(p->yuv.program);
    if (p->yuv.tex)
        gl->DeleteTextures(1, &p->yuv.tex);
    if (p->yuv.fbo) {
        GLuint fbo = (GLuint)p->yuv.fbo;
        gl->DeleteFramebuffers(1, &fbo);
    }
    memset(&p->yuv, 0, sizeof(p->yuv));
}

// Set up the whole diag branch after the context is current and functions are
// loaded. Everything here is fail-closed.
static bool yuv_diag_setup(struct ra_ctx *ctx, int w, int h)
{
    struct priv *p = ctx->priv;
    struct GL *gl = &p->gl;

    if (gl->es < 300) {
        MP_MSG(ctx, MSGL_FATAL, "YUV-DIAG: GLES 3.0+ required\n");
        return false;
    }

    const char *gl_exts = gl->GetString(GL_EXTENSIONS)
        ? (const char *)gl->GetString(GL_EXTENSIONS) : "";
    if (!gl_check_extension(gl_exts, "GL_EXT_YUV_target")) {
        MP_MSG(ctx, MSGL_FATAL, "YUV-DIAG: GL_EXT_YUV_target missing in the "
               "GL extension string; failing open (fail-closed, no "
               "fallback)\n");
        return false;
    }
    MP_ERR(ctx, "YUV-DIAG: GL_EXT_YUV_target present\n");

    // glIsEnabled is not part of mpv's GL struct; resolve it once for the
    // final-pass enable-bit save/restore. Unresolved only shrinks the
    // restored state set, never the fail-closed checks — and is logged so
    // the reduced restore is never silent.
    p->yuv.is_enabled = (unsigned char (GLAPIENTRY *)(GLenum))
        eglGetProcAddress("glIsEnabled");
    if (!p->yuv.is_enabled) {
        MP_WARN(ctx, "YUV-DIAG: glIsEnabled unresolved; the final pass skips "
                "the blend/depth/scissor enable-bit save/restore (all "
                "queryable state is still restored)\n");
    }

    if (!yuv_diag_ensure_offscreen(ctx, w, h) ||
        !yuv_diag_ensure_program(ctx))
        return false;

    p->yuv.ready = true;
    MP_ERR(ctx, "YUV-DIAG: branch active (NV12 window, libplacebo rendered "
           "to RGB offscreen, layout(yuv) final pass)\n");
    return true;
}

// E1 experiment (MKSURF-E1, lab-gated, default off): after the EGL window
// surface is created and before the first swap, re-run the vendor's gated
// dataspace perform on the exact ANativeWindow this context commits
// through. The dataspace VALUE is unchanged (the lab arms the same PQ value
// the Java binding path applies); only the injection point moves from the
// Java surfaceCreated/binding site to the actual EGL producer's window, so
// the state is consumed at queueBuffer time on the real commit point. The
// vendor export reads a diag atomic that is 0 unless the lab armed it, so
// with the switch off this is a silent no-op. The symbol is resolved once
// per process: RTLD_DEFAULT first, then dlopen of the vendor plugin's
// soname (the plugin is already loaded in-process by its Java side); a
// failed lookup stays a permanent silent no-op with one MP_DBG line.
static void android_apply_diag_dataspace(struct ra_ctx *ctx,
                                         ANativeWindow *native_window)
{
    typedef int (*mk_vendor_apply_fn)(void *);
    static mk_vendor_apply_fn apply_fn;
    static bool resolved;
    if (!resolved) {
        resolved = true;
        apply_fn = (mk_vendor_apply_fn)dlsym(RTLD_DEFAULT,
                                             "mkvendor_apply_diag_to_window");
        if (!apply_fn) {
            // Spec'd plugin soname first, then the CMake target soname the
            // vendor plugin actually loads ("media_kit_dataspace_vendor").
            void *lib = dlopen("libmedia_kit_android_dataspace_vendor.so",
                               RTLD_NOW);
            if (!lib)
                lib = dlopen("libmedia_kit_dataspace_vendor.so", RTLD_NOW);
            if (lib)
                apply_fn = (mk_vendor_apply_fn)dlsym(
                    lib, "mkvendor_apply_diag_to_window");
        }
        if (!apply_fn)
            MP_DBG(ctx, "MKSURF-E1: vendor symbol unavailable; no-op\n");
    }
    if (!apply_fn || !native_window)
        return;
    // One-shot per EGL window surface creation, never per frame. No
    // addresses in any log line.
    int ret = apply_fn(native_window);
    if (ret > 0) {
        MP_ERR(ctx, "MKSURF-E1: dataspace applied ret=%d\n", ret);
    } else {
        MP_DBG(ctx, "MKSURF-E1: dataspace no-op ret=%d\n", ret);
    }
}

// ---------------------------------------------------------------------------
// ANativeWindow producer-callback hook (MKSURF-P0 lineage): the EGL
// producer's buffer-callback entry points are intercepted through the
// ANativeWindow function-pointer table that mpv holds, on this device
// (LG-H870DS / QCOM msm8996 Esx). The wrappers are pure passthrough on
// their own; two gated payloads ride them:
//   - MKS_YUV_P1_INJECT (feature): a metadata write on the queue path.
//   - MKS_YUV_P0_OBSERVE (diagnostic): zero-mutation observation — every
//     argument passed through untouched, nothing written to buffers or
//     metadata, bounded log output only.
// With both off, every producer call is exactly the untouched original
// entry point plus the bounded-restore bookkeeping below.
//
// Signature provenance (do NOT "fix" these to the AOSP system/window.h
// offsets): the NDK header android/native_window.h declares ANativeWindow
// opaque; the legacy function-pointer table lives in AOSP system/window.h,
// but the on-device object does NOT use the AOSP-N offsets. llvm-objdump of
// the pulled QCOM subdriver (eglSubDriverAndroid.so, lab artifact
// lg-egl-format-investigation-20261008) proves what the driver itself calls
// on the real window object:
//   window+0x70  f(window, int)                       setSwapInterval
//                (call sites 0x6cd0/0x6fac, w1 = interval, 0 == success;
//                 string "ANativeWindow setSwapInterval failed" @ 0xab47)
//   window+0x90  f(window, int what, int *value)      query (0x6d2c..0x6d6c)
//   window+0x98  f(window, int op, ...)               perform (0x6df4: op 8
//                 SET_BUFFERS_DIMENSIONS w,h; 0x7554: op 6 transform)
//   window+0xa8  f(window, ANativeWindowBuffer**, int *fence_fd)
//                dequeueBuffer (0x6e14: out buffer, out fence stored at
//                 [this+0x64] and close()d; ctor 0x7d90 inits it to -1;
//                 string "dequeueBuffer failed" @ 0xac23)
//   window+0xb0  f(window, ANativeWindowBuffer*, int fence_fd)
//                queueBuffer (0x7840: buffer from [this+0x20], fence from
//                 the EglSubDriverHelperAddFenceEvent return, 0 == success)
//   window+0xb8  f(window, ANativeWindowBuffer*, int fence_fd)
//                cancelBuffer (0x6ec8, error path, fence = -1)
// The app's ANativeWindow reaches the subdriver unwrapped: eglCreateWindowSurface
// in libEGL_adreno.so passes the native window argument straight through to
// the subdriver interface, and EglAndroidWindowSurface::Create validates the
// window magic at +0x00 and stores it verbatim at [this+0x18]. Since EGL
// renders through these very call sites on the device, these offsets and
// signatures are the device-true ABI. lockBuffer is NOT hooked: no call site
// for it appears in the disassembled paths of this build, so no confirmed
// offset exists (log-if-refused safety pattern would not apply).
//
// Safety contract:
// - Strong-typed wrappers with exactly the confirmed signatures (no variadic
//   guessing); all arguments are register-class scalars on aarch64.
// - Zero allocation, zero blocking in the wrappers (passthrough + conditional
//   log only). Fence fds are never read, copied or closed - passed through.
// - Buffer pointers are stored ONLY as dedup identity (bounded table) and
//   are never dereferenced after the wrapped call returns.
// - Lifecycle: the wrappers live for the context lifetime — install is bound
//   to android_init (diag-armed path only) and restore is bound to
//   android_uninit, which unconditionally covers the init failure path too.
//   Ownership (V2 review R1): the hook records its installer (owner). An
//   install while a hook owned by a DIFFERENT context is live is refused
//   with one MP_ERR and zero state change; the owner may replace its window
//   (previous binding restored first, then the new window hooked, one log
//   line); the owner re-entering install on the same window is an idempotent
//   success. Restore only takes effect for the owner — another context's
//   restore/uninit is a no-op (no entry-point write, no P1 state clear).
//   Restore assumption (recorded per task card): mpv's EGL producer is
//   single-threaded - the restore runs on the same render thread after the
//   swap returned and before any subsequent producer call, so no callback
//   can be in flight, and the window ends byte-identical to its pre-install
//   state.
// - The hook state is a file-static struct so a wrapper in flight never
//   dereferences a ra_ctx priv; only one window may be hooked at a time (a
//   different window replaces the previous binding, see Lifecycle above).
//
// Gate placement: see the gate matrix at the top of this file. The hook
// lifecycle (install/restore) is feature infrastructure (MKS_YUV_HOOK)
// because the P1 injection needs it; only the observation output is
// diagnostic (MKS_YUV_P0_OBSERVE). The runtime vendor switch (p->yuv_diag)
// stays the second gate for both payloads. All log lines use the YUV-P0 /
// YUV-P1 prefixes and never contain addresses.
// ---------------------------------------------------------------------------

#if MKS_YUV_HOOK

typedef int (*mks_p0_dequeue_fn)(ANativeWindow *win, void **out_buffer,
                                 int *out_fence_fd);
typedef int (*mks_p0_queue_fn)(ANativeWindow *win, void *buffer, int fence_fd);

// Device-true entry-point offsets (see the provenance block above). These are
// NOT the AOSP system/window.h offsets.
#define MKS_P0_OFF_DEQUEUE 0xa8
#define MKS_P0_OFF_QUEUE 0xb0

#if MKS_YUV_P0_OBSERVE
#define MKS_P0_MAX_SEEN 32    // dedup table cap (anti-spam bound)
#endif

// Self-contained hook state: a wrapper in flight must never dereference a
// ra_ctx priv (this struct is all it touches). One hooked window at a time,
// and exactly one owner (V2 review R1): the context that installed it.
// The trailing fields are diagnostic-only (MKS_YUV_P0_OBSERVE); with the
// gate off the struct carries exactly the passthrough lifecycle state.
struct mks_p0_state {
    // Installer identity: only this context may replace the window or
    // restore the entry points; every other context's install/restore is
    // refused/no-op (see the lifecycle contract above).
    struct ra_ctx *owner;
    ANativeWindow *win;
    mks_p0_dequeue_fn orig_dequeue;
    mks_p0_queue_fn orig_queue;
#if MKS_YUV_P0_OBSERVE
    struct mp_log *log;
    int seen;
    bool capped;
    const void *seen_buf[MKS_P0_MAX_SEEN];
#endif
};

static struct mks_p0_state mks_p0;

// ---------------------------------------------------------------------------
// P1 queue-time color-metadata injection (MKSURF-P1, feature gate
// MKS_YUV_P1_INJECT, default on; runtime double-gated, see the gating note
// at the end of this block).
//
// Question this answers: the config49 NV12 path reaches a real HWC video
// layer, but the layer is never recognized as HDR (gray picture). Binary
// forensics (qdmeta-forensics artifacts + the eglSubDriverAndroid
// disassembly) closed the recognition chain: the QCOM display stack's HDR
// decision for a video layer is driven by the gralloc COLOR_METADATA page
// of the queued buffer —
//   setMetaData(private_handle_t*, op, data)  [libqdMetaData.so, already
//     loaded in-process via gralloc.msm8996.so; it validates the
//     private_handle_t and mmaps the handle's metadata fd (4096 B, RW,
//     SHARED) itself]
//   op=2 COLOR_METADATA: memcpy(metadata_page+0x38, data, 1768)  (setMetaData
//     disasm 0x944-0x950: add x0, mmap_base, #0x38; mov w2, #0x6e8) and ORs
//     the op bit into the page's flags word (0x828-0x834)
//   HWC HWCDisplay::SetMetaData(private_handle_t*, Layer*): checks that
//     flags bit (tbnz bit1 at 0x156fc) and memcpy's [page+0x38] 1768 B into
//     the Layer (0x15740-0x1574c)
//   SDM: the layer is HDR-marked only when the copied ColorMetadata has
//     primaries==9 (BT.2020) && (transfer|2)==0x12 (16=PQ / 18=HLG) →
//     Layer 0x400 flag → HandleHDR → hal_hdr.
// mpv's EGL-allocated buffers never populate that metadata page, which is
// the no-HDR-recognition root cause under investigation.
//
// Mechanism (all buffer offsets device-proven; see the buffer-head note at
// mks_p0_note below): the queue wrapper receives the exact
// ANativeWindowBuffer the subdriver just magic-validated; its +0x60 field
// is the const native_handle_t* handle (private_handle_t is layout-
// compatible: the CAF struct begins with native_handle_t). Before passing
// the buffer to the original queueBuffer, the wrapper calls
//   setMetaData(buffer->handle, 2, color_block)
// on EVERY queue (no dedup): the write is an idempotent same-content copy
// and runs unconditionally, so the metadata page always carries the HDR flag
// regardless of buffer rotation order. The bounded set-done table records
// distinct buffers only so the table-full transition logs once; it has no
// dedup semantics and never suppresses a set.
//
// The 1768-byte color block (static const, read-only segment), first three
// int32:
//   +0x00 primaries = 9   (BT.2020, the SDM check value)
//   +0x04 range     = 1   (FIRST TRIAL value; the LIMITED enum attribution
//                          awaits the HWC SetCSC disassembly final proof)
//   +0x08 transfer  = 16  (ST2084/PQ)
// and the remaining 1764 bytes zero. The HWC consumer directly reads only
// the leading fields per the disassembly; the zero-filled tail risk is
// registered in the CR decision tree.
//
// Failure policy: exactly the four clauses declared in the gate matrix at
// the top of this file (dlsym miss / magic mismatch / rc != 0 cadence /
// recovery). A setMetaData failure NEVER blocks the queue — the only claim
// is that this buffer received no fresh injection (an older metadata page
// may persist on it; nothing is claimed about the effective HDR state). A
// placeholder buffer (<= 1x1 in the buffer head, seen before the producer's
// first setBuffersGeometry) is never injected into: the metadata identity
// triple must strictly match the buffer content, so the injection is skipped
// for it and the queue proceeds untouched (refusals are counted, the first
// one logs). The injection lives for the hook lifetime: install at
// android_init arms it, restore at android_uninit (or a window replacement)
// clears the injection state and the queue reverts to the untouched
// original entry point.
//
// Gating: P1 requires MKS_YUV_HOOK (the injection rides the queue wrapper;
// enforced by the #error in the gate matrix at the top of this file) plus
// its own double runtime gate: this context's p->yuv_diag (the config49
// branch is active) checked at the arm call site, AND a live re-read of the
// vendor FFI diag atomic (mkvendor_yuv_diag_enabled) inside yuv_p1_arm.
// With any gate off, the wrapper is the pure passthrough and behavior is
// bit-identical to before. All log lines use the YUV-P1 prefix and never
// contain addresses.
// ---------------------------------------------------------------------------

#if MKS_YUV_P1_INJECT

typedef int (*mks_setmetadata_fn)(void *handle, int param, void *param_data);

// op=2 COLOR_METADATA: the setMetaData op that memcpy's 1768 bytes of color
// metadata into the handle's metadata page at +0x38 (disasm-proven; the
// numeric constant is the authority here, not any qdMetaData.h revision).
#define MKS_P1_MD_OP_COLOR_METADATA 2
// ANativeWindowBuffer head magic — the exact constant the subdriver's own
// QueueBuffer compares at buffer+0x00 (0x7810: movz/movk of 0x5f626672).
#define MKS_P1_BUF_MAGIC 0x5f626672u
// ANativeWindowBuffer.handle offset (arm64 AOSP system/window.h layout,
// cross-confirmed by the subdriver: QueueBuffer NULL-checks [buf+0x60] at
// 0x7820 and UpdateSubResourceWithCropInfo passes it as private_handle_t*
// to GetFormatQualifier at 0x4430; the public NDK native_window.h keeps the
// struct opaque, so the disassembly is the authority).
#define MKS_P1_BUF_OFF_HANDLE 0x60
// Buffer-head geometry fields for the placeholder guard, same device-proven
// layout the P0 note block documents (width@0x38, height@0x3c; 0x04/0x08 are
// android_native_base_t common.version/reserved, never geometry).
#define MKS_P1_BUF_OFF_WIDTH 0x38
#define MKS_P1_BUF_OFF_HEIGHT 0x3c
// Bounded set-done table (same anti-spam bound as the P0 dedup table).
#define MKS_P1_MAX_BUF 32
// setMetaData failure-log cadence: first failure, then every 32nd (see the
// failure policy in the gate matrix at the top of this file).
#define MKS_P1_RC_LOG_EVERY 32
// 1768 bytes / 4 = 442 int32.
#define MKS_P1_COLOR_BLOCK_INT32 (1768 / 4)

static const int32_t mks_p1_color_block[MKS_P1_COLOR_BLOCK_INT32] = {
    9,   // +0x00 primaries (BT.2020)
    1,   // +0x04 range = FULL (matches the shader's full-normalized PQ output;
         // the display decodes this directly as PQ without range expansion)
    16,  // +0x08 transfer (ST2084/PQ)
    // remaining 1764 bytes stay zero (static const, read-only segment)
};

// Self-contained injection state (a wrapper in flight never dereferences a
// ra_ctx priv; same contract as mks_p0). Failure/refusal accounting follows
// the failure policy in the gate matrix at the top of this file.
struct mks_p1_state {
    bool armed;
    struct mp_log *log;
    mks_setmetadata_fn setmetadata;
    const void *set_done[MKS_P1_MAX_BUF];
    int set_done_count;
    bool table_full_logged;
    int refused;      // placeholder-buffer refusals since arm (first one logs)
    int rc_failures;  // cumulative setMetaData failures since arm
    bool rc_failed;   // a failure is outstanding (recovery not yet logged)
};

static struct mks_p1_state mks_p1;

// Runs on the producer thread inside the queue wrapper, BEFORE the original
// queueBuffer: inject the color metadata into the buffer's gralloc metadata
// page so the HWC consumer sees it at composition time. Inert when not
// armed; never blocks or rewrites the queue call; the fence fd belongs to
// the passthrough below and is not touched here.
static void mks_p1_inject(const void *buf)
{
    if (!mks_p1.armed)
        return;
    // No dedup: set op=0x2 on EVERY queue to guarantee the metadata page
    // always carries the HDR flag regardless of buffer rotation order.
    // ABI guard: disarm on any buffer whose head magic is not the constant
    // the subdriver itself validates — a mismatch means the layout
    // assumption above no longer holds, so fail closed (no injection, the
    // queue itself is unaffected).
    uint32_t magic = 0;
    int bw = 0, bh = 0;
    memcpy(&magic, buf, sizeof(magic));
    if (magic != MKS_P1_BUF_MAGIC) {
        mks_p1.armed = false;
        mp_err(mks_p1.log, "YUV-P1: buffer magic 0x%x unexpected (want 0x%x); "
               "injection disarmed (fail-closed)\n", (unsigned)magic,
               (unsigned)MKS_P1_BUF_MAGIC);
        return;
    }
    // Placeholder guard: the 1x1 (or degenerate) first-frame buffer queued
    // before the producer's setBuffersGeometry must not carry the metadata
    // (identity/content mismatch); skip the injection, the queue proceeds
    // untouched. Geometry comes from the device-proven buffer-head offsets.
    memcpy(&bw, (const unsigned char *)buf + MKS_P1_BUF_OFF_WIDTH, sizeof(bw));
    memcpy(&bh, (const unsigned char *)buf + MKS_P1_BUF_OFF_HEIGHT,
           sizeof(bh));
    if (bw <= 1 || bh <= 1) {
        mks_p1.refused++;
        if (mks_p1.refused == 1)
            mp_err(mks_p1.log, "YUV-P1: %dx%d placeholder buffer; injection "
                   "refused (queue passthrough)\n", bw, bh);
        return;
    }
    if (mks_p1.set_done_count >= MKS_P1_MAX_BUF) {
        if (!mks_p1.table_full_logged) {
            mks_p1.table_full_logged = true;
            mp_err(mks_p1.log, "YUV-P1: set-done table full (%d); re-setting "
                   "on every queue (idempotent same-content copy)\n",
                   MKS_P1_MAX_BUF);
        }
    } else {
        // Bookkeeping only (no dedup semantics): record distinct buffers so
        // the table-full transition logs once; sets are never skipped.
        mks_p1.set_done[mks_p1.set_done_count++] = buf;
    }
    void *handle = NULL;
    memcpy(&handle, (const unsigned char *)buf + MKS_P1_BUF_OFF_HANDLE,
           sizeof(handle));
    if (!handle)
        return;  // the subdriver pre-validated non-NULL; defensive only
    int rc = mks_p1.setmetadata(handle, MKS_P1_MD_OP_COLOR_METADATA,
                                (void *)mks_p1_color_block);
    // Failure policy clause 3/4: a non-zero rc does NOT block the queue; the
    // only claim is that THIS buffer got no fresh injection (its metadata
    // page may still carry older content — no HDR-state claim). The first
    // failure logs, then every MKS_P1_RC_LOG_EVERY-th failure logs with the
    // cumulative count; a success after failures logs the recovery once.
    if (rc != 0) {
        mks_p1.rc_failures++;
        mks_p1.rc_failed = true;
        if (mks_p1.rc_failures == 1 ||
            mks_p1.rc_failures % MKS_P1_RC_LOG_EVERY == 0)
            mp_err(mks_p1.log, "YUV-P1: setMetaData(op=%d) rc=%d (failure "
                   "#%d; queue unaffected)\n", MKS_P1_MD_OP_COLOR_METADATA,
                   rc, mks_p1.rc_failures);
    } else if (mks_p1.rc_failed) {
        mks_p1.rc_failed = false;
        mp_err(mks_p1.log, "YUV-P1: setMetaData ok again (%d failure(s) "
               "accumulated)\n", mks_p1.rc_failures);
    }
}

// Arm the injection at context setup (called only when THIS context just
// installed the P0 hook). Gate 1 is the caller's p->yuv_diag (the config49
// branch is active for this context); gate 2 is the live vendor FFI diag
// atomic re-read here — the same atomic the lab arms from Dart, read fresh
// so a disarm between init and surface creation keeps the injection off.
static void yuv_p1_arm(struct ra_ctx *ctx)
{
    typedef int (*mk_yuv_diag_fn)(void);
    static mk_yuv_diag_fn enabled_fn;
    static bool resolved;
    if (!resolved) {
        resolved = true;
        enabled_fn = (mk_yuv_diag_fn)dlsym(RTLD_DEFAULT,
                                           "mkvendor_yuv_diag_enabled");
        if (!enabled_fn) {
            void *lib = dlopen("libmedia_kit_android_dataspace_vendor.so",
                               RTLD_NOW);
            if (!lib)
                lib = dlopen("libmedia_kit_dataspace_vendor.so", RTLD_NOW);
            if (lib)
                enabled_fn = (mk_yuv_diag_fn)dlsym(
                    lib, "mkvendor_yuv_diag_enabled");
        }
    }
    if (!enabled_fn || enabled_fn() == 0) {
        MP_DBG(ctx, "YUV-P1: FFI diag switch off; injection not armed\n");
        return;
    }
    // Fresh state per install: stale buffer pointers from a previous window
    // must not suppress sets for recycled GraphicBuffer addresses.
    memset(&mks_p1, 0, sizeof(mks_p1));
    // libqdMetaData.so is already loaded in-process by gralloc.msm8996.so,
    // so RTLD_DEFAULT resolves; the dlopen soname fallback only covers a
    // late/other-scope load (an already-loaded library just refcounts).
    mks_p1.setmetadata = (mks_setmetadata_fn)dlsym(RTLD_DEFAULT,
                                                   "setMetaData");
    if (!mks_p1.setmetadata) {
        void *lib = dlopen("libqdMetaData.so", RTLD_NOW);
        if (lib)
            mks_p1.setmetadata =
                (mks_setmetadata_fn)dlsym(lib, "setMetaData");
    }
    if (!mks_p1.setmetadata) {
        MP_ERR(ctx, "YUV-P1: setMetaData unresolved; injection permanently "
               "off (queue passthrough unaffected)\n");
        return;
    }
    mks_p1.log = ctx->log;
    mks_p1.armed = true;
    MP_ERR(ctx, "YUV-P1: queue-time setMetaData injection armed (op=%d "
           "COLOR_METADATA, 1768 B: primaries=9 range=1 transfer=16)\n",
           MKS_P1_MD_OP_COLOR_METADATA);
}

#endif // MKS_YUV_P1_INJECT

// Dedup + bounded log, called from the wrappers only after the wrapped call
// returned. Geometry comes from the stable ANativeWindowBuffer head:
// magic@0x00 (0x5f626672, the exact constant the subdriver's own QueueBuffer
// compares at 0x7810), width@0x38, height@0x3c, stride@0x40, format@0x44 —
// the arm64 AOSP system/window.h layout (android_native_base_t common at
// 0x00..0x37), cross-confirmed by the subdriver reading the buffer format at
// +0x44 (UpdateSubResourceWithCropInfo 0x43f0) and the buffer handle at
// +0x60 (0x4430). Correction (P1 round): the first observation build read
// width@0x04/height@0x08/format@0x10 — that hit common.version and the
// reserved[] zeros, which is exactly why the euv10-p0b log lines read
// "168x0 fmt 0x0"; the offsets now match the disassembly.
// ANativeWindow_Buffer (the ANativeWindow_lock struct) is not
// part of these signatures; the buffer-head fields are the available
// equivalent of the requested w/h/format observation.
#if MKS_YUV_P0_OBSERVE
static void mks_p0_note(const void *buf, int rc, const char *what)
{
    for (int i = 0; i < mks_p0.seen; i++) {
        if (mks_p0.seen_buf[i] == buf)
            return;  // already logged once; pass through silently
    }
    if (mks_p0.seen >= MKS_P0_MAX_SEEN) {
        if (!mks_p0.capped) {
            mks_p0.capped = true;
            mp_err(mks_p0.log, "YUV-P0: dedup table full (%d); later "
                   "buffers pass through unlogged\n", MKS_P0_MAX_SEEN);
        }
        return;
    }
    const unsigned char *b = buf;
    int w = 0, h = 0, fmt = 0;
    memcpy(&w, b + 0x38, sizeof(w));
    memcpy(&h, b + 0x3c, sizeof(h));
    memcpy(&fmt, b + 0x44, sizeof(fmt));
    mks_p0.seen_buf[mks_p0.seen++] = buf;
    mp_err(mks_p0.log, "YUV-P0: %s #%d %dx%d fmt 0x%x rc=%d\n",
           what, mks_p0.seen - 1, w, h, (unsigned)fmt, rc);
}
#endif // MKS_YUV_P0_OBSERVE

static int mks_p0_dequeue_wrapper(ANativeWindow *win, void **out_buffer,
                                  int *out_fence_fd)
{
    mks_p0_dequeue_fn orig = mks_p0.orig_dequeue;
    if (!orig)
        return -1;  // unreachable while installed; refuse over a NULL call
    int rc = orig(win, out_buffer, out_fence_fd);
#if MKS_YUV_P0_OBSERVE
    // A failed dequeue leaves the out parameters undefined by the producer
    // contract; only a success is noted (fail-closed over a possibly
    // uninitialized out buffer pointer).
    if (rc == 0 && mks_p0.win == win && out_buffer && *out_buffer)
        mks_p0_note(*out_buffer, rc, "dequeue");
#endif
    return rc;
}

static int mks_p0_queue_wrapper(ANativeWindow *win, void *buffer, int fence_fd)
{
    mks_p0_queue_fn orig = mks_p0.orig_queue;
    if (!orig)
        return -1;  // unreachable while installed; refuse over a NULL call
#if MKS_YUV_P1_INJECT
    // P1: write the HDR color metadata into the buffer's gralloc metadata
    // page BEFORE the producer queues it, so the HWC consumer reads it at
    // composition time. Inert unless armed; never blocks the queue.
    if (mks_p0.win == win && buffer)
        mks_p1_inject(buffer);
#endif
    // The fence fd is passed through verbatim; it is never read or copied.
    int rc = orig(win, buffer, fence_fd);
#if MKS_YUV_P0_OBSERVE
    if (mks_p0.win == win && buffer)
        mks_p0_note(buffer, rc, "queue");
#endif
    return rc;
}

static void yuv_p0_restore(struct ra_ctx *ctx);

// Install the passthrough wrappers after the window surface exists and the
// context is bound, before the first frame. Ownership rules (V2 review R1):
// an install while a hook owned by a different context is live is refused
// with one MP_ERR and zero state change (the live hook's window binding,
// entry points and P1 state stay untouched); the owner may replace its
// window (previous binding restored first, then the new window hooked); the
// owner re-entering install on the same window is an idempotent success.
static void yuv_p0_install(struct ra_ctx *ctx, ANativeWindow *win)
{
    if (!win) {
        MP_ERR(ctx, "YUV-P0: no native window; not installed\n");
        return;
    }
    if (mks_p0.win) {
        if (mks_p0.owner != ctx) {
            // R1: the live hook belongs to another context — refuse without
            // touching it (no entry-point write, no P1 state clear).
            MP_ERR(ctx, "YUV-P0: hook already owned by another context; "
                   "install refused\n");
            return;
        }
        if (mks_p0.win == win)
            return;  // same owner + same window: idempotent re-entry
        // Same owner, new window: ownership transfer — restore the previous
        // binding first (which clears the P1 injection state scoped to it),
        // then hook the new window (see the lifecycle contract above).
        yuv_p0_restore(ctx);
        MP_ERR(ctx, "YUV-P0: window replaced; previous binding restored "
               "before the new install\n");
    }
    mks_p0_dequeue_fn *dq =
        (mks_p0_dequeue_fn *)((char *)win + MKS_P0_OFF_DEQUEUE);
    mks_p0_queue_fn *qb =
        (mks_p0_queue_fn *)((char *)win + MKS_P0_OFF_QUEUE);
    if (!*dq || !*qb) {
        MP_ERR(ctx, "YUV-P0: producer entry point missing at the confirmed "
               "offsets; not installed\n");
        return;
    }
    mks_p0.owner = ctx;
    mks_p0.win = win;
#if MKS_YUV_P0_OBSERVE
    mks_p0.log = ctx->log;
#endif
    mks_p0.orig_dequeue = *dq;
    mks_p0.orig_queue = *qb;
    *dq = mks_p0_dequeue_wrapper;
    *qb = mks_p0_queue_wrapper;
#if MKS_YUV_P0_OBSERVE
    MP_ERR(ctx, "YUV-P0: observing (dequeue+queue wrappers installed)\n");
#endif
}

// Restore the original entry points at a producer-quiet boundary: the owner
// context teardown (android_uninit, including the init failure path) or the
// owner replacing its window. Owner-scoped (V2 review R1): a restore attempt
// by any other context is a no-op with one MP_ERR — the hook, its window
// binding and the P1 injection state stay untouched. No-op when nothing is
// installed.
static void yuv_p0_restore(struct ra_ctx *ctx)
{
    if (!mks_p0.win)
        return;
    if (mks_p0.owner != ctx) {
        MP_ERR(ctx, "YUV-P0: restore by non-owner context ignored; hook left "
               "untouched\n");
        return;
    }
    ANativeWindow *win = mks_p0.win;
#if MKS_YUV_P0_OBSERVE
    int seen = mks_p0.seen;
#endif
    mks_p0_dequeue_fn *dq =
        (mks_p0_dequeue_fn *)((char *)win + MKS_P0_OFF_DEQUEUE);
    mks_p0_queue_fn *qb =
        (mks_p0_queue_fn *)((char *)win + MKS_P0_OFF_QUEUE);
    *dq = mks_p0.orig_dequeue;
    *qb = mks_p0.orig_queue;
#if MKS_YUV_P1_INJECT
    // P1: the injection state is scoped to the same hook lifetime. Clearing
    // it here (and re-initializing at each arm) keeps a fresh install from
    // trusting stale buffer identities of a previous window.
    memset(&mks_p1, 0, sizeof(mks_p1));
#endif
    memset(&mks_p0, 0, sizeof(mks_p0));
#if MKS_YUV_P0_OBSERVE
    MP_ERR(ctx, "YUV-P0: restored (original entry points back, %d distinct "
           "buffers observed)\n", seen);
#endif
}

#endif // MKS_YUV_HOOK

// Swap-path failure-log cadence: the first failure and every 32nd failure
// log with the cumulative count, everything in between is silent but counted
// (same style as the setMetaData rc policy in the gate matrix at the top of
// this file). Pure predicate, host-locked by the A3 policy harness.
#define MKS_SWAP_FAIL_LOG_EVERY 32
static bool mks_fail_log_now(int count)
{
    return count == 1 || count % MKS_SWAP_FAIL_LOG_EVERY == 0;
}

// The defensive not-ready swap observation fires only for an armed branch
// whose final pass is down; an un-armed context is ready=false by
// construction (the normal product path) and must stay silent. Pure
// predicate, host-locked by the A3 policy harness.
static bool mks_swap_notready(bool diag_armed, bool final_pass_ready)
{
    return diag_armed && !final_pass_ready;
}

static void android_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
#if MKS_YUV_P0_OBSERVE
    // b: bounded submit observation — the first 5 swaps log begin (with the
    // final-pass readiness, so a ready=false state is visible exactly where
    // it matters) and the EGL error at return. No queueBuffer hook.
    if (p->yuv_diag) {
        p->yuv.swap_count++;
        if (p->yuv.swap_count <= 5)
            MP_ERR(ctx, "YUV-DIAG: swap #%d begin (final-pass ready=%d)\n",
                   p->yuv.swap_count, p->yuv.ready);
    }
#endif
    // MKS YUV diag: final layout(yuv) pass from the RGB offscreen into the
    // YUV default framebuffer, immediately before the commit. Inert when the
    // branch is off (ready stays false).
    yuv_diag_final_pass(ctx);
    // Swap-path error policy (gate matrix at the top of this file): a swap
    // with the final pass down is never skipped, only logged, and an
    // eglSwapBuffers failure never changes this void path.
    if (mks_swap_notready(p->yuv_diag, p->yuv.ready)) {
        p->yuv.notready_swaps++;
        if (mks_fail_log_now(p->yuv.notready_swaps))
            MP_ERR(ctx, "YUV-DIAG: swap with final-pass ready=false (#%d); "
                   "committing without the final pass\n", p->yuv.notready_swaps);
    }
    if (!eglSwapBuffers(p->egl_display, p->egl_surface)) {
        p->swap_failures++;
        if (mks_fail_log_now(p->swap_failures))
            MP_ERR(ctx, "YUV-SWAP: eglSwapBuffers failed (error=0x%x, failure "
                   "#%d)\n", (unsigned)eglGetError(), p->swap_failures);
    }
#if MKS_YUV_P0_OBSERVE
    if (p->yuv_diag && p->yuv.swap_count <= 5) {
        EGLint err = eglGetError();
        MP_ERR(ctx, "YUV-DIAG: swap #%d returned (egl error=0x%x)\n",
               p->yuv.swap_count, err);
    }
#endif
}

static void android_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    // Hook teardown: restore the producer entry points at the context
    // teardown boundary (the hook lives for the context lifetime).
#if MKS_YUV_HOOK
    yuv_p0_restore(ctx);
#endif
    // MKS YUV diag: free GL objects while the context is still current. The
    // gl->DeleteProgram guard covers init failures that happen before
    // mpegl_load_functions ran (with yuv_diag already true).
    if (p->yuv_diag && p->gl.DeleteProgram)
        yuv_diag_uninit(ctx);

    ra_gl_ctx_uninit(ctx);

    if (p->egl_surface) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(p->egl_display, p->egl_surface);
    }
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);

    vo_android_uninit(ctx->vo);
}

static bool android_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);

    if (!vo_android_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "EGL failed to initialize.\n");
        goto fail;
    }

    // MKS YUV diag: runtime switch read at the branch decision point (same
    // dlsym pattern as E1; missing vendor symbol = off). MP_ERR so the
    // collection stream shows the branch entry even under probing verbosity.
    p->yuv_diag = yuv_diag_enabled(ctx);
    if (p->yuv_diag)
        MP_ERR(ctx, "YUV-DIAG: runtime switch on; taking config49 branch\n");

    EGLConfig config;
    if (p->yuv_diag) {
        struct mpegl_cb cb = { .yuv_diag_config = true };
        if (!mpegl_create_context_cb(ctx, p->egl_display, cb,
                                     &p->egl_context, &config))
            goto fail;
    } else if (!mpegl_create_context(ctx, p->egl_display, &p->egl_context,
                                     &config)) {
        goto fail;
    }

    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    EGLint format;
    // Fail-closed (same policy as the setBuffersGeometry check below): an
    // unattributed visual id must never reach the YUV-DIAG logs or the
    // window geometry call as an uninitialized value.
    if (!eglGetConfigAttrib(p->egl_display, config, EGL_NATIVE_VISUAL_ID,
                            &format)) {
        MP_ERR(ctx, "YUV-DIAG: EGL_NATIVE_VISUAL_ID query failed "
               "(error=0x%x); terminating (fail-closed)\n", eglGetError());
        goto fail;
    }
    if (p->yuv_diag) {
        // Gate-1b forensics + QCOM old-driver workaround: eglCreateWindowSurface
        // on a YUV config typically requires the native window format to be
        // preset to the config's visual id with the real output size, or it
        // fails with EGL_BAD_MATCH. Preset explicitly and log the outcome.
        int w = 0, h = 0;
        if (!vo_android_surface_size(ctx->vo, &w, &h)) {
            w = ANativeWindow_getWidth(native_window);
            h = ANativeWindow_getHeight(native_window);
        }
        if (w <= 0 || h <= 0) {
            MP_ERR(ctx, "YUV-DIAG: window size unknown; presetting format "
                   "0x%x with size preserved (0x0)\n", (unsigned)format);
            w = 0;
            h = 0;
        } else {
            MP_ERR(ctx, "YUV-DIAG: presetting window format 0x%x (%dx%d)\n",
                   (unsigned)format, w, h);
        }
        int32_t rc = ANativeWindow_setBuffersGeometry(native_window, w, h,
                                                      format);
        if (rc != 0) {
            MP_ERR(ctx, "YUV-DIAG: ANativeWindow_setBuffersGeometry failed "
                   "(rc=%d, format 0x%x, %dx%d); terminating (fail-closed)\n",
                   rc, (unsigned)format, w, h);
            goto fail;
        }
        MP_ERR(ctx, "YUV-DIAG: ANativeWindow_setBuffersGeometry ok "
               "(format 0x%x, %dx%d)\n", (unsigned)format, w, h);
    } else {
        ANativeWindow_setBuffersGeometry(native_window, 0, 0, format);
    }

    p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
                                    (EGLNativeWindowType)native_window, NULL);

    if (p->yuv_diag) {
        if (p->egl_surface != EGL_NO_SURFACE) {
            MP_ERR(ctx, "YUV-DIAG: eglCreateWindowSurface ok (config49)\n");
        } else {
            MP_ERR(ctx, "YUV-DIAG: eglCreateWindowSurface rejected config49 "
                   "(error=0x%x); A2a fails, no fallback\n", eglGetError());
        }
    }

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(ctx, "Could not create EGL surface!\n");
        goto fail;
    }

    // E1: after the window surface exists, before the first swap and before
    // the context is bound, re-apply the lab diag dataspace on the exact
    // window this producer commits through (strict no-op unless armed).
    android_apply_diag_dataspace(ctx, native_window);

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                        p->egl_context)) {
        if (p->yuv_diag)
            MP_ERR(ctx, "YUV-DIAG: eglMakeCurrent failed (error=0x%x)\n",
                   eglGetError());
        MP_FATAL(ctx, "Failed to set context!\n");
        goto fail;
    }
    if (p->yuv_diag)
        MP_ERR(ctx, "YUV-DIAG: eglMakeCurrent ok\n");

    // Producer-callback hook: after the window surface exists and the
    // context is bound, before the first frame - install the passthrough
    // wrappers for the context lifetime (restored at android_uninit; the P1
    // metadata injection rides the queue wrapper; inert unless the YUV diag
    // branch is active).
#if MKS_YUV_HOOK
    if (p->yuv_diag)
        yuv_p0_install(ctx, native_window);
#if MKS_YUV_P1_INJECT
    // P1 arm, double-gated: gate 1 is p->yuv_diag here (the config49 branch
    // took the YUV path for THIS context); gate 2 is the live vendor FFI
    // diag atomic re-read inside yuv_p1_arm. Arming additionally requires
    // that the hook is live for THIS window AND owned by THIS context (V2
    // review R1): after a successful install both always hold — including
    // the replacement path, where the previous window's binding (and
    // injection state) was just restored — and either check fails exactly
    // when the install was refused (no window, missing entry points, or the
    // hook already owned by another context), so a refused context can never
    // arm the injection or touch the owner's P1 state.
    if (p->yuv_diag && mks_p0.win == native_window && mks_p0.owner == ctx)
        yuv_p1_arm(ctx);
#endif
#endif

    mpegl_load_functions(&p->gl, ctx->log);

    // MKS YUV diag: fail-closed branch setup — GL_EXT_YUV_target check,
    // high-precision RGB offscreen FBO and the final layout(yuv) pass
    // program. Any failure here fails the open (no fallback).
    if (p->yuv_diag) {
        int w = 0, h = 0;
        ANativeWindow *nw = vo_android_native_window(ctx->vo);
        if (nw) {
            w = ANativeWindow_getWidth(nw);
            h = ANativeWindow_getHeight(nw);
        }
        if (!yuv_diag_setup(ctx, w, h))
            goto fail;
    }

    struct ra_ctx_params params = {
        .swap_buffers = android_swap_buffers,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto fail;

    return true;
fail:
    android_uninit(ctx);
    return false;
}

static bool android_reconfig(struct ra_ctx *ctx)
{
    int w, h;
    if (!vo_android_surface_size(ctx->vo, &w, &h))
        return false;

    // MKS YUV diag: absorb transient placeholder resizes (the Dart binding
    // emits 1x1 until the video rect is known). The previously initialized
    // offscreen/window state is kept as-is; the next real-size reconfig
    // performs the actual geometry/offscreen update. Terminating the branch
    // here would kill the session on a harmless startup transient.
    if (((struct priv *)ctx->priv)->yuv_diag && w <= 1 && h <= 1) {
        MP_ERR(ctx, "YUV-DIAG: placeholder resize %dx%d skipped; keeping "
                    "previous offscreen state\n", w, h);
        return true;
    }

    // Update window geometry to prevent screen tearing
    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (native_window) {
        int32_t current_format = ANativeWindow_getFormat(native_window);
        if (ANativeWindow_setBuffersGeometry(native_window, w, h,
                                             current_format) != 0)
            return false;
    }

    ctx->vo->dwidth = w;
    ctx->vo->dheight = h;

    // MKS YUV diag: keep the offscreen FBO at the surface size before the
    // libplacebo swapchain re-wraps it (gpu_ctx_resize -> pl_swapchain_resize
    // runs right after this in vo_gpu_next's reconfig path).
    if (((struct priv *)ctx->priv)->yuv_diag &&
        !yuv_diag_ensure_offscreen(ctx, w, h))
        return false;

    ra_gl_ctx_resize(ctx->swapchain, w, h, 0);
    return true;
}

static int android_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    return VO_NOTIMPL;
}

const struct ra_ctx_fns ra_ctx_android = {
    .type           = "opengl",
    .name           = "android",
    .description    = "Android/EGL",
    .reconfig       = android_reconfig,
    .control        = android_control,
    .init           = android_init,
    .uninit         = android_uninit,
};
