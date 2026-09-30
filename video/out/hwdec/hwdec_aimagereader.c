/*
 * Copyright (c) 2021 sfan5 <sfan5@live.de>
 *
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

#include "config.h"

#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <media/NdkImageReader.h>
#include <android/native_window_jni.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "misc/jni.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "video/out/gpu/hwdec.h"
#include "video/mp_image.h"
#include "video/out/opengl/ra_gl.h"

typedef void *GLeglImageOES;
typedef void *EGLImageKHR;
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#ifndef GL_TEXTURE_BINDING_EXTERNAL_OES
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif

// GLES external YUV samplers on this device's driver effectively normalize
// buffer code values as code/1020 instead of the code/1023 the Dolby Vision
// reshape domain assumes, so the reshaper input arrives inflated by 1023/1020.
// This is a property of the driver's Y2Y sampling, not of the buffer format
// itself: 0x325 is the vendor Main10 (10-bit) layout this device's
// OMX.hisi.video.decoder.hevc surface output always uses (not an 8-bit
// downgrade), while 0x23/YV12 are true 8-bit formats whose codes sit at x4
// in the same sampling domain. Whether other drivers behave the same is
// unverified; unlisted formats get k=1 (no correction).
static const struct { uint32_t format; float k; } ahb_yuv_scales[] = {
    { 0x325,                  1023.0f / 1020.0f },
    { 0x23,                   1023.0f / 1020.0f },
    { 0x32315659u /* YV12 */, 1023.0f / 1020.0f },
};

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    AImageReader *reader;
    jobject surface;
    void *lib_handle;
    // NDK callbacks may already be queued when a listener is unregistered.
    // Keep their context alive until the reader (and its looper) is deleted.
    mp_mutex callback_lock;
    mp_cond callback_cond;
    struct priv *callback_mapper;
    unsigned callbacks_active;
    bool callback_state_initialized;

    media_status_t (*AImageReader_newWithUsage)(
        int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
    media_status_t (*AImageReader_getWindow)(
        AImageReader *, ANativeWindow **);
    media_status_t (*AImageReader_setImageListener)(
        AImageReader *, AImageReader_ImageListener *);
    media_status_t (*AImageReader_acquireLatestImage)(AImageReader *, AImage **);
    void (*AImageReader_delete)(AImageReader *);
    media_status_t (*AImage_getHardwareBuffer)(const AImage *, AHardwareBuffer **);
    media_status_t (*AImage_getCropRect)(const AImage *, AImageCropRect *);
    media_status_t (*AImage_getTimestamp)(const AImage *, int64_t *);
    void (*AImage_delete)(AImage *);
    void (*AHardwareBuffer_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    jobject (*ANativeWindow_toSurface)(JNIEnv *, ANativeWindow *);
};

struct retired_image {
    struct mp_image *source;
    AImage *image;
    EGLImageKHR egl_image;
    EGLDisplay display;
    GLsync fence;
    GLuint texture;
    struct ra_tex *wrapped_tex;
};

struct priv {
    struct mp_log *log;

    GLuint gl_texture;
    bool direct_yuv;
    bool direct_retire;
    // Retire-and-fence protection for every mapped codec buffer, not just
    // the P5 direct YUV path. Without it the standard OES import releases
    // the AImage at unmap while GL may still be sampling it; the decoder
    // then rewrites the buffer mid-scan, visible as tearing (a diagonal
    // split) and green blocks on drivers without implicit AHardwareBuffer
    // synchronization (observed on Adreno 512).
    bool buffer_retire;
    GLsync sample_fence;
    struct retired_image retired[2];
    int retired_count;
    // External-sampling domain correction for the dovi reshape, evaluated
    // once per mapper from the first mapped AHardwareBuffer's format.
    // Per-mapper (not process-global): a mapper rebuilt for the next file
    // must re-evaluate it.
    bool ahb_format_checked;
    float dovi_rescale_k;
    // Identity (codec buffer + pts) of the last frame whose dovi metadata
    // was rescaled, so map retries and redraws never apply the correction
    // twice. Keying on the mp_image pointer would be unreliable: remapping
    // allocates a fresh struct, and freed structs get their addresses
    // recycled for the next frame.
    void *dovi_rescale_last_buffer;
    double dovi_rescale_last_pts;
    bool sample_submitted;
    AImage *image;
    EGLImageKHR egl_image;

    mp_mutex lock;
    mp_cond cond;
    bool image_available;

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(
        EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLClientBuffer (EGLAPIENTRY *GetNativeClientBufferANDROID)(
        const struct AHardwareBuffer *);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
};

static const struct { const char *symbol; int offset; } lib_functions[] = {
    { "AImageReader_newWithUsage", offsetof(struct priv_owner, AImageReader_newWithUsage) },
    { "AImageReader_getWindow", offsetof(struct priv_owner, AImageReader_getWindow) },
    { "AImageReader_setImageListener", offsetof(struct priv_owner, AImageReader_setImageListener) },
    { "AImageReader_acquireLatestImage", offsetof(struct priv_owner, AImageReader_acquireLatestImage) },
    { "AImageReader_delete", offsetof(struct priv_owner, AImageReader_delete) },
    { "AImage_getHardwareBuffer", offsetof(struct priv_owner, AImage_getHardwareBuffer) },
    { "AImage_delete", offsetof(struct priv_owner, AImage_delete) },
    { "AHardwareBuffer_describe", offsetof(struct priv_owner, AHardwareBuffer_describe) },
    { "ANativeWindow_toSurface", offsetof(struct priv_owner, ANativeWindow_toSurface) },
    { NULL, 0 },
};

// Optional entry points; their absence only disables the P5 direct YUV path.
static const struct { const char *symbol; int offset; } lib_functions_optional[] = {
    { "AImage_getCropRect", offsetof(struct priv_owner, AImage_getCropRect) },
    { "AImage_getTimestamp", offsetof(struct priv_owner, AImage_getTimestamp) },
    { NULL, 0 },
};


static AVBufferRef *create_mediacodec_device_ref(jobject surface)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->surface = surface;

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static bool load_lib_functions(struct priv_owner *p, struct mp_log *log)
{
    p->lib_handle = dlopen("libmediandk.so", RTLD_NOW | RTLD_GLOBAL);
    if (!p->lib_handle)
        return false;
    for (int i = 0; lib_functions[i].symbol; i++) {
        const char *sym = lib_functions[i].symbol;
        void *fun = dlsym(p->lib_handle, sym);
        if (!fun)
            fun = dlsym(RTLD_DEFAULT, sym);
        if (!fun) {
            mp_warn(log, "Could not resolve symbol %s\n", sym);
            return false;
        }

        *(void **) ((uint8_t*)p + lib_functions[i].offset) = fun;
    }
    for (int i = 0; lib_functions_optional[i].symbol; i++) {
        void *fun = dlsym(p->lib_handle, lib_functions_optional[i].symbol);
        if (!fun)
            fun = dlsym(RTLD_DEFAULT, lib_functions_optional[i].symbol);
        *(void **) ((uint8_t*)p + lib_functions_optional[i].offset) = fun;
    }
    return true;
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    mp_mutex_init(&p->callback_lock);
    mp_cond_init(&p->callback_cond);
    p->callback_state_initialized = true;

    if (!ra_is_gl(hw->ra_ctx->ra))
        return -1;
    if (!eglGetCurrentContext())
        return -1;

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!gl_check_extension(exts, "EGL_ANDROID_image_native_buffer"))
        return -1;

    JNIEnv *env = MP_JNI_GET_ENV(hw);
    if (!env)
        return -1;

    if (!load_lib_functions(p, hw->log))
        return -1;

    static const char *es2_exts[] = {"GL_OES_EGL_image_external", 0};
    static const char *es3_exts[] = {"GL_OES_EGL_image_external_essl3", 0};
    GL *gl = ra_gl_get(hw->ra_ctx->ra);
    if (gl_check_extension(gl->extensions, es3_exts[0]))
        hw->glsl_extensions = es3_exts;
    else
        hw->glsl_extensions = es2_exts;

    // dummy dimensions, AImageReader only transports hardware buffers.
    // Retire protection holds one image beyond the frame being rendered
    // (plus one for an adjacent-frame redraw), so three slots are tight but
    // sufficient; the decoder-side frame request is clamped accordingly
    // (get_req_frames). Requesting more slots (4-5) sounds safer but breaks
    // codec start on vendor OMX decoders: OMX.hisi rejects the larger output
    // buffer count demanded by the BufferQueue ("port(1) BufferCount error",
    // followed by signalError on the output-port transition), observed on
    // LYA-AL00 / Kirin 980. Keep 3 unless per-vendor limits are probed.
    media_status_t ret = p->AImageReader_newWithUsage(16, 16,
        AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        3, &p->reader);
    if (ret != AMEDIA_OK) {
        MP_ERR(hw, "newWithUsage failed: %d\n", ret);
        return -1;
    }
    mp_assert(p->reader);

    ANativeWindow *window;
    ret = p->AImageReader_getWindow(p->reader, &window);
    if (ret != AMEDIA_OK) {
        MP_ERR(hw, "getWindow failed: %d\n", ret);
        return -1;
    }
    mp_assert(window);

    jobject surface = p->ANativeWindow_toSurface(env, window);
    p->surface = (*env)->NewGlobalRef(env, surface);
    (*env)->DeleteLocalRef(env, surface);

    p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = hw->driver->name,
        .av_device_ref = create_mediacodec_device_ref(p->surface),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(hw, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(hw->devs, &p->hwctx);

    return 0;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    if (p->surface) {
        JNIEnv *env = MP_JNI_GET_ENV(hw);
        mp_assert(env);
        (*env)->DeleteGlobalRef(env, p->surface);
        p->surface = NULL;
    }

    if (p->reader) {
        p->AImageReader_delete(p->reader);
        p->reader = NULL;
    }

    if (p->callback_state_initialized) {
        mp_cond_destroy(&p->callback_cond);
        mp_mutex_destroy(&p->callback_lock);
        p->callback_state_initialized = false;
    }

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);

    if (p->lib_handle) {
        dlclose(p->lib_handle);
        p->lib_handle = NULL;
    }
}

static void image_callback(void *context, AImageReader *reader)
{
    struct priv_owner *o = context;
    mp_mutex_lock(&o->callback_lock);
    struct priv *p = o->callback_mapper;
    if (p)
        o->callbacks_active++;
    mp_mutex_unlock(&o->callback_lock);
    if (!p)
        return;

    mp_mutex_lock(&p->lock);
    p->image_available = true;
    mp_cond_signal(&p->cond);
    mp_mutex_unlock(&p->lock);

    mp_mutex_lock(&o->callback_lock);
    mp_assert(o->callbacks_active > 0);
    if (--o->callbacks_active == 0)
        mp_cond_signal(&o->callback_cond);
    mp_mutex_unlock(&o->callback_lock);
}

static void configure_external_texture(GL *gl, bool direct_yuv)
{
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER,
                      direct_yuv ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER,
                      direct_yuv ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool create_next_external_texture(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    GLint old_active = 0, old_external = 0;
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_external);
    gl->GenTextures(1, &p->gl_texture);
    if (!p->gl_texture) {
        gl->ActiveTexture(old_active);
        return false;
    }
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    configure_external_texture(gl, p->direct_yuv);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_external);
    gl->ActiveTexture(old_active);
    if (gl->GetError() != GL_NO_ERROR) {
        gl->DeleteTextures(1, &p->gl_texture);
        p->gl_texture = 0;
        return false;
    }
    return true;
}

static bool wrap_external_texture(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = ra_find_unorm_format(mapper->ra, 1, 4),
        .render_src = true,
        .src_linear = true,
        .external_oes = true,
        .external_yuv = p->direct_yuv,
    };
    if (!params.format || params.format->ctype != RA_CTYPE_UNORM)
        return false;
    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
    return mapper->tex[0] != NULL;
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    p->log = mapper->log;
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->cond);

    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    p->GetNativeClientBufferANDROID =
        (void *)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->GetNativeClientBufferANDROID || !p->EGLImageTargetTexture2DOES)
        return -1;

    // Direct raw Y/Cb/Cr sampling of the P5 BL buffer requires GLES3
    // YUV_target, the crop/timestamp entry points and the libplacebo
    // external YUV sampler (HAVE_PL_EXTERNAL_YUV); without any of these
    // fall back to the standard OES import.
#if HAVE_PL_EXTERNAL_YUV
    p->direct_yuv = mapper->src_params.dv_profile == 5 &&
                    mapper->src_params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION &&
                    mapper->src_params.repr.dovi;
    if (p->direct_yuv && (gl->es < 300 ||
                          !gl_check_extension(gl->extensions, "GL_EXT_YUV_target") ||
                          !o->AImage_getCropRect || !o->AImage_getTimestamp)) {
        MP_VERBOSE(mapper, "P5 direct YUV unavailable; falling back to standard OES import\n");
        p->direct_yuv = false;
    }
#endif
    p->buffer_retire = gl->FenceSync && gl->ClientWaitSync &&
                       gl->DeleteSync && gl->Flush;
    p->direct_retire = p->direct_yuv && p->buffer_retire;

    AImageReader_ImageListener listener = {
        .context = o,
        .onImageAvailable = image_callback,
    };
    mp_mutex_lock(&o->callback_lock);
    mp_assert(!o->callback_mapper);
    o->callback_mapper = p;
    mp_mutex_unlock(&o->callback_lock);
    media_status_t listener_ret =
        o->AImageReader_setImageListener(o->reader, &listener);
    if (listener_ret != AMEDIA_OK) {
        mp_mutex_lock(&o->callback_lock);
        o->callback_mapper = NULL;
        mp_mutex_unlock(&o->callback_lock);
        MP_ERR(mapper, "AImageReader_setImageListener failed: %d\n", listener_ret);
        return -1;
    }
    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = p->direct_yuv ? IMGFMT_MEDIACODEC_YUV : IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;
    if (p->direct_yuv) {
        // External YUV sampling preserves normalized 10-bit BL code values.
        // (The descriptor in img_format.c claims 8-bit comps on purpose:
        // the format only describes the sampler's normalized input domain,
        // not the buffer's 10-bit precision.)
        mapper->dst_params.repr.bits = (struct pl_bit_encoding) {
            .sample_depth = 10, .color_depth = 10,
        };
        MP_VERBOSE(mapper, "P5 direct external YUV sampler enabled\n");
    }

    if (!create_next_external_texture(mapper) || !wrap_external_texture(mapper))
        return -1;

    return 0;
}

static struct retired_image retired_take(struct priv *p, int index)
{
    struct retired_image image = p->retired[index];
    for (int i = index + 1; i < p->retired_count; i++)
        p->retired[i - 1] = p->retired[i];
    p->retired[--p->retired_count] = (struct retired_image){0};
    return image;
}

static void retire_release(struct ra_hwdec_mapper *mapper, int index)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);
    struct retired_image image = retired_take(p, index);
    gl->DeleteSync(image.fence);
    if (image.wrapped_tex)
        ra_tex_free(mapper->ra, &image.wrapped_tex);
    gl->DeleteTextures(1, &image.texture);
    p->DestroyImageKHR(image.display, image.egl_image);
    o->AImage_delete(image.image);
    mp_image_unrefp(&image.source);
}

// Release retired images until at most max_keep remain. A fence that does
// not signal within roughly a frame period is treated as stuck; glFinish()
// is a full completion barrier either way, so waiting longer only stalls
// the render thread.
static void retire_reap(struct ra_hwdec_mapper *mapper, int max_keep)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    while (p->retired_count > max_keep) {
        struct retired_image *image = &p->retired[0];
        GLenum status = gl->ClientWaitSync(image->fence,
                                           GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            status = gl->ClientWaitSync(image->fence,
                                        GL_SYNC_FLUSH_COMMANDS_BIT,
                                        MP_TIME_MS_TO_NS(50));
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
                gl->Finish();
        }
        retire_release(mapper, 0);
    }
}

// Take a retired image back as the mapper's current mapping, for redraws of
// a frame whose codec buffer was already retired, or to restore the last
// shown picture when a redraw arrives without a fresh ImageReader callback.
// Whatever the mapper currently holds (e.g. the still-unbound texture of a
// failed map attempt) is released first.
static void mapper_adopt_retired(struct ra_hwdec_mapper *mapper,
                                 struct retired_image image)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    gl->DeleteSync(image.fence);
    ra_tex_free(mapper->ra, &mapper->tex[0]);
    if (p->gl_texture)
        gl->DeleteTextures(1, &p->gl_texture);
    if (p->egl_image) {
        p->DestroyImageKHR(eglGetCurrentDisplay(), p->egl_image);
        p->egl_image = NULL;
    }
    if (p->image) {
        o->AImage_delete(p->image);
        p->image = NULL;
    }
    p->image = image.image;
    p->egl_image = image.egl_image;
    p->gl_texture = image.texture;
    mapper->tex[0] = image.wrapped_tex;
    mp_image_unrefp(&image.source);
    p->sample_submitted = true;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    if (p->sample_fence) {
        gl->Finish();
        gl->DeleteSync(p->sample_fence);
        p->sample_fence = NULL;
    }

    o->AImageReader_setImageListener(o->reader, NULL);
    mp_mutex_lock(&o->callback_lock);
    o->callback_mapper = NULL;
    while (o->callbacks_active)
        mp_cond_wait(&o->callback_cond, &o->callback_lock);
    mp_mutex_unlock(&o->callback_lock);

    if (p->buffer_retire)
        retire_reap(mapper, 0);

    if (p->egl_image) {
        p->DestroyImageKHR(eglGetCurrentDisplay(), p->egl_image);
        p->egl_image = NULL;
    }
    if (p->image) {
        o->AImage_delete(p->image);
        p->image = NULL;
    }

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;

    ra_tex_free(mapper->ra, &mapper->tex[0]);

    mp_mutex_destroy(&p->lock);
    mp_cond_destroy(&p->cond);
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    if (p->buffer_retire && p->image && p->egl_image &&
        p->sample_submitted && p->gl_texture && mapper->tex[0]) {
        GL *gl = ra_gl_get(mapper->ra);
        p->sample_fence = gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (p->sample_fence)
            gl->Flush();
    }
    if (p->buffer_retire && p->image && p->egl_image &&
        p->sample_submitted && p->sample_fence && p->gl_texture) {
        if (p->retired_count >= MP_ARRAY_SIZE(p->retired))
            retire_reap(mapper, 1);
        mp_assert(p->retired_count < MP_ARRAY_SIZE(p->retired));
        p->retired[p->retired_count++] = (struct retired_image){
            .source = mp_image_new_ref(mapper->src),
            .image = p->image,
            .egl_image = p->egl_image,
            .display = eglGetCurrentDisplay(),
            .fence = p->sample_fence,
            .texture = p->gl_texture,
            .wrapped_tex = mapper->tex[0],
        };
        mapper->tex[0] = NULL;
        p->image = NULL;
        p->egl_image = NULL;
        p->sample_fence = NULL;
        p->gl_texture = 0;
        p->sample_submitted = false;
        return;
    }

    if (p->direct_yuv && p->image && p->sample_submitted) {
        GL *gl = ra_gl_get(mapper->ra);
        // Keep the codec buffer alive until libplacebo has sampled the texture.
        // A missing fence falls back to the same completion barrier.
        gl->Finish();
    }
    if (p->sample_fence) {
        GL *gl = ra_gl_get(mapper->ra);
        gl->DeleteSync(p->sample_fence);
        p->sample_fence = NULL;
    }

    if (p->egl_image) {
        p->DestroyImageKHR(eglGetCurrentDisplay(), p->egl_image);
        p->egl_image = 0;
    }
    if (p->image) {
        o->AImage_delete(p->image);
        p->image = NULL;
    }
    p->sample_submitted = false;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);
    p->sample_submitted = false;

    if (p->buffer_retire) {
        // A redraw may map one MediaCodec output more than once. Reuse its
        // still-owned external texture instead of acquiring a nonexistent
        // second AImage for the same codec buffer.
        for (int i = 0; i < p->retired_count; i++) {
            struct retired_image *held = &p->retired[i];
            if (!held->source ||
                held->source->planes[3] != mapper->src->planes[3] ||
                held->source->pts != mapper->src->pts)
                continue;
            mapper_adopt_retired(mapper, retired_take(p, i));
            return 0;
        }
        retire_reap(mapper, 1);
        if (!p->gl_texture && !create_next_external_texture(mapper))
            return -1;
        if (!mapper->tex[0] && !wrap_external_texture(mapper))
            return -1;
    }

    if (p->direct_yuv &&
        (mapper->src_params.repr.sys != PL_COLOR_SYSTEM_DOLBYVISION ||
         !mapper->src_params.repr.dovi || !mapper->src->dovi)) {
        MP_ERR(mapper, "P5 direct YUV frame lacks DOVI metadata\n");
        return -1;
    }

    {
        if (mapper->src->imgfmt != IMGFMT_MEDIACODEC)
            return -1;
        AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)mapper->src->planes[3];
        av_mediacodec_release_buffer(buffer, 1);
    }

    // A queued callback does not guarantee an image is still available.
    // Retry this transient state without spinning the VO.
    media_status_t ret = AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
    bool image_notified = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        mp_mutex_lock(&p->lock);
        if (!p->image_available)
            mp_cond_timedwait(&p->cond, &p->lock, MP_TIME_MS_TO_NS(10));
        image_notified |= p->image_available;
        p->image_available = false;
        mp_mutex_unlock(&p->lock);

        ret = o->AImageReader_acquireLatestImage(o->reader, &p->image);
        if (ret != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
            break;
    }
    if (ret != AMEDIA_OK) {
        // A codec flush can discard a just-released output before it reaches
        // ImageReader. Keep the preceding image only for a nearby PTS; a seek
        // or new stream must not inherit an unrelated picture.
        if (p->direct_retire && p->retired_count > 0) {
            struct retired_image *held = &p->retired[p->retired_count - 1];
            double delta = mapper->src->pts - held->source->pts;
            if (delta > 0.0 && delta <= 0.05) {
                mapper_adopt_retired(mapper,
                                     retired_take(p, p->retired_count - 1));
                MP_WARN(mapper, "ImageReader missed adjacent frame at pts=%.9f; displaying previous image\n",
                        mapper->src->pts);
                return 0;
            }
        }
        // The ordinary OES mapper can be asked to draw the same codec frame
        // again without a fresh ImageReader callback. Upstream keeps the
        // previous texture in that case; retiring moved that texture (and
        // its image) out of the mapper, so restore the most recently
        // retired image instead of sampling an empty external texture.
        if (!p->direct_yuv && !image_notified) {
            if (p->retired_count > 0) {
                mapper_adopt_retired(mapper,
                                     retired_take(p, p->retired_count - 1));
            } else {
                // Nothing to restore (e.g. the very first frame); keep
                // whatever texture the mapper still holds.
                p->sample_submitted = true;
            }
            return 0;
        }
        mp_mutex_lock(&p->lock);
        bool notification_pending = p->image_available;
        mp_mutex_unlock(&p->lock);
        MP_ERR(mapper, "acquireLatestImage failed after retry: %d pts=%.9f notification_pending=%d\n",
               ret, mapper->src->pts, notification_pending);
        return -1;
    }
    mp_assert(p->image);

    AHardwareBuffer *hwbuf = NULL;
    ret = o->AImage_getHardwareBuffer(p->image, &hwbuf);
    if (ret != AMEDIA_OK) {
        MP_ERR(mapper, "getHardwareBuffer failed: %d\n", ret);
        return -1;
    }
    mp_assert(hwbuf);

    // Update texture size since it may differ
    AHardwareBuffer_Desc d;
    o->AHardwareBuffer_describe(hwbuf, &d);
    if (!p->ahb_format_checked) {
        p->ahb_format_checked = true;
        p->dovi_rescale_k = 1.0f;
        for (int i = 0; i < MP_ARRAY_SIZE(ahb_yuv_scales); i++) {
            if (ahb_yuv_scales[i].format == d.format) {
                p->dovi_rescale_k = ahb_yuv_scales[i].k;
                break;
            }
        }
        MP_VERBOSE(mapper, "AHB format=0x%x stride=%u layers=%u %ux%u; dovi rescale k=%.6f\n",
                   (unsigned)d.format, (unsigned)d.stride, (unsigned)d.layers,
                   (unsigned)d.width, (unsigned)d.height, p->dovi_rescale_k);
    }
    // The GLES external YUV sampler hands the reshaper a signal normalized
    // as code/1020, which in the 10-bit BL reshape domain (code/1023) is
    // inflated by 1023/1020. Compensate by rescaling the reshape pivots and
    // polynomial/MMR coefficients of the frame's own DOVI metadata
    // (substituting s = s'/k), so every consumer of the mp_image sees the
    // corrected mapping.
    if (p->direct_yuv && p->dovi_rescale_k != 1.0f &&
        mapper->src->dovi && mapper->src->params.repr.dovi &&
        (void *) mapper->src->dovi->data == mapper->src->params.repr.dovi &&
        (mapper->src->planes[3] != p->dovi_rescale_last_buffer ||
         mapper->src->pts != p->dovi_rescale_last_pts)) {
        struct pl_dovi_metadata *meta = (void *) mapper->src->dovi->data;
        const float k = p->dovi_rescale_k;
        p->dovi_rescale_last_buffer = mapper->src->planes[3];
        p->dovi_rescale_last_pts = mapper->src->pts;
        for (int c = 0; c < 3; c++) {
            struct pl_reshape_data *comp = &meta->comp[c];
            if (!comp->num_pivots)
                continue;
            // Interior pivots select the reshape segment on the sampled
            // (rescaled) input signal and must follow it; the first and
            // last pivots only clamp the reshape output, which stays in
            // the original domain.
            for (int i = 1; i + 1 < comp->num_pivots; i++)
                comp->pivots[i] *= k;
            for (int i = 0; i + 1 < comp->num_pivots; i++) {
                if (comp->method[i] == 0) {
                    comp->poly_coeffs[i][1] /= k;
                    comp->poly_coeffs[i][2] /= k * k;
                } else if (comp->method[i] == 1) {
                    for (int j = 0; j < 3 && j < comp->mmr_order[i]; j++) {
                        // Per reshape order j (0-based), the weight groups
                        // multiply basis terms of degree j+1 (the linear
                        // sig terms), 2*(j+1) (the sigX.xyz cross terms)
                        // and 3*(j+1) (the sigX.w triple product).
                        float k1 = powf(k, j + 1);
                        for (int w = 0; w < 3; w++)
                            comp->mmr_coeffs[i][j][w] /= k1;
                        for (int w = 3; w < 6; w++)
                            comp->mmr_coeffs[i][j][w] /= k1 * k1;
                        comp->mmr_coeffs[i][j][6] /= k1 * k1 * k1;
                    }
                }
            }
        }
    }
    AImageCropRect crop = {0};
    if (p->direct_yuv) {
        int64_t image_ns = 0;
        media_status_t ts_ret = o->AImage_getTimestamp(p->image, &image_ns);
        media_status_t crop_ret = o->AImage_getCropRect(p->image, &crop);
        double frame_ns = mapper->src->pts * 1000000000.0;
        bool valid_pts = isfinite(frame_ns) && frame_ns >= 0 &&
                         frame_ns < (double)INT64_MAX && image_ns >= 0 &&
                         fabs(frame_ns - (double)image_ns) <= 1000.0;
        bool valid_crop = crop_ret == AMEDIA_OK &&
                          crop.left >= 0 && crop.top >= 0 &&
                          crop.right > crop.left && crop.bottom > crop.top &&
                          crop.right <= d.width && crop.bottom <= d.height &&
                          crop.right - crop.left == mapper->src_params.w &&
                          crop.bottom - crop.top == mapper->src_params.h;
        if (ts_ret != AMEDIA_OK || !valid_pts || !valid_crop) {
            MP_ERR(mapper, "P5 direct YUV identity/crop mismatch: frame_pts=%.9f image_ns=%"PRId64" ts=%d crop=%d,%d,%d,%d crop_status=%d buffer=%ux%u expected=%dx%d\n",
                   mapper->src->pts, image_ns, ts_ret,
                   crop.left, crop.top, crop.right, crop.bottom, crop_ret,
                   d.width, d.height, mapper->src_params.w,
                   mapper->src_params.h);
            return -1;
        }
    } else if (mapper->tex[0]->params.w != d.width ||
               mapper->tex[0]->params.h != d.height) {
        MP_VERBOSE(p, "Texture dimensions changed to %dx%d\n", d.width, d.height);
        mapper->tex[0]->params.w = d.width;
        mapper->tex[0]->params.h = d.height;
    }

    const EGLint basic_attribs[] = {EGL_NONE};
    const EGLint yuv_attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLClientBuffer buf = p->GetNativeClientBufferANDROID(hwbuf);
    if (!buf)
        return -1;
    p->egl_image = p->CreateImageKHR(eglGetCurrentDisplay(),
        EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf,
        p->direct_yuv ? yuv_attribs : basic_attribs);
    if (!p->egl_image)
        return -1;

    if (p->direct_yuv) {
        GLint old_active, old_external;
        gl->GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
        gl->ActiveTexture(GL_TEXTURE0);
        gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_external);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
        p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, p->egl_image);
        GLenum import_error = gl->GetError();
        if (import_error == GL_NO_ERROR)
            p->sample_submitted = true; // unmap waits for libplacebo's sample
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_external);
        gl->ActiveTexture(old_active);
        if (import_error != GL_NO_ERROR)
            return -1;
    } else {
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
        p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, p->egl_image);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        if (gl->GetError() == GL_NO_ERROR)
            p->sample_submitted = true;
    }

    return 0;
}


const struct ra_hwdec_driver ra_hwdec_aimagereader = {
    .name = "aimagereader",
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
