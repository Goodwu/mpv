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
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libplacebo/config.h>

#ifdef PL_HAVE_D3D11
#include <libplacebo/d3d11.h>
#endif

#ifdef PL_HAVE_OPENGL
#include <libplacebo/opengl.h>
#endif

#include "context.h"
#include "config.h"
#include "common/common.h"
#if HAVE_ANDROID
#include <sys/system_properties.h>
#include <dlfcn.h>
#endif
#include "options/m_config.h"
#include "video/out/placebo/utils.h"
#include "video/out/gpu/video.h"

#if HAVE_D3D11
#include "osdep/windows_utils.h"
#include "video/out/d3d11/ra_d3d11.h"
#include "video/out/d3d11/context.h"
#endif

#if HAVE_GL
#include "video/out/opengl/context.h"
#include "video/out/opengl/ra_gl.h"
# if HAVE_EGL || HAVE_EGL_ANDROID
#include <EGL/egl.h>
# endif
#endif

#if HAVE_VULKAN
#include "video/out/vulkan/context.h"
#endif

#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_ANDROID && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
#include "video/out/opengl/mks_yuv_compare.h"

// EGL 1.4 does not require eglGetProcAddress to expose core EGL symbols. The
// explicit capture loader can resolve missing core functions from linked EGL;
// ordinary creation retains mpv's original get_proc_addr callback unchanged.
static pl_voidfunc_t yuv_compare_get_proc(void *priv, const char *name)
{
    struct GL *gl = priv;
    void *fn = gl->get_fn(gl->fn_ctx, name);
    if (!fn && !strncmp(name, "egl", 3))
        fn = dlsym(RTLD_DEFAULT, name);
    return (pl_voidfunc_t)fn;
}

static bool configure_yuv_compare_egl(struct gpu_ctx *ctx,
                                     struct pl_opengl_params *params,
                                     struct GL *gl, bool diagnostic_compare)
{
    if (!diagnostic_compare)
        return true;
    // HAVE_EGL can be false even though Android/EGL is active. Only
    // capture explicitly enables this owner/loader contract; default
    // Android GPU capabilities and imports retain their old behavior.
    if (!gl->get_fn || !mks_android_yuv_compare_owner_current(ctx->ra_ctx) ||
        eglGetCurrentDisplay() == EGL_NO_DISPLAY ||
        eglGetCurrentContext() == EGL_NO_CONTEXT ||
        eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE ||
        eglGetCurrentSurface(EGL_READ) == EGL_NO_SURFACE)
    {
        MP_ERR(ctx, "MKS-YUV-COMPARE event=egl-owner-rejected before-lib-create=1\n");
        return false;
    }
    params->egl_display = eglGetCurrentDisplay();
    params->egl_context = eglGetCurrentContext();
    params->get_proc_addr_ex = yuv_compare_get_proc;
    params->proc_ctx = gl;
#ifdef PL_HAVE_MKS_YUV_COMPARE_CAPS
    params->diagnostic_yuv_compare_caps = true;
#endif
    MP_ERR(ctx, "MKS-YUV-COMPARE event=egl-owner-configured android-current-verified=1 egl-loader-requested=1\n");
    return true;
}
#endif

#if HAVE_D3D11
static bool d3d11_pl_init(struct vo *vo, struct gpu_ctx *ctx,
                          struct ra_ctx_opts *ctx_opts)
{
#if !defined(PL_HAVE_D3D11)
    MP_MSG(ctx, vo->probing ? MSGL_V : MSGL_ERR,
           "libplacebo was built without D3D11 support.\n");
    return false;
#else // defined(PL_HAVE_D3D11)
    bool success = false;

    ID3D11Device   *device    = ra_d3d11_get_device(ctx->ra_ctx->ra);
    IDXGISwapChain *swapchain = ra_d3d11_ctx_get_swapchain(ctx->ra_ctx);
    if (!device || !swapchain) {
        mp_err(ctx->log,
               "Failed to receive required components from the mpv d3d11 "
               "context! (device: %s, swap chain: %s)\n",
               device    ? "OK" : "failed",
               swapchain ? "OK" : "failed");
        goto err_out;
    }

    pl_d3d11 d3d11 = pl_d3d11_create(ctx->pllog,
        pl_d3d11_params(
            .device = device,
        )
    );
    if (!d3d11) {
        mp_err(ctx->log, "Failed to acquire a d3d11 libplacebo context!\n");
        goto err_out;
    }
    ctx->gpu = d3d11->gpu;

    mppl_log_set_probing(ctx->pllog, false);

    ctx->swapchain = pl_d3d11_create_swapchain(d3d11,
        pl_d3d11_swapchain_params(
            .swapchain = swapchain,
            .disable_10bit_sdr = ra_d3d11_ctx_prefer_8bit_output_format(ctx->ra_ctx),
        )
    );
    if (!ctx->swapchain) {
        mp_err(ctx->log, "Failed to acquire a d3d11 libplacebo swap chain!\n");
        goto err_out;
    }

    success = true;

err_out:
    SAFE_RELEASE(swapchain);
    SAFE_RELEASE(device);

    return success;
#endif // defined(PL_HAVE_D3D11)
}
#endif // HAVE_D3D11

struct gpu_ctx *gpu_ctx_create(struct vo *vo, struct ra_ctx_opts *ctx_opts)
{
    struct gpu_ctx *ctx = talloc_zero(NULL, struct gpu_ctx);
    ctx->log = vo->log;
    ctx->ra_ctx = ra_ctx_create(vo, *ctx_opts);
    if (!ctx->ra_ctx)
        goto err_out;

#if HAVE_VULKAN
    struct mpvk_ctx *vkctx = ra_vk_ctx_get(ctx->ra_ctx);
    if (vkctx) {
        ctx->pllog = vkctx->pllog;
        ctx->gpu = vkctx->gpu;
        ctx->swapchain = vkctx->swapchain;
        return ctx;
    }
#endif

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        goto err_out;

    mppl_log_set_probing(ctx->pllog, vo->probing);

#if HAVE_D3D11
    if (ra_is_d3d11(ctx->ra_ctx->ra)) {
        if (!d3d11_pl_init(vo, ctx, ctx_opts))
            goto err_out;

        return ctx;
    }
#endif

#if HAVE_GL && defined(PL_HAVE_OPENGL)
    if (ra_is_gl(ctx->ra_ctx->ra)) {
        struct GL *gl = ra_gl_get(ctx->ra_ctx->ra);
#if HAVE_ANDROID && (defined(PL_HAVE_MKS_YUV_COMPARE) || defined(PL_HAVE_MKS_YUV_ABORT_FRAME))
        char yuv_compare[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.mks.yuv_compare", yuv_compare);
        bool compare_session = !strcmp(yuv_compare, "1");
        bool diagnostic_compare = compare_session;
        // The UV table is an opt-in (user ruling 2026-11-02: untabled
        // fusion is the default). When opted in it seeds through the same
        // atlas machinery and needs this capability cache, without opening
        // any compare transaction. An unsupported owner/loader contract must
        // not break playback: degrade to no capability cache (seeds then
        // fail arming and the failure budget disables the table for the
        // session). An explicit compare session keeps the strict failure
        // contract.
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
        char fusion_table[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.mks.yuv_fusion_table", fusion_table);
        if (!diagnostic_compare && !strcmp(fusion_table, "1"))
            diagnostic_compare = true;
#endif
#endif
        struct pl_opengl_params params = *pl_opengl_params(
            .debug = ctx_opts->debug,
            .allow_software = ctx_opts->allow_sw,
            .get_proc_addr_ex = (void *) gl->get_fn,
            .proc_ctx = gl->fn_ctx,
        );
#ifdef PL_HAVE_MKS_DISABLE_TIMERS
        // M2' frame-timing owner: freeze the timing mode and disable all LP
        // internal timers when the owner will be active in this context.
        {
            char fgt_mode[PROP_VALUE_MAX] = {0};
            bool fgt_active = __system_property_get("debug.mks.gpu_timing",
                                                     fgt_mode) > 0 &&
                (!strcmp(fgt_mode, "frame") || !strcmp(fgt_mode, "frame-span") ||
                 !strcmp(fgt_mode, "frame-count"));
            params.mks_disable_timers = fgt_active;
            MP_ERR(ctx, "MKS-FGPU-LP-DISABLE prop=%s active=%d\n",
                   fgt_mode[0] ? fgt_mode : "<unset>", fgt_active ? 1 : 0);
        }
#endif
# if HAVE_EGL
        params.egl_display = eglGetCurrentDisplay();
        params.egl_context = eglGetCurrentContext();
# endif
#if HAVE_ANDROID && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
        if (diagnostic_compare &&
            !configure_yuv_compare_egl(ctx, &params, gl, diagnostic_compare)) {
            if (compare_session)
                goto err_out;
            MP_ERR(ctx, "MKS-FUSION-TABLE event=caps-unavailable playback-continues\n");
            diagnostic_compare = false;
        }
#endif
        pl_opengl opengl = pl_opengl_create(ctx->pllog, &params);
        if (!opengl)
            goto err_out;
        ctx->gpu = opengl->gpu;

        mppl_log_set_probing(ctx->pllog, false);

        ctx->swapchain = pl_opengl_create_swapchain(opengl, pl_opengl_swapchain_params(
            .max_swapchain_depth = vo->opts->swapchain_depth,
            .framebuffer.flipped = gl->flipped,
#if HAVE_ANDROID && defined(PL_HAVE_MKS_YUV_ABORT_FRAME)
            .diagnostic_abort_frame_no_gl = diagnostic_compare,
#endif
#if HAVE_EGL_ANDROID
            // MKS YUV diag (lab-gated, default off): when THIS Android EGL
            // context took the YUV diag branch, point the libplacebo
            // swapchain at its per-context high-precision RGB offscreen FBO
            // instead of the YUV default framebuffer (RGB writes to a YUV
            // target would be undefined). The final layout(yuv) pass to the
            // YUV window runs at swap time in context_android.c. Returns 0
            // (default framebuffer) for any context that did not take the
            // branch, so this is a no-op for every non-diag build/run.
            .framebuffer.id = mks_yuv_diag_swapchain_fbo(ctx->ra_ctx),
#endif
        ));
        if (!ctx->swapchain)
            goto err_out;

        return ctx;
    }
#elif HAVE_GL
    if (ra_is_gl(ctx->ra_ctx->ra)) {
        MP_MSG(ctx, vo->probing ? MSGL_V : MSGL_ERR,
            "libplacebo was built without OpenGL support.\n");
    }
#endif

err_out:
    gpu_ctx_destroy(&ctx);
    return NULL;
}

bool gpu_ctx_resize(struct gpu_ctx *ctx, int w, int h)
{
#if HAVE_VULKAN
    if (ra_vk_ctx_get(ctx->ra_ctx))
        // vulkan RA handles this by itself
        return true;
#endif

    return pl_swapchain_resize(ctx->swapchain, &w, &h);
}

void gpu_ctx_destroy(struct gpu_ctx **ctxp)
{
    struct gpu_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    if (!ctx->ra_ctx)
        goto skip_common_pl_cleanup;

#if HAVE_VULKAN
    if (ra_vk_ctx_get(ctx->ra_ctx))
        // vulkan RA context handles pl cleanup by itself,
        // skip common local clean-up.
        goto skip_common_pl_cleanup;
#endif

    if (ctx->swapchain)
        pl_swapchain_destroy(&ctx->swapchain);

    if (ctx->gpu) {
#if HAVE_GL && defined(PL_HAVE_OPENGL)
        if (ra_is_gl(ctx->ra_ctx->ra)) {
            pl_opengl opengl = pl_opengl_get(ctx->gpu);
            pl_opengl_destroy(&opengl);
        }
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
        if (ra_is_d3d11(ctx->ra_ctx->ra)) {
            pl_d3d11 d3d11 = pl_d3d11_get(ctx->gpu);
            pl_d3d11_destroy(&d3d11);
        }
#endif
    }

    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);

skip_common_pl_cleanup:
    ra_ctx_destroy(&ctx->ra_ctx);

    talloc_free(ctx);
    *ctxp = NULL;
}
