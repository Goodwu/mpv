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

/*
 * media-kit Phase 1 PoC: API24 SurfaceTexture full-GPU import.
 *
 * Decoder frames (IMGFMT_MEDIACODEC, planes[3] = AVMediaCodecBuffer) are
 * rendered once into an ANativeWindow that wraps a bridge-owned
 * SurfaceTexture. Per frame the mapper:
 *   1. releases the codec buffer exactly once with render=1,
 *   2. latches (bridge-side listener wait + updateTexImage on this GL thread),
 *   3. copies the OES texture identity-wise into a private 2D lease texture
 *      (FBO pass, no matrix/range/transfer conversion whatsoever),
 *   4. fences the copy and publishes the 2D texture via mapper->tex[0].
 *
 * Binding invariants (phase1-design-spec §2/§3):
 *   - never loads under auto/probe (hw->probing gate),
 *   - bridge so must already be resident (RTLD_NOLOAD),
 *   - the OES texture is created by the bridge (never GenTextures here);
 *     mappers only bind it, the owner deletes the GL name exactly once at
 *     hwdec uninit so mapper rebuilds always find it alive,
 *   - lease pool is bounded at 4, every lease carries a fence,
 *   - no per-frame glFinish; retirement waits are bounded,
 *   - color semantics of the OES output domain are UNDECLARED: the temporary
 *     dst_params declaration only keeps the pipeline running and every
 *     temporary value is logged with "MKSURF: unverified-declaration".
 * All telemetry lines use the fixed "MKSURF:" prefix so evidence can be
 * grep-collected on device.
 */

#include "config.h"

#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <EGL/egl.h>
#include <jni.h>
#include <android/native_window.h>

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "osdep/timer.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/opengl/ra_gl.h"
#include "video/mp_image.h"

#ifndef GL_TEXTURE_BINDING_EXTERNAL_OES
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif

// Sentinel for "lease holds no pool slot". Explicitly required: the mapper
// priv is zero-allocated, so 0 would alias the first real slot.
#define MKSURF_NO_SLOT (-1)

// Total number of 2D lease textures (current + retired leases share this pool).
#define MKSURF_POOL_SIZE 4
// Retired leases kept around for redraws of the previous frames.
#define MKSURF_KEEP_RETIRED 2
// Bounded latch wait; timeout fails the map without publishing stale content.
#define MKSURF_LATCH_TIMEOUT_MS 50
// Cold-start grace: the first latches wait longer, because listener dispatch
// and the decoder cold-start budget are still warming up (run4 evidence: the
// very first latch timed out at 50 ms and the frame was correctly dropped by
// the consumed-registry gate on retry). Timeout behavior is unchanged; only
// the bound differs for these latches.
#define MKSURF_LATCH_GRACE_COUNT 2
#define MKSURF_LATCH_GRACE_TIMEOUT_MS 250
// Bounded retirement wait before giving up on a stuck fence.
#define MKSURF_REAP_TIMEOUT_MS 50
// Per-map identity bound floor: |latch_ts - src_pts| beyond
// max(1.5/fps, this floor) is dropped as stale. Stateless: every map is
// judged on its own inputs only.
#define MKSURF_STALE_FLOOR_S 0.1
// Fallback for the 1.5/fps term when nominal_fps is unavailable/invalid.
#define MKSURF_STALE_FPS_FALLBACK_S 0.5
// Bounded consumed-identity registry size (ring). Bound arithmetic: at most
// decoder output queue (~8) + mpv frame queue (<=4) + lease pool (4) +
// retired leases (3) ~= 20 identities can be simultaneously live, well below
// 64. The registry deliberately outlives its entries, so once the ring is
// full, ring eviction is steady-state behavior (about one eviction per
// frame; see consumed_add): evicted identities move to the graveyard ring
// for idempotent-reacquire recognition, never a silent overwrite.
#define MKSURF_CONSUMED_SIZE 64
// Recently-evicted identities kept only so an idempotent reacquire (a frame
// consumed once, evicted, then re-mapped after every lease died) can be
// recognized and logged loudly instead of passing as a fresh frame.
#define MKSURF_EVICTED_SIZE 16

#define MKSURF_BRIDGE_SO "libmedia_kit_video_hdr_bridge.so"

// ---------------------------------------------------------------------------
// MKS_ST_DIAG_READBACK: compile-time diagnostic gate for the per-frame lease
// readback evidence hook (mkst_diag_readback + its call site + its priv
// state). Diagnostic gates default OFF, and with the gate at 0 the compiled
// object must stay byte-identical to a build with the readback code
// physically deleted (verification: compile both with the exact product
// command and compare the object sha256). The runtime switch is separate:
// the bridge export mkst_diag_enabled() only arms the hook per session, the
// compile gate decides whether the code exists at all.
// ---------------------------------------------------------------------------
#ifndef MKS_ST_DIAG_READBACK
#define MKS_ST_DIAG_READBACK 0
#endif

// C API exported by the media_kit_video_hdr_bridge so (spec §2.4/§4). All
// entry points are resolved at driver init; the bridge itself is initialized
// by the media_kit_video platform view before this driver can load.
// mkst_ensure_init is verify-only: returns 1 when the bridge is initialized
// and 0 when it is not; it never initializes — the Java side owns the init.
typedef int (*mkst_ensure_init_fn)(void);
typedef void *(*mkst_create_decoder_window_fn)(void);
typedef int (*mkst_latch_fn)(int timeout_ms, uint64_t *out_ts_ns);
typedef int (*mkst_get_texture_name_fn)(void);
// jobject Surface wrapping the bridge SurfaceTexture (global-ref owned by
// the bridge); the FFmpeg JNI wrapper's configure path requires it.
typedef void *(*mkst_get_surface_jobject_fn)(void);
// int return: matches the bridge's actual mkst_shutdown export signature
// (V2 review R2; the caller ignores the result).
typedef int (*mkst_shutdown_fn)(void);
typedef const char *(*mkst_version_fn)(void);
#if MKS_ST_DIAG_READBACK
// Optional diagnostic symbol: may be absent in older bridges. A dlsym miss
// here is not fatal (unlike the mandatory symbols) and only means the
// diagnostic readback hook stays permanently off. Diagnostic-only plumbing
// (typedef, priv field and dlsym resolution): all inside the gate so the
// off identity covers them (V2 review R5).
typedef int (*mkst_diag_enabled_fn)(void);
#endif

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    ANativeWindow *anw;
    // Never dlclose()d: the bridge so stays resident for the process lifetime
    // (spec §2.9) and its JNI state outlives this hwdec.
    void *lib_handle;
    // GL saved at init for owner-level teardown (OES texture name deletion).
    GL *gl;
    // The bridge SurfaceTexture's OES texture name. Owned at owner level:
    // mappers only bind it, they must never delete it (vo_gpu_next destroys
    // and rebuilds mappers while the bridge stays alive; a deleted name would
    // hand the next mapper a dead texture). Deleted exactly once in uninit.
    GLuint oes_tex_name;

    mkst_ensure_init_fn ensure_init;
    mkst_create_decoder_window_fn create_decoder_window;
    mkst_latch_fn latch;
    mkst_get_texture_name_fn get_texture_name;
    mkst_get_surface_jobject_fn surface_jobject;
    mkst_shutdown_fn shutdown;
    mkst_version_fn version;
#if MKS_ST_DIAG_READBACK
    mkst_diag_enabled_fn diag_enabled; // NULL = hook permanently off
#endif
    // Decoder-window ownership (V2 review R2): set exactly once, when THIS
    // instance successfully acquired the decoder window from the bridge
    // (create_decoder_window succeeded). Only such an owner may call
    // mkst_shutdown in uninit; an instance whose create was refused (another
    // instance holds the window) or that never reached create must leave the
    // shared bridge untouched. Later init failures (texture name, surface
    // jobject, device ctx) do NOT clear it: the bridge holds this instance's
    // active window, so its uninit must release it via mkst_shutdown.
    bool owns_decoder_window;
};

// One texture slot in the bounded lease pool. Texture objects live as long as
// the mapper (recreated only if the source dimensions change); leases
// reference them by index.
struct mkst_slot {
    struct ra_tex *tex; // 2D lease texture, we own both ra_tex and GL name
    GLuint name;
    int w, h;
};

// A published frame: the private 2D copy plus everything needed for redraw
// matching, retirement and identity evidence.
struct mkst_lease {
    // Keeps the mp_image (and thus planes[3] = AVMediaCodecBuffer) alive for
    // redraw identity matching. Refs are taken in map(), released in reap.
    struct mp_image *source;
    double src_pts;   // seconds (mp_image.pts unit)
    uint64_t latch_ts; // SurfaceTexture timestamp, ns
    GLsync fence;     // signals after the renderer is done sampling the copy
    int slot;         // index into priv->slots[]; -1 = empty
};

// Registry entry for a codec frame whose single explicit release has already
// happened. Key = planes[3] pointer + pts (double match), same identity the
// lease redraw matching uses.
struct mkst_consumed {
    void *buffer;  // planes[3] identity (AVMediaCodecBuffer *)
    double pts;    // mp_image.pts, seconds
    bool used;
};

struct priv {
    GL *gl;
    struct mp_log *log;

    // Bridge-owned OES texture, wrapped for ra (we never create/delete the
    // GL texture object itself).
    struct ra_tex *oes_tex;
    GLuint oes_tex_name;

    // Identity copy program (OES sampler -> plain 2D write) and one shared
    // FBO used for every copy and the format probe.
    GLuint program;
    GLuint fbo;

    // Chosen 2D format (probed once per mapper) and the mpv imgfmt declared
    // downstream for it.
    const struct ra_format *fmt;
    int dst_imgfmt;

    // Retire-and-fence protection for published copies.
    bool have_fence;

    struct mkst_slot slots[MKSURF_POOL_SIZE];
    struct mkst_lease cur; // active mapping; mapper->tex[0] mirrors cur
    struct mkst_lease retired[MKSURF_POOL_SIZE - 1];
    int retired_count;

    // Bounded consumed-identity registry: every frame that has had its one
    // explicit release is registered here before any further map work. Hits
    // must be served from a lease (adopt) and never released/latched again.
    struct mkst_consumed consumed[MKSURF_CONSUMED_SIZE];
    int consumed_next;
    // Graveyard for identities evicted from the registry above, so an
    // idempotent reacquire can be recognized (and counted loudly) instead of
    // silently re-releasing.
    struct mkst_consumed evicted[MKSURF_EVICTED_SIZE];
    int evicted_next;

    // Telemetry counters, all logged with the fixed MKSURF: prefix.
    uint64_t release_count;                // av_mediacodec_release_buffer calls
    uint64_t release_failed_count;         // release returned an error
    uint64_t consumed_overflow_count;      // consumed ring evicted a live entry
    uint64_t horizon_overflow_count;       // consumed hit with no lease to serve
    uint64_t idempotent_reacquire_count;   // release on an evicted identity
    uint64_t nonfinite_pts_count;          // map rejected: non-finite src pts
    uint64_t latch_count;
    int latch_attempts;                    // total mkst_latch calls (grace gate)
    uint64_t latch_timeout_count;
    uint64_t stale_drop_count;             // per-map identity bound exceeded
    uint64_t fence_created_count;
    uint64_t fence_deleted_count;
    uint64_t fence_create_failed_count;
    uint64_t retire_count;
    uint64_t reap_count;
    uint64_t reap_stuck_count;
    uint64_t pool_peak;             // max concurrent leases (current+retired)
    bool logged_fps_fallback;

#if MKS_ST_DIAG_READBACK
    // Diagnostic readback hook state (mkst_diag_readback). The buffer is a
    // fixed priv member (center 8x8, GL_RGBA + GL_FLOAT = 64*4 GLfloat) so
    // the per-frame hook never mallocs.
    GLfloat diag_buf[8 * 8 * 4];
    bool diag_readback_unsupported; // reported once, then never retried
#endif
};

// Full-screen triangle from gl_VertexID (GLES 3 only, no attribute buffers):
// positions (-1,-1), (3,-1), (-1,3) with matching texcoords (0,0), (2,0), (0,2).
static const char mkst_vertex_src[] =
    "#version 300 es\n"
    "out vec2 texcoord;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID & 1) << 2),\n"
    "                  float((gl_VertexID & 2) << 1));\n"
    "    texcoord = p * 0.5;\n"
    "    gl_Position = vec4(p - 1.0, 0.0, 1.0);\n"
    "}\n";

// Identity copy: sample the OES texture and store the encoded values
// unchanged. No linearization, no range multiply/add, no transfer or matrix
// conversion is permitted here (spec §3).
static const char mkst_fragment_src[] =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES oes_tex;\n"
    "in vec2 texcoord;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = texture(oes_tex, texcoord);\n"
    "}\n";

// Both handles must be set on the device context:
// - surface (the jobject): the JNI wrapper's configure path reads ONLY this
//   (mediacodec_wrapper.c:1383, mediacodec_jni_configure) — with only
//   native_window set, configure silently falls back to buffer mode, release
//   is ignored and the SurfaceTexture never sees a frame (device run4 root
//   cause).
// - native_window: used by the NDK wrapper and kept as the precedence
//   fallback. FFmpeg performs its own acquire/release pairing on it
//   (ff_mediacodec_surface_ref).
// Teardown ordering: hwdec uninit calls mkst_shutdown() first and releases
// the ANativeWindow and the device ref afterwards. The jobject stays valid
// for every use because decoder use has already ended by then (media close
// precedes vo teardown) and because the jobject is held by the bridge's
// global reference; FFmpeg's ff_mediacodec_surface_ref creates its own
// NewGlobalRef around it, so this driver neither holds nor deletes a JNI
// reference.
static AVBufferRef *create_mediacodec_device_ref(jobject surface,
                                                 ANativeWindow *anw)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->surface = surface;
    hwctx->native_window = anw;

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static bool load_bridge_functions(struct priv_owner *p, struct mp_log *log)
{
    // The bridge must already be resident (loaded by the media_kit_video
    // platform view). RTLD_NOLOAD never starts it from here.
    p->lib_handle = dlopen(MKSURF_BRIDGE_SO, RTLD_NOLOAD | RTLD_GLOBAL);
    if (!p->lib_handle) {
        mp_err(log, "MKSURF: bridge so not resident (dlopen RTLD_NOLOAD failed): %s\n",
               dlerror());
        return false;
    }

    const struct { const char *sym; void **fn; } tab[] = {
        { "mkst_ensure_init",           (void **)&p->ensure_init },
        { "mkst_create_decoder_window", (void **)&p->create_decoder_window },
        { "mkst_latch",                 (void **)&p->latch },
        { "mkst_get_texture_name",      (void **)&p->get_texture_name },
        { "mkst_get_surface_jobject",   (void **)&p->surface_jobject },
        { "mkst_shutdown",              (void **)&p->shutdown },
        { "mkst_version",               (void **)&p->version },
    };
    for (int i = 0; i < (int)MP_ARRAY_SIZE(tab); i++) {
        *tab[i].fn = dlsym(p->lib_handle, tab[i].sym);
        if (!*tab[i].fn) {
            mp_err(log, "MKSURF: dlsym failed for %s\n", tab[i].sym);
            return false;
        }
    }

#if MKS_ST_DIAG_READBACK
    // Optional diagnostic symbol: resolved separately because it may not
    // exist in older bridges. NULL only disables the diagnostic readback
    // hook; the mandatory symbols above still fail closed. Inside the gate
    // so the off identity covers this plumbing (V2 review R5).
    p->diag_enabled =
        (mkst_diag_enabled_fn)dlsym(p->lib_handle, "mkst_diag_enabled");
#endif
    return true;
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    // Opt-in gate first of all: the default/auto probe must never activate
    // this PoC importer; only an explicit hwdec=surfacetexture request
    // (is_auto == false) reaches here with probing == false.
    if (hw->probing) {
        MP_VERBOSE(hw, "MKSURF: auto probe refused; explicit hwdec=surfacetexture required\n");
        return -1;
    }

    // GL/EGL gate, same as the aimagereader importer: the mapper runs on the
    // GL thread of an app EGL context.
    if (!ra_is_gl(hw->ra_ctx->ra))
        return -1;
    if (!eglGetCurrentContext())
        return -1;
    // Kept for owner-level teardown (OES texture name deletion in uninit);
    // the ra and its GL are still alive at that point (hwdec ctx uninit runs
    // before ra teardown in both vo_gpu and vo_gpu_next).
    p->gl = ra_gl_get(hw->ra_ctx->ra);

    if (!load_bridge_functions(p, hw->log))
        return -1;

    // Evidence visibility: the lab evidence stream only passes through error
    // level, so this candidate-critical telemetry line is deliberately
    // emitted at error level for the PoC (no address content, no behavioral
    // meaning; strictly an evidence emission).
    MP_ERR(hw, "MKSURF: version %s\n", p->version());

    // Frozen contract: verify-only ensure_init (1 = the bridge is
    // initialized, 0 = not initialized; it never initializes). The Java
    // platform-view side owns the init, so a 0 here fails the open — the
    // importer must never initialize the bridge on its own.
    if (p->ensure_init() != 1) {
        MP_ERR(hw, "MKSURF: bridge not initialized (verify-only ensure_init; "
               "Java side owns init)\n");
        return -1;
    }

    p->anw = p->create_decoder_window();
    if (!p->anw) {
        MP_ERR(hw, "MKSURF: bridge create_decoder_window failed\n");
        return -1;
    }

    // Shutdown ownership is recorded here, immediately after
    // create_decoder_window succeeds and independent of the success of every
    // later init step (V2 re-review R2 partial-acquire fix): a successful
    // create leaves the bridge holding an active decoder window for THIS
    // instance, so if any later step fails (OES texture name, surface
    // jobject, device ctx), uninit must still call mkst_shutdown to release
    // the resources this instance acquired. Only a refused create (NULL: the
    // window is held elsewhere or the bridge never established one) leaves
    // owns_decoder_window false so uninit leaves the shared bridge untouched.
    p->owns_decoder_window = true;

    // Take ownership of the OES texture name before the surface jobject is
    // taken: every later failure path in init() must leave the name in
    // p->oes_tex_name so owner uninit deletes it exactly once (B1 fix: with
    // the previous order, a NULL surface jobject returned before this point
    // leaked the bridge-created OES texture). Managed separately from the
    // shutdown ownership above: once the name is taken, the deletion
    // responsibility stays with this instance (owner uninit), regardless of
    // the success of the remaining init steps.
    int oes_name = p->get_texture_name();
    if (oes_name <= 0) {
        MP_ERR(hw, "MKSURF: bridge returned no OES texture name (%d)\n",
               oes_name);
        return -1;
    }
    p->oes_tex_name = (GLuint)oes_name;

    // jobject Surface for the FFmpeg JNI wrapper's configure path (it reads
    // only window->surface, mediacodec_wrapper.c:1383). Fail closed without
    // it: configure would silently run in buffer mode and the SurfaceTexture
    // would never see a frame. The jobject stays bridge-owned (global ref);
    // this driver never deletes it.
    jobject surf = p->surface_jobject();
    if (!surf) {
        MP_ERR(hw, "MKSURF: bridge returned no surface jobject\n");
        return -1;
    }

    p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = hw->driver->name,
        .av_device_ref = create_mediacodec_device_ref(surf, p->anw),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };
    if (!p->hwctx.av_device_ref) {
        MP_ERR(hw, "MKSURF: failed to create mediacodec hwdevice ctx\n");
        return -1;
    }

    hwdec_devices_add(hw->devs, &p->hwctx);

    MP_INFO(hw, "MKSURF: init driver=%s probing=%d device_native_window=%d\n",
            hw->driver->name, (int)hw->probing, (int)(p->anw != NULL));
    return 0;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    // OES texture name teardown: the owner-level deletion point, after all
    // mappers are destroyed (ra_hwdec_ctx_uninit frees them first) and before
    // the bridge shutdown. Deleting in mapper uninit would break mapper
    // rebuilds: vo_gpu_next destroys and recreates mappers while the bridge
    // and its SurfaceTexture stay alive, and the next mapper must find the
    // texture name still alive. Every created GLsync was already accounted
    // by the mapper; the OES name is deleted exactly once here.
    if (p->oes_tex_name && p->gl) {
        p->gl->DeleteTextures(1, &p->oes_tex_name);
        p->oes_tex_name = 0;
    }

    // Bridge teardown first, then our window reference, then the device.
    // Ownership gate (V2 review R2): only an instance that actually acquired
    // the decoder window (create succeeded) may shut the bridge down. An
    // instance whose create was refused (another instance holds the window)
    // or that never reached create must leave the shared bridge state
    // untouched. An init that failed after a successful create reaches this
    // with owns_decoder_window true and releases the acquired bridge state
    // here (partial-acquire fix); the OES name is deleted only if it was
    // actually taken before the failure.
    if (p->owns_decoder_window && p->shutdown)
        p->shutdown();
    // Ownership is consumed by the teardown: like the OES name, the window
    // reference and the lib handle above, the flag self-clears so a repeated
    // uninit stays inert.
    p->owns_decoder_window = false;

    if (p->anw) {
        ANativeWindow_release(p->anw);
        p->anw = NULL;
    }

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);

    // Intentionally no dlclose(): the bridge so remains resident.
    p->lib_handle = NULL;
}

static GLuint compile_shader(struct ra_hwdec_mapper *mapper, GLenum type,
                             const char *src)
{
    struct priv *p = mapper->priv;
    GL *gl = p->gl;

    GLuint sh = gl->CreateShader(type);
    if (!sh)
        return 0;
    gl->ShaderSource(sh, 1, &src, NULL);
    gl->CompileShader(sh);
    GLint ok = 0;
    gl->GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char logbuf[512];
        gl->GetShaderInfoLog(sh, sizeof(logbuf), NULL, logbuf);
        MP_ERR(mapper, "MKSURF: shader compile failed: %s\n", logbuf);
        gl->DeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_copy_program(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = p->gl;

    GLuint vs = compile_shader(mapper, GL_VERTEX_SHADER, mkst_vertex_src);
    GLuint fs = vs ? compile_shader(mapper, GL_FRAGMENT_SHADER, mkst_fragment_src)
                   : 0;
    GLuint prog = 0;
    if (fs) {
        prog = gl->CreateProgram();
        gl->AttachShader(prog, vs);
        gl->AttachShader(prog, fs);
        gl->LinkProgram(prog);
        GLint ok = 0;
        gl->GetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char logbuf[512];
            gl->GetProgramInfoLog(prog, sizeof(logbuf), NULL, logbuf);
            MP_ERR(mapper, "MKSURF: program link failed: %s\n", logbuf);
            gl->DeleteProgram(prog);
            prog = 0;
        } else {
            gl->UseProgram(prog);
            gl->Uniform1i(gl->GetUniformLocation(prog, "oes_tex"), 0);
            gl->UseProgram(0);
        }
    }
    if (vs)
        gl->DeleteShader(vs);
    if (fs)
        gl->DeleteShader(fs);
    return prog;
}

// Identity copy of the current OES texture content into dst_name. The caller
// must have latched (updateTexImage ran on this thread) beforehand.
static bool draw_copy(struct ra_hwdec_mapper *mapper, GLuint dst_name,
                      int w, int h)
{
    struct priv *p = mapper->priv;
    GL *gl = p->gl;

    // V2 review R4: the read and draw framebuffer bindings are saved and
    // restored separately (a single GL_FRAMEBUFFER_BINDING save drops a
    // caller's split read/draw state, e.g. the source side of a pending
    // glBlitFramebuffer), and the viewport is saved and restored too (this
    // pass overwrites it with the copy geometry).
    GLint old_read_fbo = 0, old_draw_fbo = 0, old_program = 0, old_active = 0,
          old_oes = 0;
    GLint old_viewport[4] = {0};
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    gl->GetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    gl->GetIntegerv(GL_VIEWPORT, old_viewport);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_oes);

    gl->BindFramebuffer(GL_FRAMEBUFFER, p->fbo);
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, dst_name, 0);
    GLenum fbo_status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
    bool ok = fbo_status == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        gl->Viewport(0, 0, w, h);
        gl->UseProgram(p->program);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->oes_tex_name);
        gl->DrawArrays(GL_TRIANGLES, 0, 3);
        ok = gl->GetError() == GL_NO_ERROR;
    }
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_oes);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read_fbo);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw_fbo);
    gl->Viewport(old_viewport[0], old_viewport[1],
                 old_viewport[2], old_viewport[3]);
    gl->UseProgram(old_program);
    gl->ActiveTexture(old_active);

    if (!ok)
        MP_ERR(mapper, "MKSURF: copy draw failed (fbo status=0x%x)\n", fbo_status);
    return ok;
}

#if MKS_ST_DIAG_READBACK
// Diagnostic readback hook: after a completed 2D identity copy, read back
// the center 8x8 region of the just-written lease texture through the copy
// FBO and emit a single MP_ERR evidence line (first row: 8 pixels, rgb per
// pixel). The previous READ/DRAW_FRAMEBUFFER binding is saved and restored.
// An unsupported readback (GetError != 0) is a one-strike state: the flag
// stops the call site from entering here at all, and this function also
// early-outs BEFORE any GL work, so a failed readback is never retried.
// No-op unless the bridge exports mkst_diag_enabled() and it returns
// non-zero.
static void mkst_diag_readback(struct ra_hwdec_mapper *mapper, GLuint tex_name,
                               double pts)
{
    struct priv *p = mapper->priv;
    GL *gl = p->gl;

    // One-strike short circuit: after a failed readback this entry point
    // must not touch GL state again (it runs every map).
    if (p->diag_readback_unsupported)
        return;

    GLint old_read_fbo = 0, old_draw_fbo = 0;
    // V2 review R4: the read and draw framebuffer bindings are saved and
    // restored separately (the previous single GL_FRAMEBUFFER_BINDING save
    // dropped a caller's split read/draw state).
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    gl->BindFramebuffer(GL_FRAMEBUFFER, p->fbo);
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex_name, 0);

    int x0 = (int)mapper->src_params.w / 2 - 4;
    int y0 = (int)mapper->src_params.h / 2 - 4;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;

    gl->ReadPixels(x0, y0, 8, 8, GL_RGBA, GL_FLOAT, p->diag_buf);
    bool ok = gl->GetError() == GL_NO_ERROR;

    // Restore the bindings saved above, each to its own target.
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read_fbo);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw_fbo);

    if (!ok) {
        p->diag_readback_unsupported = true;
        MP_ERR(mapper, "MKSURF: diag readback-unsupported\n");
        return;
    }

    // First row of the 8x8 block: 8 pixels, rgb per pixel (alpha dropped),
    // comma-separated. GLfloat is widened to double for the varargs call.
    char vals[384];
    int off = 0;
    bool truncated = false;
    for (int i = 0; i < 8 && !truncated; i++) {
        const GLfloat *px = &p->diag_buf[(size_t)i * 4];
        for (int c = 0; c < 3; c++) {
            int n = snprintf(vals + off, sizeof(vals) - off, "%s%1.4f",
                             off > 0 ? "," : "", (double)px[c]);
            if (n < 0 || n >= (int)sizeof(vals) - off) {
                truncated = true;
                break;
            }
            off += n;
        }
    }
    MP_ERR(mapper, "MKSURF: diag pts=%.6f y0=[%s%s]\n", pts, vals,
           truncated ? ",..." : "");
}
#endif // MKS_ST_DIAG_READBACK

// Real-draw probe for one candidate 2D format: a 4x4 texture is created,
// attached to an FBO and the identity copy is actually drawn. FBO
// completeness plus a clean GetError are the renderable evidence; filterable
// evidence comes from ra_format.linear_filter (required before probing).
static bool probe_draw(struct ra_hwdec_mapper *mapper, const struct ra_format *fmt)
{
    struct ra_tex_params params = {
        .dimensions = 2,
        .w = 4,
        .h = 4,
        .d = 1,
        .format = fmt,
        .render_dst = true,
    };
    struct ra_tex *tex = ra_tex_create(mapper->ra, &params);
    if (!tex)
        return false;
    GLuint name = 0;
    GLenum target = 0;
    ra_gl_get_raw_tex(mapper->ra, tex, &name, &target);
    bool ok = draw_copy(mapper, name, 4, 4);
    ra_tex_free(mapper->ra, &tex);
    return ok;
}

// 2D format probe, fixed order RGBA16F -> RGBA16 unorm (V2 review removed
// RGB10_A2: IMGFMT_RGB30 declares the channel order swapped). A silent RGBA8
// fallback is forbidden: if every candidate fails, the mapper init fails
// outright.
static const struct ra_format *probe_2d_format(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    const struct ra_format *candidates[] = {
        ra_find_float16_format(mapper->ra, 4),          // rgba16f
        ra_find_unorm_format(mapper->ra, 2, 4),         // rgba16 (unorm)
    };
    const char *names[] = { "rgba16f", "rgba16-unorm" };
    // mpv imgfmt declared downstream for each candidate. rgba16f has no exact
    // mpv imgfmt (no float formats), so the nearest existing one is declared
    // and logged as such (spec §2.6 allows this with annotation). RGB10_A2
    // was removed as a candidate: its IMGFMT_RGB30 declaration has the channel
    // order swapped relative to the GL format (V2 review, adjudicated).
    const int dst_imgfmts[] = { IMGFMT_RGBA64, IMGFMT_RGBA64 };
    const bool exact[] = { false, true };

    for (int i = 0; i < (int)MP_ARRAY_SIZE(candidates); i++) {
        const struct ra_format *fmt = candidates[i];
        if (!fmt) {
            MP_INFO(mapper, "MKSURF: format candidate %s unavailable in ra\n",
                    names[i]);
            continue;
        }
        if (fmt->ctype != RA_CTYPE_UNORM && fmt->ctype != RA_CTYPE_FLOAT) {
            MP_INFO(mapper, "MKSURF: format candidate %s rejected (ctype)\n",
                    names[i]);
            continue;
        }
        if (!fmt->renderable || !fmt->linear_filter) {
            MP_INFO(mapper, "MKSURF: format candidate %s rejected (renderable=%d "
                            "linear_filter=%d)\n",
                    names[i], fmt->renderable, fmt->linear_filter);
            continue;
        }
        if (!probe_draw(mapper, fmt)) {
            MP_INFO(mapper, "MKSURF: format candidate %s rejected (real FBO draw "
                            "failed)\n", names[i]);
            continue;
        }

        // Each rejected candidate is logged at its rejection site above.
        p->dst_imgfmt = dst_imgfmts[i];
        MP_INFO(mapper, "MKSURF: capability 2D format %s chosen (FBO complete + "
                        "GL_LINEAR filterable verified by real draw)\n",
                fmt->name);
        MP_INFO(mapper, "MKSURF: capability dst imgfmt %s%s\n",
                mp_imgfmt_to_name(p->dst_imgfmt),
                exact[i] ? " (exact match)" : " (nearest mpv imgfmt; no exact "
                                               "match for the GL format)");
        return fmt;
    }

    MP_ERR(mapper, "MKSURF: no usable 2D format (rgba16f/rgba16 both "
                   "failed); refusing RGBA8 fallback, init fails\n");
    return NULL;
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    p->gl = ra_gl_get(mapper->ra);
    GL *gl = p->gl;
    p->log = mapper->log;

    // The zero-allocated priv aliases slot 0; mark every lease empty before
    // any early-failure return can reach unmap/uninit.
    p->cur.slot = MKSURF_NO_SLOT;
    for (int i = 0; i < (int)MP_ARRAY_SIZE(p->retired); i++)
        p->retired[i].slot = MKSURF_NO_SLOT;

    // The OES texture name was taken from the bridge at device init and is
    // owned by priv_owner (deleted only at hwdec uninit, never here — mapper
    // rebuilds must find it alive). Read the owner's copy; creating our own
    // texture with GenTextures is not allowed (spec §2.6).
    int oes_name = (int)o->oes_tex_name;
    if (oes_name <= 0) {
        MP_ERR(mapper, "MKSURF: owner holds no OES texture name (%d)\n",
               oes_name);
        return -1;
    }
    p->oes_tex_name = (GLuint)oes_name;

    struct ra_tex_params oes_params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = ra_find_unorm_format(mapper->ra, 1, 4),
        .render_src = true,
        .src_linear = true,
        .external_oes = true,
    };
    if (!oes_params.format || oes_params.format->ctype != RA_CTYPE_UNORM)
        return -1;
    p->oes_tex = ra_create_wrapped_tex(mapper->ra, &oes_params, p->oes_tex_name);
    if (!p->oes_tex)
        return -1;

    // Sampler state for the bridge texture (configuration only; the texture
    // object itself stays bridge-owned).
    {
        GLint old_active = 0, old_oes = 0;
        gl->GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
        gl->ActiveTexture(GL_TEXTURE0);
        gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_oes);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->oes_tex_name);
        gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_oes);
        gl->ActiveTexture(old_active);
    }

    // The identity copy program samples the external texture, which on GLES3
    // requires the essl3 form of the external image extension.
    if (gl->es < 300 ||
        !gl_check_extension(gl->extensions, "GL_OES_EGL_image_external_essl3")) {
        MP_ERR(mapper, "MKSURF: GLES3 with GL_OES_EGL_image_external_essl3 "
                       "required (es=%d)\n", gl->es);
        return -1;
    }
    p->program = build_copy_program(mapper);
    if (!p->program)
        return -1;

    gl->GenFramebuffers(1, &p->fbo);

    p->fmt = probe_2d_format(mapper);
    if (!p->fmt)
        return -1;

    p->have_fence = gl->FenceSync && gl->ClientWaitSync &&
                    gl->DeleteSync && gl->Flush;
    if (!p->have_fence)
        MP_WARN(mapper, "MKSURF: GL fences unavailable; lease retirement "
                        "unprotected\n");

    // Downstream declaration. The color identity is PROPAGATED from the
    // source MEDIACODEC frame: dst_params starts as a full copy of
    // src_params (color, repr levels/alpha, geometry, rotation, CSP flags),
    // so downstream color processing sees the actual input domain (plan
    // §5.3). Root cause of the diagnostic round 9 failure: the previous code
    // overrode color.primaries/transfer with bt.709/unknown, which mpv
    // normalized to bt.1886/SDR and the experiment whitelist rejected — for
    // real HLG sources too. Only the fields that genuinely change at this
    // import step are overridden below: the pixel format, the subformat, and
    // the sys/levels declaration of the identity FBO copy. The actual
    // sampling range is still a diagnostic open point (diag pending).
    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = p->dst_imgfmt;
    mapper->dst_params.hw_subfmt = IMGFMT_NONE;
    mapper->dst_params.repr.sys = PL_COLOR_SYSTEM_RGB;
    mapper->dst_params.repr.levels = PL_COLOR_LEVELS_FULL;

    MP_ERR(mapper, "MKSURF: unverified-declaration repr.sys=RGB "
                   "repr.levels=full\n");
    MP_ERR(mapper, "MKSURF: unverified-declaration color identity propagated "
                   "from source; sys/levels declaration unverified (diag "
                   "pending)\n");
    MP_ERR(mapper, "MKSURF: color-identity propagated primaries=%d "
                   "transfer=%d src_levels=%d\n",
            (int)mapper->dst_params.color.primaries,
            (int)mapper->dst_params.color.transfer,
            (int)mapper->src_params.repr.levels);
    MP_INFO(mapper, "MKSURF: capability oes=bridge-external copy=identity-fbo "
                    "pool=%d latch_timeout=%dms fence=%d version=%s\n",
            MKSURF_POOL_SIZE, MKSURF_LATCH_TIMEOUT_MS, p->have_fence,
            o->version());
    // Evidence visibility: see the version line above (the lab evidence
    // stream only passes through error level).
    MP_ERR(mapper, "MKSURF: active-importer=surfacetexture device_registered=1\n");

    return 0;
}

static bool lease_matches(const struct mkst_lease *lease, struct mp_image *src)
{
    // Identity = codec buffer pointer + pts, same key the release-once rule
    // uses (aimagereader redraw precedent).
    return lease->slot >= 0 && lease->source &&
           lease->source->planes[3] == src->planes[3] &&
           lease->src_pts == src->pts;
}

// Index of the retired lease matching src's identity, or -1.
static int retired_find(struct priv *p, struct mp_image *src)
{
    for (int i = 0; i < p->retired_count; i++)
        if (lease_matches(&p->retired[i], src))
            return i;
    return -1;
}

// Delete a fence exactly once and account it. Every GLsync created through
// fence_create() is released through this helper, so created/deleted counts
// must balance by the time the mapper is destroyed.
static void fence_delete(struct ra_hwdec_mapper *mapper, GLsync *fence)
{
    struct priv *p = mapper->priv;

    if (*fence) {
        p->gl->DeleteSync(*fence);
        *fence = NULL;
        p->fence_deleted_count++;
    }
}

// Create a retirement/copy fence. NULL return must fail the publishing map
// (no fenceless publication while GL fences are supported).
static GLsync fence_create(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    GLsync fence = p->gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence) {
        p->fence_created_count++;
        p->gl->Flush();
    } else {
        p->fence_create_failed_count++;
    }
    return fence;
}

// Release a lease's dynamic resources (fence + source ref). The slot texture
// stays in the pool. The lease is left marked empty.
static void lease_release(struct ra_hwdec_mapper *mapper, struct mkst_lease *lease)
{
    fence_delete(mapper, &lease->fence);
    if (lease->source)
        mp_image_unrefp(&lease->source);
    lease->slot = MKSURF_NO_SLOT;
    lease->src_pts = 0;
    lease->latch_ts = 0;
}

// Consumed-identity registry: bounded ring, key = planes[3] pointer + pts.
static bool consumed_find(struct priv *p, void *buffer, double pts)
{
    for (int i = 0; i < MKSURF_CONSUMED_SIZE; i++) {
        if (p->consumed[i].used &&
            p->consumed[i].buffer == buffer &&
            p->consumed[i].pts == pts)
            return true;
    }
    return false;
}

// Identity that was consumed once but evicted from the registry ring.
static bool evicted_find(struct priv *p, void *buffer, double pts)
{
    for (int i = 0; i < MKSURF_EVICTED_SIZE; i++) {
        if (p->evicted[i].used &&
            p->evicted[i].buffer == buffer &&
            p->evicted[i].pts == pts)
            return true;
    }
    return false;
}

// Register a frame as consumed. Key = planes[3] pointer + pts (double
// match), which also guards against decoder buffer-address reuse: a recycled
// pointer only aliases an entry if the new frame also carries the identical
// pts. Worst case of that aliasing (loop/seek streams that legitimately
// repeat pts on the same buffer address) is one dropped frame (map fails
// closed at the consumed-hit branch); the next map with a fresh pts proceeds.
//
// Ring eviction (overwriting the oldest entry) is steady-state behavior:
// live identities are bounded by decoder output queue (~8) + mpv frame queue
// (<=4) + lease pool (4) + retired (3) ~= 20 < 64 (see MKSURF_CONSUMED_SIZE),
// but the registry deliberately outlives its entries, so once the ring is
// full every new identity evicts the oldest: steady-state is about one
// eviction per frame (~30/s at 30fps). Evicted identities move to the
// graveyard ring (idempotent-reacquire recognition); the eviction counter
// lands in the uninit summary.
static void consumed_add(struct ra_hwdec_mapper *mapper, void *buffer, double pts)
{
    struct priv *p = mapper->priv;
    struct mkst_consumed *entry = &p->consumed[p->consumed_next];

    if (entry->used) {
        // Keep the evicted identity in the graveyard so a later reacquire of
        // it is recognized and logged loudly (idempotent-reacquire-release).
        struct mkst_consumed *grave = &p->evicted[p->evicted_next];
        grave->buffer = entry->buffer;
        grave->pts = entry->pts;
        grave->used = true;
        p->evicted_next = (p->evicted_next + 1) % MKSURF_EVICTED_SIZE;

        p->consumed_overflow_count++;
        // Steady-state behavior, not an error: with a 64-entry ring the
        // oldest entry is evicted on every add once the ring is full, i.e.
        // steady-state is about one eviction per frame (~30/s at 30fps;
        // run9 logged 2690 of these over 90 s). The evicted identity moves
        // to the graveyard above, so accounting stays closed. Debug level
        // only; the counter still lands in the uninit summary.
        MP_DBG(mapper, "MKSURF: consumed ring steady-state eviction "
                         "pts=%.9f (count=%"PRIu64")\n",
                entry->pts, p->consumed_overflow_count);
    }
    entry->buffer = buffer;
    entry->pts = pts;
    entry->used = true;
    p->consumed_next = (p->consumed_next + 1) % MKSURF_CONSUMED_SIZE;
}

static struct mkst_lease retired_take(struct priv *p, int index)
{
    struct mkst_lease lease = p->retired[index];
    for (int i = index + 1; i < p->retired_count; i++)
        p->retired[i - 1] = p->retired[i];
    p->retired[--p->retired_count] = (struct mkst_lease){.slot = MKSURF_NO_SLOT};
    return lease;
}

// Release retired leases until at most max_keep remain. Fence waits are
// bounded; a fence that is still stuck after the bound is recorded and the
// lease released anyway (GL texture deletion is safe with in-flight use; no
// glFinish is ever issued).
static void retire_reap(struct ra_hwdec_mapper *mapper, int max_keep)
{
    struct priv *p = mapper->priv;
    GL *gl = p->gl;

    while (p->retired_count > max_keep) {
        struct mkst_lease *lease = &p->retired[0];
        if (lease->fence) {
            GLenum status = gl->ClientWaitSync(lease->fence,
                                               GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            if (status != GL_ALREADY_SIGNALED &&
                status != GL_CONDITION_SATISFIED) {
                status = gl->ClientWaitSync(lease->fence,
                                            GL_SYNC_FLUSH_COMMANDS_BIT,
                                            MP_TIME_MS_TO_NS(MKSURF_REAP_TIMEOUT_MS));
                if (status != GL_ALREADY_SIGNALED &&
                    status != GL_CONDITION_SATISFIED) {
                    p->reap_stuck_count++;
                    MP_WARN(mapper, "MKSURF: retire fence stuck after %d ms; "
                                    "releasing anyway (stuck_count=%"PRIu64")\n",
                            MKSURF_REAP_TIMEOUT_MS, p->reap_stuck_count);
                }
            }
        }
        lease_release(mapper, lease);
        for (int i = 1; i < p->retired_count; i++)
            p->retired[i - 1] = p->retired[i];
        p->retired_count--;
        p->retired[p->retired_count] = (struct mkst_lease){.slot = MKSURF_NO_SLOT};
        p->reap_count++;
        MP_INFO(mapper, "MKSURF: reap retired=%d reaps=%"PRIu64"\n",
                p->retired_count, p->reap_count);
    }
}

// Take a retired lease back as the current mapping for a redraw of a frame
// whose codec buffer was already released (and possibly retired). Whatever
// the mapper currently holds is released first.
static void mapper_adopt_retired(struct ra_hwdec_mapper *mapper, int index)
{
    struct priv *p = mapper->priv;
    struct mkst_lease lease = retired_take(p, index);

    // The adopted lease's old retirement fence belongs to the finished
    // sampling round; delete it here (exactly once). The next unmap creates
    // a fresh sampling fence for the re-published lease.
    fence_delete(mapper, &lease.fence);

    if (p->cur.slot >= 0) {
        lease_release(mapper, &p->cur);
        mapper->tex[0] = NULL;
    }

    // Ownership of lease (incl. its source ref) transfers to cur.
    p->cur = lease;
    mapper->tex[0] = p->slots[lease.slot].tex;
}

// Find a free pool slot (not held by cur or any retired lease) and make sure
// its texture exists with the current source dimensions.
static int take_slot(struct ra_hwdec_mapper *mapper){
    struct priv *p = mapper->priv;
    int w = mapper->src_params.w;
    int h = mapper->src_params.h;

    for (int i = 0; i < MKSURF_POOL_SIZE; i++) {
        if (p->cur.slot == i)
            continue;
        bool in_retired = false;
        for (int j = 0; j < p->retired_count; j++)
            in_retired |= p->retired[j].slot == i;
        if (in_retired)
            continue;

        struct mkst_slot *s = &p->slots[i];
        if (s->tex && (s->w != w || s->h != h)) {
            ra_tex_free(mapper->ra, &s->tex);
            s->name = 0;
            s->w = s->h = 0;
        }
        if (!s->tex) {
            struct ra_tex_params params = {
                .dimensions = 2,
                .w = w,
                .h = h,
                .d = 1,
                .format = p->fmt,
                .render_src = true,
                .render_dst = true,
                .src_linear = true,
            };
            s->tex = ra_tex_create(mapper->ra, &params);
            if (!s->tex) {
                MP_ERR(mapper, "MKSURF: lease texture creation failed\n");
                return -1;
            }
            GLenum target = 0;
            ra_gl_get_raw_tex(mapper->ra, s->tex, &s->name, &target);
            s->w = w;
            s->h = h;
        }
        return i;
    }
    MP_ERR(mapper, "MKSURF: lease pool exhausted (retired=%d)\n", p->retired_count);
    return -1;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    if (mapper->src->imgfmt != IMGFMT_MEDIACODEC)
        return -1;

    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)mapper->src->planes[3];
    if (!buffer) {
        MP_ERR(mapper, "MKSURF: frame lacks AVMediaCodecBuffer (drop)\n");
        return -1;
    }
    // Fail closed on non-finite pts BEFORE any release: such a frame can
    // never be identity-matched nor sanity-checked later. The codec buffer
    // deliberately stays unconsumed on this error path (logged).
    double src_pts = mapper->src->pts;
    if (!isfinite(src_pts)) {
        p->nonfinite_pts_count++;
        MP_ERR(mapper, "MKSURF: nonfinite-pts pts=%.9f (drop; count=%"PRIu64"); "
                       "codec buffer left unreleased\n",
               src_pts, p->nonfinite_pts_count);
        return -1;
    }

    // Service decision, in strict order. Live-lease lookups must run BEFORE
    // the consumed-registry gate: a lease match means the frame is already
    // owned and must be served with zero release and zero latch, regardless
    // of registry state.
    //
    // a) Current lease (defensive: unmap normally runs before every map, so
    //    cur is usually empty here).
    if (p->cur.slot >= 0 && lease_matches(&p->cur, mapper->src)) {
        mapper->tex[0] = p->slots[p->cur.slot].tex;
        return 0;
    }
    // b) Retired lease: redraw of a frame whose codec buffer was already
    //    released exactly once; reuse the copy. Works with or without fence
    //    support (retired leases exist in both cases).
    int ridx = retired_find(p, mapper->src);
    if (ridx >= 0) {
        mapper_adopt_retired(mapper, ridx);
        MP_INFO(mapper, "MKSURF: redraw adopt-retired pts=%.9f "
                        "retired=%d reaps=%"PRIu64"\n",
                src_pts, p->retired_count, p->reap_count);
        return 0;
    }
    // c) Consumed but every lease is gone: the frame was released once and
    //    its copy is unrecoverable. No release, no latch, no publish.
    if (consumed_find(p, buffer, src_pts)) {
        p->horizon_overflow_count++;
        MP_ERR(mapper, "MKSURF: horizon-overflow consumed frame has no lease "
                       "(drop; overflow_count=%"PRIu64")\n",
               p->horizon_overflow_count);
        return -1;
    }

    // d) Fresh frame (or an idempotent reacquire after registry eviction):
    //    the one and only explicit release call site in this file, before
    //    the latch. The mp_image unref's implicit release is atomic-idempotent
    //    in FFmpeg. Real FFmpeg contract (libavcodec/mediacodec.c): returns 0
    //    on success (including a buffer already released once) and a negative
    //    value on failure.
    retire_reap(mapper, MKSURF_KEEP_RETIRED);
    // Re-check for a reusable lease right before the irreversible release.
    // The reap above cannot create matches (a/b just found none), but a
    // lease serve always beats an idempotent re-release, so verify once more.
    if (p->cur.slot >= 0 && lease_matches(&p->cur, mapper->src)) {
        mapper->tex[0] = p->slots[p->cur.slot].tex;
        return 0;
    }
    ridx = retired_find(p, mapper->src);
    if (ridx >= 0) {
        mapper_adopt_retired(mapper, ridx);
        MP_INFO(mapper, "MKSURF: redraw adopt-retired (pre-release recheck) "
                        "pts=%.9f retired=%d\n",
                src_pts, p->retired_count);
        return 0;
    }
    // Registry eviction graveyard: an identity that was consumed once, got
    // evicted from the 64-entry ring, and is now back with every lease gone.
    // The re-release below is idempotent at the codec level (FFmpeg atomic);
    // make it loud instead of silently treating it as a fresh frame.
    if (evicted_find(p, buffer, src_pts)) {
        p->idempotent_reacquire_count++;
        // Error level: a rare real event that must survive the error-only
        // lab evidence stream.
        MP_ERR(mapper, "MKSURF: idempotent-reacquire-release pts=%.9f "
                        "(consumed before, evicted from registry; codec-level "
                        "release-once held by FFmpeg atomic; count=%"PRIu64")\n",
                src_pts, p->idempotent_reacquire_count);
    }
    int rret = av_mediacodec_release_buffer(buffer, 1);
    p->release_count++;
    if (rret < 0) {
        p->release_failed_count++;
        MP_ERR(mapper, "MKSURF: release-failed ret=%d (drop, no latch/no "
                       "publish; failures=%"PRIu64")\n",
               rret, p->release_failed_count);
        // FFmpeg bumps the buffer's release state atomically before calling
        // the underlying release, so the buffer counts as consumed even
        // though the underlying call failed. Register it here too, or a
        // retry of this frame would issue a second explicit release call on
        // the same object.
        consumed_add(mapper, buffer, src_pts);
        return -1;
    }
    // Register the frame as consumed immediately: the release has happened
    // even if every later step of this map fails.
    consumed_add(mapper, buffer, src_pts);

    uint64_t ts_ns = 0;
    // Cold-start grace: the first latches get a longer bound while the
    // decoder and the bridge's main-Looper dispatch warm up. Timeout behavior
    // is identical (drop + consumed-registry entry); only the bound differs.
    int latch_timeout_ms = MKSURF_LATCH_TIMEOUT_MS;
    p->latch_attempts++;
    if (p->latch_attempts <= MKSURF_LATCH_GRACE_COUNT) {
        latch_timeout_ms = MKSURF_LATCH_GRACE_TIMEOUT_MS;
        if (p->latch_attempts == 1)
            MP_INFO(mapper, "MKSURF: latch cold-start grace active: %d ms "
                            "bound for the first %d latches, %d ms after\n",
                    MKSURF_LATCH_GRACE_TIMEOUT_MS, MKSURF_LATCH_GRACE_COUNT,
                    MKSURF_LATCH_TIMEOUT_MS);
    }
    int lret = o->latch(latch_timeout_ms, &ts_ns);
    if (lret != 0) {
        if (lret > 0) {
            p->latch_timeout_count++;
            MP_ERR(mapper, "MKSURF: latch timeout after %d ms (drop; "
                           "release_count=%"PRIu64" timeouts=%"PRIu64")\n",
                   latch_timeout_ms, p->release_count,
                   p->latch_timeout_count);
        } else {
            MP_ERR(mapper, "MKSURF: latch error %d (drop; "
                           "release_count=%"PRIu64")\n",
                   lret, p->release_count);
        }
        // No stale publish, no retirement changes on this path.
        return -1;
    }
    p->latch_count++;

    // Latch timestamp sanity (the bridge reports ns): reject only the absurd
    // bound. 0 is a legal timestamp (first frame with PTS 0 passes); values
    // at or beyond 2^53 (~104 days in ns) are sign-bit garbage from a
    // negative timestamp or a broken clock, and would lose precision as a
    // double. Either way this is treated as a latch failure.
    if (ts_ns >= (UINT64_C(1) << 53)) {
        p->latch_timeout_count++;
        MP_ERR(mapper, "MKSURF: latch-ts-invalid ts=%"PRIu64" (drop, counted "
                       "as latch failure; timeouts=%"PRIu64")\n",
               ts_ns, p->latch_timeout_count);
        return -1;
    }

    // Per-map stale/identity bound: |latch_ts - src_pts| beyond
    // max(1.5/fps, 0.1s) drops the frame. Stateless by design: no watermark,
    // no cross-map state; the next map is judged on its own inputs only.
    // mp_image.pts is a double in seconds; the SurfaceTexture timestamp is ns
    // (read from the bridge latch).
    double ts_s = (double)ts_ns / 1000000000.0;
    double delta = fabs(ts_s - src_pts);
    double fps = mapper->src->nominal_fps;
    double interval_term;
    if (fps > 0.0 && isfinite(fps)) {
        interval_term = 1.5 / fps;
    } else {
        interval_term = MKSURF_STALE_FPS_FALLBACK_S;
        if (!p->logged_fps_fallback) {
            p->logged_fps_fallback = true;
            MP_INFO(mapper, "MKSURF: nominal_fps invalid (fps=%.6f); stale "
                            "threshold interval term uses %.1fs fallback\n",
                    fps, MKSURF_STALE_FPS_FALLBACK_S);
        }
    }
    if (!isfinite(interval_term))
        interval_term = MKSURF_STALE_FPS_FALLBACK_S;
    double stale_threshold =
        interval_term > MKSURF_STALE_FLOOR_S ? interval_term
                                             : MKSURF_STALE_FLOOR_S;
    if (delta > stale_threshold) {
        p->stale_drop_count++;
        MP_ERR(mapper, "MKSURF: stale-drop delta=%.6fs threshold=%.6fs "
                       "latch_ts=%"PRIu64" src_pts=%.9f (drop; "
                       "stale_drops=%"PRIu64")\n",
               delta, stale_threshold, ts_ns, src_pts, p->stale_drop_count);
        return -1;
    }

    int slot = take_slot(mapper);
    if (slot < 0)
        return -1;
    if (!draw_copy(mapper, p->slots[slot].name, mapper->src_params.w,
                   mapper->src_params.h))
        return -1;

#if MKS_ST_DIAG_READBACK
    // Diagnostic readback, after the 2D copy completed and before publish.
    // Gate order: the one-strike unsupported flag first (a device without
    // working ReadPixels stops entering here after the first failed
    // attempt), then the bridge runtime switch, double-checked (pointer +
    // call).
    if (!p->diag_readback_unsupported && o->diag_enabled && o->diag_enabled())
        mkst_diag_readback(mapper, p->slots[slot].name, src_pts);
#endif

    p->cur = (struct mkst_lease) {
        .source = mp_image_new_ref(mapper->src),
        .src_pts = src_pts,
        .latch_ts = ts_ns,
        .fence = NULL,
        .slot = slot,
    };
    if (!p->cur.source) {
        p->cur.slot = MKSURF_NO_SLOT;
        MP_ERR(mapper, "MKSURF: mp_image_new_ref failed (drop; "
                       "release_count=%"PRIu64")\n", p->release_count);
        return -1;
    }
    // A published lease must carry a fence; failure fails this map (the
    // frame stays consumed, no publish happens).
    if (p->have_fence) {
        p->cur.fence = fence_create(mapper);
        if (!p->cur.fence) {
            MP_ERR(mapper, "MKSURF: fence-create-failed at publish (drop; "
                           "failures=%"PRIu64")\n",
                   p->fence_create_failed_count);
            lease_release(mapper, &p->cur);
            return -1;
        }
    }

    int in_use = p->retired_count + 1;
    if (in_use > (int)p->pool_peak)
        p->pool_peak = in_use;

    mapper->tex[0] = p->slots[slot].tex;

    MP_INFO(mapper, "MKSURF: frame latch_ts=%"PRIu64"ns src_pts=%.9f delta=%.6fs "
                    "release_count=%"PRIu64" pool=%d/%"PRIu64" fence=%d\n",
            ts_ns, src_pts, delta, p->release_count, in_use, p->pool_peak,
            (int)(p->cur.fence != NULL));

    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    if (p->cur.slot < 0)
        return;

    // Swap the lease fence: delete the publish-time copy fence (exactly
    // once, before the field is overwritten), then fence the renderer's
    // sampling of this lease so a later reuse of the same pool texture waits
    // until that sampling completed.
    fence_delete(mapper, &p->cur.fence);
    if (p->have_fence) {
        p->cur.fence = fence_create(mapper);
        if (!p->cur.fence)
            MP_WARN(mapper, "MKSURF: fence-create-failed at retire; lease "
                            "retires unprotected (failures=%"PRIu64")\n",
                    p->fence_create_failed_count);
    }
    if (p->retired_count >= (int)MP_ARRAY_SIZE(p->retired))
        retire_reap(mapper, (int)MP_ARRAY_SIZE(p->retired) - 1);
    mp_assert(p->retired_count < (int)MP_ARRAY_SIZE(p->retired));

    p->retired[p->retired_count++] = p->cur;
    p->cur = (struct mkst_lease){.slot = MKSURF_NO_SLOT};
    mapper->tex[0] = NULL;
    p->retire_count++;

    MP_INFO(mapper, "MKSURF: retire pts=%.9f retired=%d retires=%"PRIu64
                    " release_count=%"PRIu64"\n",
            p->retired[p->retired_count - 1].src_pts, p->retired_count,
            p->retire_count, p->release_count);
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = p->gl;

    // Fences first, then textures (spec §2.9 ordering).
    if (p->have_fence)
        retire_reap(mapper, 0);
    for (int i = 0; i < p->retired_count; i++)
        lease_release(mapper, &p->retired[i]);
    p->retired_count = 0;
    if (p->cur.slot >= 0) {
        lease_release(mapper, &p->cur);
        mapper->tex[0] = NULL;
    }

    for (int i = 0; i < MKSURF_POOL_SIZE; i++) {
        if (p->slots[i].tex)
            ra_tex_free(mapper->ra, &p->slots[i].tex);
        p->slots[i] = (struct mkst_slot){0};
    }

    if (p->fbo)
        gl->DeleteFramebuffers(1, &p->fbo);
    p->fbo = 0;
    if (p->program)
        gl->DeleteProgram(p->program);
    p->program = 0;

    // Drop only the wrapped view: the OES GL name is deleted at the owner
    // level (hwdec uninit) so mapper rebuilds always find it alive.
    if (p->oes_tex)
        ra_tex_free(mapper->ra, &p->oes_tex);
    p->oes_tex = NULL;
    p->oes_tex_name = 0;

    // Fence lifecycle audit: every fence must have been deleted exactly once.
    if (p->fence_created_count != p->fence_deleted_count)
        MP_WARN(mapper, "MKSURF: fence accounting unbalanced created=%"PRIu64
                        " deleted=%"PRIu64" create_failed=%"PRIu64"\n",
                p->fence_created_count, p->fence_deleted_count,
                p->fence_create_failed_count);

    // Closeout telemetry at error level: the lab evidence stream only passes
    // through error level, so this uninit summary must not use info.
    MP_ERR(mapper, "MKSURF: mapper destroyed release_count=%"PRIu64
                    " release_failed=%"PRIu64" consumed_overflow=%"PRIu64
                    " horizon_overflow=%"PRIu64" idempotent_reacquires=%"PRIu64
                    " nonfinite_pts=%"PRIu64
                    " latches=%"PRIu64" timeouts=%"PRIu64
                    " stale_drops=%"PRIu64
                    " retires=%"PRIu64" reaps=%"PRIu64" reap_stuck=%"PRIu64
                    " fences=%"PRIu64"/%"PRIu64" pool_peak=%"PRIu64"\n",
            p->release_count, p->release_failed_count,
            p->consumed_overflow_count,
            p->horizon_overflow_count, p->idempotent_reacquire_count,
            p->nonfinite_pts_count,
            p->latch_count, p->latch_timeout_count,
            p->stale_drop_count,
            p->retire_count, p->reap_count, p->reap_stuck_count,
            p->fence_created_count, p->fence_deleted_count,
            p->pool_peak);
}

const struct ra_hwdec_driver ra_hwdec_surfacetexture = {
    .name = "surfacetexture",
    .priv_size = sizeof(struct priv_owner),
    .imgfmts = {IMGFMT_MEDIACODEC, 0},
    .device_type = AV_HWDEVICE_TYPE_MEDIACODEC,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
