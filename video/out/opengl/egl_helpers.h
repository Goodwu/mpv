#ifndef MP_GL_EGL_HELPERS_H
#define MP_GL_EGL_HELPERS_H

#include <stdbool.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "video/out/gpu/context.h"

struct mp_log;

bool mpegl_create_context(struct ra_ctx *ctx, EGLDisplay display,
                          EGLContext *out_context, EGLConfig *out_config);

struct mpegl_cb {
    // if set, pick the desired config from the given list and return its index
    // defaults to 0 (they are sorted by eglChooseConfig). return a negative
    // number to indicate an error condition or that no suitable configs could
    // be found.
    int (*refine_config)(void *user_data, EGLConfig *configs, int num_configs);
    void *user_data;
    // MKS YUV diag (lab-gated, default off): select the fixed 8-bit NV12 YUV
    // EGLConfig (EGL_NATIVE_VISUAL_ID 0x7FA30C04, config_id 49 family) instead
    // of any RGB config and assert its full YUV attribute set at runtime.
    // Mutually exclusive with the exact RGB bit matching of output-format
    // (rgb10_a2 et al). Assertion failure is a hard failure: no config
    // fallback, the whole context creation fails.
    bool yuv_diag_config;
};

bool mpegl_create_context_cb(struct ra_ctx *ctx, EGLDisplay display,
                             struct mpegl_cb cb, EGLContext *out_context,
                             EGLConfig *out_config);

struct GL;
void mpegl_load_functions(struct GL *gl, struct mp_log *log);

EGLDisplay mpegl_get_display(EGLenum platform, const char *platform_ext_name,
                             void *native_display);

EGLSurface mpegl_create_window_surface(EGLDisplay dpy, EGLConfig config,
                                       void *native_window);

#endif
