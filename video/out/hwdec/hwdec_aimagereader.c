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

#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <sys/system_properties.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <media/NdkImageReader.h>
#include <android/native_window_jni.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "misc/jni.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "video/out/gpu/hwdec.h"
#include "video/mp_image.h"
#include "video/out/opengl/ra_gl.h"
#include "video/out/opengl/formats.h"

typedef void *GLeglImageOES;
typedef void *EGLImageKHR;
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#ifndef GL_TEXTURE_BINDING_EXTERNAL_OES
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

// The shared FFmpeg build still references this default-off stage probe flag.
atomic_bool media_kit_stage_probe_enabled;

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
    bool raw_yuv;
    bool direct_yuv;
    bool raw_mrt;
    bool raw_half;
    bool raw_packed10;
    bool raw_420_sidecar;
    bool raw_code_scale;
    bool raw_chroma_left_probe;
    bool raw_full_dump;
    bool raw_full_dump_done;
    bool raw_pixel_probe;
    bool raw_spatial_probe;
    bool raw_peak_probe;
    bool raw_local_probe;
    bool raw_error_probe;
    bool rpu_hash_logged;
    int raw_pixel_frames;
    unsigned raw_peak_done;
    void (GLAPIENTRY *DrawBuffers)(GLsizei, const GLenum *);
    bool raw_perf;
    bool section_perf;
    uint64_t section_frames;
    int64_t section_acquire_wall_ns, section_acquire_cpu_ns;
    int64_t section_create_wall_ns, section_create_cpu_ns;
    int64_t section_bind_wall_ns, section_bind_cpu_ns;
    int64_t section_finish_wall_ns, section_finish_cpu_ns;
    bool raw_split;
    bool raw_early_fence;
    bool raw_retire;
    bool direct_retire;
    GLsync raw_sample_fence;
    struct retired_image retired[2];
    int retired_count;
    uint64_t raw_retire_maps, raw_retire_reaped, raw_retire_waits;
    uint64_t raw_retire_fallbacks;
    int64_t raw_retire_wait_ns;
    uint64_t image_callbacks;
    uint64_t empty_acquires;
    uint64_t image_acquired;
    uint64_t image_deleted;
    bool image_timeline;
    bool ahb_identity_probe;
    uint64_t ahb_identity_samples;
    double timeline_failed_pts;
    int64_t timeline_failed_us;
    bool timeline_followup;
    int64_t raw_perf_split_ns;
    uint64_t raw_split_frames;
    bool raw_timer;
    bool raw_timer_pending;
    GLuint raw_timer_query;
    bool raw_format_probe;
    bool raw_format_probe_logged;
    bool raw_gl_preimport_probe;
    bool raw_gl_preimport_probe_logged;
    bool raw_perf_pending;
    volatile bool raw_submitted;
    uint64_t raw_perf_frames, raw_perf_window_frames;
    int64_t raw_perf_submit_ns, raw_perf_submit_window_sum_ns;
    int64_t raw_perf_submit_window_max_ns;
    int64_t raw_perf_finish_window_sum_ns, raw_perf_finish_window_max_ns;
    GLuint raw_program;
    GLuint raw_vao;
    GLuint raw_texture[3];
    GLuint raw_fbo[3];
    int raw_w, raw_h;
    int raw_logged;
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

static int64_t p5_mapper_cpu_ns(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void p5_section_log(const char *tag, const char *fmt, ...)
{
    void *lib = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
    typedef int (*vprint_fn)(int, const char *, const char *, va_list);
    vprint_fn vprint = lib ? dlsym(lib, "__android_log_vprint") : NULL;
    if (vprint) {
        va_list args;
        va_start(args, fmt);
        vprint(6, tag, fmt, args);
        va_end(args);
    }
    if (lib)
        dlclose(lib);
}

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

    // dummy dimensions, AImageReader only transports hardware buffers
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
    p->image_callbacks++;
    uint64_t callback_number = p->image_callbacks;
    bool timeline = p->image_timeline;
    p->image_available = true;
    mp_cond_signal(&p->cond);
    mp_mutex_unlock(&p->lock);
    if (timeline && callback_number <= 12)
        p5_section_log("P5_TIMELINE", "callback n=%"PRIu64" mono_us=%"PRId64,
                       callback_number, mp_time_ns() / 1000);
    mp_mutex_lock(&o->callback_lock);
    mp_assert(o->callbacks_active > 0);
    if (--o->callbacks_active == 0)
        mp_cond_signal(&o->callback_cond);
    mp_mutex_unlock(&o->callback_lock);
}

// The ordinary GLES format table deliberately excludes R32F render targets.
// This diagnostic path requires EXT_color_buffer_float and checks every FBO.
static const struct gl_format raw_gl_format = {
    .name = "r32f", .internal_format = GL_R32F,
    .format = GL_RED, .type = GL_FLOAT, .flags = F_CR | F_ES3,
};
static const struct ra_format raw_ra_format = {
    .name = "r32f", .priv = (void *)&raw_gl_format,
    .ctype = RA_CTYPE_FLOAT, .ordered = true, .num_components = 1,
    .component_size = {32}, .component_depth = {32}, .pixel_size = 4,
    .renderable = true,
};
static const struct gl_format raw_gl_format_half = {
    .name = "r16f", .internal_format = GL_R16F,
    .format = GL_RED, .type = GL_HALF_FLOAT, .flags = F_CR | F_ES3,
};
static const struct ra_format raw_ra_format_half = {
    .name = "r16f", .priv = (void *)&raw_gl_format_half,
    .ctype = RA_CTYPE_FLOAT, .ordered = true, .num_components = 1,
    .component_size = {16}, .component_depth = {16}, .pixel_size = 2,
    .renderable = true,
};
static const struct gl_format raw_gl_format_packed10 = {
    .name = "rgb10_a2", .internal_format = GL_RGB10_A2,
    .format = GL_RGBA, .type = GL_UNSIGNED_INT_2_10_10_10_REV,
    .flags = F_CR | F_ES3,
};
static const struct ra_format raw_ra_format_packed10 = {
    .name = "rgb10_a2", .priv = (void *)&raw_gl_format_packed10,
    .ctype = RA_CTYPE_UNORM, .ordered = true, .num_components = 4,
    .component_size = {10, 10, 10, 2},
    .component_depth = {10, 10, 10, 2}, .pixel_size = 4,
    .renderable = true,
};

static const char raw_vertex_shader[] =
    "#version 300 es\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char raw_fragment_shader[] =
    "#version 300 es\n"
    "#extension GL_EXT_YUV_target : require\n"
    "precision highp float;\n"
    "precision highp __samplerExternal2DY2YEXT;\n"
    "uniform __samplerExternal2DY2YEXT image;\n"
    "uniform vec4 crop;\n"
    "uniform vec2 size;\n"
    "uniform int plane;\n"
    "out highp float value;\n"
    "void main() {\n"
    "  vec2 uv = crop.xy + (gl_FragCoord.xy / size) * crop.zw;\n"
    "  vec3 yuv = texture(image, uv).rgb;\n"
    "  value = plane == 0 ? yuv.r : (plane == 1 ? yuv.g : yuv.b);\n"
    "}\n";

static const char raw_fragment_shader_mrt[] =
    "#version 300 es\n"
    "#extension GL_EXT_YUV_target : require\n"
    "precision highp float;\n"
    "precision highp __samplerExternal2DY2YEXT;\n"
    "uniform __samplerExternal2DY2YEXT image;\n"
    "uniform vec4 crop;\n"
    "uniform vec2 size;\n"
    "layout(location=0) out highp float y_value;\n"
    "layout(location=1) out highp float u_value;\n"
    "layout(location=2) out highp float v_value;\n"
    "void main() {\n"
    "  vec2 uv = crop.xy + (gl_FragCoord.xy / size) * crop.zw;\n"
    "  vec3 yuv = texture(image, uv).rgb;\n"
    "  y_value = yuv.r;\n"
    "  u_value = yuv.g;\n"
    "  v_value = yuv.b;\n"
    "}\n";

static const char raw_fragment_shader_packed10[] =
    "#version 300 es\n"
    "#extension GL_EXT_YUV_target : require\n"
    "precision highp float;\n"
    "precision highp __samplerExternal2DY2YEXT;\n"
    "uniform __samplerExternal2DY2YEXT image;\n"
    "uniform vec4 crop;\n"
    "uniform vec2 size;\n"
    "uniform float code_scale;\n"
    "uniform float chroma_left_probe;\n"
    "layout(location=0) out highp vec4 yuv_value;\n"
    "void main() {\n"
    "  vec2 uv = crop.xy + (gl_FragCoord.xy / size) * crop.zw;\n"
    "  vec3 yuv = texture(image, uv).rgb;\n"
    "  if (chroma_left_probe > 0.5 && mod(floor(gl_FragCoord.x), 2.0) > 0.5) {\n"
    "    vec2 chroma_uv = uv + vec2(crop.z / size.x, 0.0);\n"
    "    yuv.gb = texture(image, chroma_uv).gb;\n"
    "  }\n"
    "  yuv_value = vec4(yuv * code_scale, 1.0);\n"
    "}\n";

static GLuint raw_compile_shader(struct ra_hwdec_mapper *mapper, GLenum type,
                                 const char *source)
{
    GL *gl = ra_gl_get(mapper->ra);
    GLuint shader = gl->CreateShader(type);
    gl->ShaderSource(shader, 1, &source, NULL);
    gl->CompileShader(shader);
    GLint ok = 0;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl->GetShaderInfoLog(shader, sizeof(log), NULL, log);
        MP_ERR(mapper, "P5 raw YUV shader compilation failed: %s\n", log);
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool raw_create_program(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    GLuint vertex = raw_compile_shader(mapper, GL_VERTEX_SHADER,
                                       raw_vertex_shader);
    GLuint fragment = raw_compile_shader(mapper, GL_FRAGMENT_SHADER,
                                         p->raw_packed10 ? raw_fragment_shader_packed10 :
                                         p->raw_mrt ? raw_fragment_shader_mrt : raw_fragment_shader);
    if (vertex && fragment) {
        p->raw_program = gl->CreateProgram();
        gl->AttachShader(p->raw_program, vertex);
        gl->AttachShader(p->raw_program, fragment);
        gl->LinkProgram(p->raw_program);
        GLint ok = 0;
        gl->GetProgramiv(p->raw_program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {0};
            gl->GetProgramInfoLog(p->raw_program, sizeof(log), NULL, log);
            MP_ERR(mapper, "P5 raw YUV shader link failed: %s\n", log);
            gl->DeleteProgram(p->raw_program);
            p->raw_program = 0;
        }
    }
    if (vertex)
        gl->DeleteShader(vertex);
    if (fragment)
        gl->DeleteShader(fragment);
    if (p->raw_program)
        gl->GenVertexArrays(1, &p->raw_vao);
    return p->raw_program && p->raw_vao;
}

static void raw_destroy_targets(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    for (int n = 0; n < 3; n++) {
        ra_tex_free(mapper->ra, &mapper->tex[n]);
        if (p->raw_fbo[n])
            gl->DeleteFramebuffers(1, &p->raw_fbo[n]);
        if (p->raw_texture[n])
            gl->DeleteTextures(1, &p->raw_texture[n]);
        p->raw_fbo[n] = p->raw_texture[n] = 0;
    }
    p->raw_w = p->raw_h = 0;
}

static bool raw_create_targets(struct ra_hwdec_mapper *mapper, int w, int h)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    if (p->raw_420_sidecar && ((w | h) & 1)) {
        MP_ERR(mapper, "P5 420 sidecar requires even dimensions: %dx%d\n", w, h);
        return false;
    }
    if (p->raw_w == w && p->raw_h == h)
        return true;
    GLint old_texture, old_draw_fbo, old_read_fbo;
    gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    for (int n = 0; n < 3; n++) {
        if (old_texture == p->raw_texture[n])
            old_texture = 0;
        if (old_draw_fbo == p->raw_fbo[n])
            old_draw_fbo = 0;
        if (old_read_fbo == p->raw_fbo[n])
            old_read_fbo = 0;
    }
    raw_destroy_targets(mapper);
    for (int n = 0; n < (p->raw_packed10 ? (p->raw_420_sidecar ? 2 : 1) : 3); n++) {
        int tex_w = p->raw_420_sidecar && n == 1 ? w / 2 : w;
        int tex_h = p->raw_420_sidecar && n == 1 ? h / 2 : h;
        gl->GenTextures(1, &p->raw_texture[n]);
        gl->BindTexture(GL_TEXTURE_2D, p->raw_texture[n]);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->TexImage2D(GL_TEXTURE_2D, 0,
                       p->raw_packed10 ? GL_RGB10_A2 : p->raw_half ? GL_R16F : GL_R32F,
                       tex_w, tex_h, 0, p->raw_packed10 ? GL_RGBA : GL_RED,
                       p->raw_packed10 ? GL_UNSIGNED_INT_2_10_10_10_REV :
                       p->raw_half ? GL_HALF_FLOAT : GL_FLOAT, NULL);
        if (!p->raw_mrt) {
            gl->GenFramebuffers(1, &p->raw_fbo[n]);
            gl->BindFramebuffer(GL_FRAMEBUFFER, p->raw_fbo[n]);
            gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, p->raw_texture[n], 0);
            GLenum status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
            GLenum err = gl->GetError();
            if (status != GL_FRAMEBUFFER_COMPLETE || err != GL_NO_ERROR) {
                MP_ERR(mapper, "P5 raw YUV R32F FBO unavailable: plane=%d status=%x error=%x\n",
                       n, status, err);
                goto fail;
            }
        }
        struct ra_tex_params params = {
            .dimensions = 2, .w = tex_w, .h = tex_h, .d = 1,
            .format = p->raw_packed10 ? &raw_ra_format_packed10 :
                      p->raw_half ? &raw_ra_format_half : &raw_ra_format,
            .render_src = true,
        };
        mapper->tex[n] = ra_create_wrapped_tex(mapper->ra, &params,
                                                p->raw_texture[n]);
        if (!mapper->tex[n])
            goto fail;
    }
    if (p->raw_mrt) {
        const GLenum attachments[] = {GL_COLOR_ATTACHMENT0,
                                      GL_COLOR_ATTACHMENT1,
                                      GL_COLOR_ATTACHMENT2};
        gl->GenFramebuffers(1, &p->raw_fbo[0]);
        gl->BindFramebuffer(GL_FRAMEBUFFER, p->raw_fbo[0]);
        for (int n = 0; n < 3; n++)
            gl->FramebufferTexture2D(GL_FRAMEBUFFER, attachments[n],
                                     GL_TEXTURE_2D, p->raw_texture[n], 0);
        p->DrawBuffers(3, attachments);
        GLenum status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
        GLenum err = gl->GetError();
        if (status != GL_FRAMEBUFFER_COMPLETE || err != GL_NO_ERROR) {
            MP_ERR(mapper, "P5 raw YUV MRT FBO unavailable: status=%x error=%x\n",
                   status, err);
            goto fail;
        }
    }
    gl->BindTexture(GL_TEXTURE_2D, old_texture);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fbo);
    p->raw_w = w;
    p->raw_h = h;
    return true;
fail:
    gl->BindTexture(GL_TEXTURE_2D, old_texture);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fbo);
    raw_destroy_targets(mapper);
    return false;
}

static void raw_set_enabled(GL *gl, GLenum cap, GLint enabled)
{
    if (enabled)
        gl->Enable(cap);
    else
        gl->Disable(cap);
}

static void raw_stage(const char *stage)
{
    void *lib = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
    typedef int (*log_print_fn)(int, const char *, const char *, ...);
    log_print_fn log_print = lib ? dlsym(lib, "__android_log_print") : NULL;
    if (log_print)
        log_print(6, "P5_RAW_STAGE", "%s", stage);
    if (lib)
        dlclose(lib);
}

static void raw_probe_pixels(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    double pts = mapper->src->pts;
    if (!p->raw_pixel_probe)
        return;
    static const double peak_pts[] = {18.40, 56.62, 62.56};
    static const int peak_x[] = {1538, 2078, 2388};
    static const int peak_y[] = {714, 1552, 318};
    static const int error_x[] = {2217, 2580, 1751, 1985};
    static const int error_y[] = {1045, 938, 1189, 1002};
    int peak = -1;
    if (p->raw_error_probe) {
        if (pts < 10.0 || pts >= 10.02 || p->raw_pixel_frames)
            return;
    } else if (p->raw_peak_probe) {
        for (int i = 0; i < 3; i++) {
            if (!(p->raw_peak_done & (1u << i)) &&
                pts >= peak_pts[i] && pts < peak_pts[i] + 0.20) {
                peak = i;
                break;
            }
        }
        if (peak < 0)
            return;
        p->raw_peak_done |= 1u << peak;
    } else if (pts < 10.0 ||
               pts >= (p->raw_spatial_probe ? 10.02 : 11.0) ||
               p->raw_pixel_frames >= (p->raw_spatial_probe ? 1 : 60)) {
        return;
    }
    p->raw_pixel_frames++;
    if (p->raw_pixel_frames == 1)
        raw_stage("pixel_probe_readback");
    GLint old_read_buffer = 0, old_read_fbo = 0;
    gl->GetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, p->raw_fbo[0]);
    const int y = p->raw_h / 2;
    for (int i = 0; i < (p->raw_error_probe ? 4 : p->raw_spatial_probe ? 9 : 1); i++) {
        int x = p->raw_error_probe ? error_x[i] :
                p->raw_peak_probe ? peak_x[peak] :
                p->raw_spatial_probe ? (i % 3 + 1) * p->raw_w / 4 : p->raw_w / 2;
        int sample_y = p->raw_error_probe ? error_y[i] :
                       p->raw_peak_probe ? peak_y[peak] :
                       p->raw_spatial_probe ? (i / 3 + 1) * p->raw_h / 4 : y;
        if (p->raw_packed10) {
            if (p->raw_error_probe) {
                uint32_t local[49] = {0};
                gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
                gl->ReadPixels(x - 3, sample_y - 3, 7, 7, GL_RGBA,
                               GL_UNSIGNED_INT_2_10_10_10_REV, local);
                GLenum local_error = gl->GetError();
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 7; col++) {
                        uint32_t value = local[row * 7 + col];
                        char line[256];
                        snprintf(line, sizeof(line),
                                 "P5_ERROR_RAW pts=%.9f xy=%d,%d yuv=%u,%u,%u error=0x%x",
                                 pts, x - 3 + col, sample_y - 3 + row,
                                 value & 1023, (value >> 10) & 1023,
                                 (value >> 20) & 1023, local_error);
                        raw_stage(line);
                    }
                }
                if (p->raw_420_sidecar) {
                    uint32_t uv = 0;
                    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, p->raw_fbo[1]);
                    gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
                    gl->ReadPixels(x / 2, sample_y / 2, 1, 1, GL_RGBA,
                                   GL_UNSIGNED_INT_2_10_10_10_REV, &uv);
                    GLenum uv_error = gl->GetError();
                    char line[256];
                    snprintf(line, sizeof(line),
                             "P5_420_UV pts=%.9f xy=%d,%d uv=%u,%u error=0x%x",
                             pts, x / 2, sample_y / 2,
                             (uv >> 10) & 1023, (uv >> 20) & 1023, uv_error);
                    raw_stage(line);
                    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, p->raw_fbo[0]);
                    gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
                }
            }
            if (p->raw_peak_probe && p->raw_local_probe && peak != 1) {
                uint32_t local[49] = {0};
                gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
                gl->ReadPixels(x - 3, sample_y - 3, 7, 7, GL_RGBA,
                               GL_UNSIGNED_INT_2_10_10_10_REV, local);
                GLenum local_error = gl->GetError();
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 7; col++) {
                        uint32_t value = local[row * 7 + col];
                        char line[256];
                        snprintf(line, sizeof(line),
                                 "P5_LOCAL pts=%.9f xy=%d,%d yuv=%u,%u,%u error=0x%x",
                                 pts, x - 3 + col, sample_y - 3 + row,
                                 value & 1023, (value >> 10) & 1023,
                                 (value >> 20) & 1023, local_error);
                        raw_stage(line);
                    }
                }
            }
            uint32_t value = 0;
            gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
            gl->ReadPixels(x, sample_y, 1, 1, GL_RGBA,
                           GL_UNSIGNED_INT_2_10_10_10_REV, &value);
            GLenum err = gl->GetError();
            char line[256];
            snprintf(line, sizeof(line),
                     "P5_PIXEL pts=%.9f xy=%d,%d format=packed10 yuv=%u,%u,%u error=0x%x",
                     mapper->src->pts, x, sample_y, value & 1023,
                     (value >> 10) & 1023, (value >> 20) & 1023, err);
            raw_stage(line);
        } else if (p->raw_mrt && !p->raw_half) {
            float values[3] = {0};
            GLenum errors[3] = {0};
            for (int c = 0; c < 3; c++) {
                gl->ReadBuffer(GL_COLOR_ATTACHMENT0 + c);
                gl->ReadPixels(x, sample_y, 1, 1, GL_RED, GL_FLOAT, &values[c]);
                errors[c] = gl->GetError();
            }
            char line[256];
            snprintf(line, sizeof(line),
                     "P5_PIXEL pts=%.9f xy=%d,%d format=r32f yuv=%.9f,%.9f,%.9f error=0x%x,0x%x,0x%x",
                     mapper->src->pts, x, sample_y, values[0], values[1], values[2],
                     errors[0], errors[1], errors[2]);
            raw_stage(line);
        }
    }
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
    gl->ReadBuffer(old_read_buffer);
}

static void raw_dump_full(struct ra_hwdec_mapper *mapper,
                          const AImageCropRect *crop, int buffer_w, int buffer_h)
{
    struct priv *p = mapper->priv;
    if (!p->raw_full_dump || p->raw_full_dump_done ||
        mapper->src->pts < 10.0 || mapper->src->pts >= 10.02 ||
        (!p->raw_420_sidecar && (p->raw_w > 256 || p->raw_h > 144)))
        return;

    GL *gl = ra_gl_get(mapper->ra);
    int dump_w = p->raw_420_sidecar ? p->raw_w / 2 : p->raw_w;
    int dump_h = p->raw_420_sidecar ? p->raw_h / 2 : p->raw_h;
    size_t bytes = (size_t)dump_w * dump_h * sizeof(uint32_t);
    uint32_t *pixels = malloc(bytes);
    if (!pixels)
        return;
    GLint old_read_fbo = 0, old_read_buffer = 0;
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    gl->GetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER,
                        p->raw_fbo[p->raw_420_sidecar ? 1 : 0]);
    gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->ReadPixels(0, 0, dump_w, dump_h, GL_RGBA,
                   GL_UNSIGNED_INT_2_10_10_10_REV, pixels);
    GLenum error = gl->GetError();
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
    gl->ReadBuffer(old_read_buffer);
    size_t written = 0;
    if (error == GL_NO_ERROR) {
        FILE *file = fopen(p->raw_420_sidecar ?
            "/sdcard/Android/data/com.example.media_kit_test/files/p5-raw-420-uv-packed10.bin" :
            "/sdcard/Android/data/com.example.media_kit_test/files/p5-raw-full-packed10.bin", "wb");
        if (file) {
            written = fwrite(pixels, 1, bytes, file);
            fclose(file);
        }
    }
    if (p->raw_420_sidecar && written == bytes) {
        size_t y_bytes = (size_t)p->raw_w * p->raw_h * sizeof(uint32_t);
        uint32_t *y_pixels = malloc(y_bytes);
        if (y_pixels) {
            gl->BindFramebuffer(GL_READ_FRAMEBUFFER, p->raw_fbo[0]);
            gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
            gl->ReadPixels(0, 0, p->raw_w, p->raw_h, GL_RGBA,
                           GL_UNSIGNED_INT_2_10_10_10_REV, y_pixels);
            GLenum y_error = gl->GetError();
            gl->BindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
            gl->ReadBuffer(old_read_buffer);
            size_t y_written = 0;
            if (y_error == GL_NO_ERROR) {
                FILE *y_file = fopen("/sdcard/Android/data/com.example.media_kit_test/files/p5-raw-420-y-packed10.bin", "wb");
                if (y_file) {
                    y_written = fwrite(y_pixels, 1, y_bytes, y_file);
                    fclose(y_file);
                }
            }
            MP_WARN(mapper, "P5_RAW_Y pts=%.9f bytes=%zu written=%zu error=0x%x\n",
                    mapper->src->pts, y_bytes, y_written, y_error);
            p->raw_full_dump_done = y_written == y_bytes;
            free(y_pixels);
        }
    } else {
        p->raw_full_dump_done = written == bytes;
    }
    MP_WARN(mapper, "P5_RAW_FULL pts=%.9f w=%d h=%d bytes=%zu written=%zu error=0x%x crop=%d,%d,%d,%d buffer=%d,%d chroma_left_probe=%d\n",
            mapper->src->pts, dump_w, dump_h, bytes, written, error,
            crop->left, crop->top, crop->right, crop->bottom,
            buffer_w, buffer_h, p->raw_chroma_left_probe);
    char line[256];
    snprintf(line, sizeof(line),
             "P5_RAW_FULL pts=%.9f size=%dx%d bytes=%zu written=%zu error=0x%x crop=%d,%d,%d,%d buffer=%d,%d chroma_left_probe=%d",
             mapper->src->pts, dump_w, dump_h, bytes, written, error,
             crop->left, crop->top, crop->right, crop->bottom,
             buffer_w, buffer_h, p->raw_chroma_left_probe);
    raw_stage(line);
    free(pixels);
}

static bool raw_render(struct ra_hwdec_mapper *mapper, const AImageCropRect *crop,
                       int buffer_w, int buffer_h)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    GLint old_program, old_vao, old_active, old_external, old_texture,
          old_draw_fbo, old_read_fbo, old_viewport[4];
    GLint blend, dither, scissor, depth, stencil, cull, discard;
    GLint write_mask[4];
    if (p->raw_logged <= 5) raw_stage("enter_state");
    gl->GetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    gl->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    gl->GetIntegerv(GL_VIEWPORT, old_viewport);
    gl->GetIntegerv(GL_BLEND, &blend);
    gl->GetIntegerv(GL_DITHER, &dither);
    gl->GetIntegerv(GL_SCISSOR_TEST, &scissor);
    gl->GetIntegerv(GL_DEPTH_TEST, &depth);
    gl->GetIntegerv(GL_STENCIL_TEST, &stencil);
    gl->GetIntegerv(GL_CULL_FACE, &cull);
    gl->GetIntegerv(GL_RASTERIZER_DISCARD, &discard);
    gl->GetIntegerv(GL_COLOR_WRITEMASK, write_mask);
    if (!write_mask[0] || (p->raw_packed10 && (!write_mask[1] || !write_mask[2]))) {
        MP_ERR(mapper, "P5 raw YUV required channel writes disabled\n");
        return false;
    }
    if (p->raw_logged <= 5) raw_stage("state_captured");
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_external);
    gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);

    if (p->raw_logged <= 5) raw_stage("textures_captured");
    gl->Disable(GL_BLEND);
    if (p->raw_logged <= 5) raw_stage("blend_off");
    gl->Disable(GL_DITHER);
    if (p->raw_logged <= 5) raw_stage("dither_off");
    gl->Disable(GL_SCISSOR_TEST);
    if (p->raw_logged <= 5) raw_stage("scissor_off");
    gl->Disable(GL_DEPTH_TEST);
    if (p->raw_logged <= 5) raw_stage("depth_off");
    gl->Disable(GL_STENCIL_TEST);
    if (p->raw_logged <= 5) raw_stage("stencil_off");
    gl->Disable(GL_CULL_FACE);
    if (p->raw_logged <= 5) raw_stage("cull_off");
    gl->Disable(GL_RASTERIZER_DISCARD);
    if (p->raw_logged <= 5) raw_stage("discard_off");
    gl->UseProgram(p->raw_program);
    if (p->raw_logged <= 5) raw_stage("program_bound");
    gl->BindVertexArray(p->raw_vao);
    if (p->raw_logged <= 5) raw_stage("vao_bound");
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    if (p->raw_logged <= 5) raw_stage("oes_bound");
    gl->Uniform1i(gl->GetUniformLocation(p->raw_program, "image"), 0);
    if (p->raw_logged <= 5) raw_stage("image_uniform");
    gl->Uniform2f(gl->GetUniformLocation(p->raw_program, "size"),
                  p->raw_w, p->raw_h);
    if (p->raw_packed10) {
        gl->Uniform1f(gl->GetUniformLocation(p->raw_program, "code_scale"),
                      p->raw_code_scale ? 1020.0f / 1023.0f : 1.0f);
        gl->Uniform1f(gl->GetUniformLocation(p->raw_program, "chroma_left_probe"),
                      p->raw_chroma_left_probe ? 1.0f : 0.0f);
    }
    if (p->raw_logged <= 5) raw_stage("size_uniform");
    // GL textures have a bottom-left origin; AImage crop coordinates are top-left.
    gl->Uniform4f(gl->GetUniformLocation(p->raw_program, "crop"),
                  (float)crop->left / buffer_w,
                  (float)(buffer_h - crop->bottom) / buffer_h,
                  (float)(crop->right - crop->left) / buffer_w,
                  (float)(crop->bottom - crop->top) / buffer_h);
    if (p->raw_logged <= 5) raw_stage("crop_uniform");
    if (p->raw_logged <= 5) raw_stage("draw_setup");
    if (p->raw_timer) {
        gl->BeginQuery(GL_TIME_ELAPSED, p->raw_timer_query);
        p->raw_timer_pending = true;
    }
    gl->Viewport(0, 0, p->raw_w, p->raw_h);
    if (p->raw_mrt || p->raw_packed10) {
        gl->BindFramebuffer(GL_FRAMEBUFFER, p->raw_fbo[0]);
        if (p->raw_logged <= 5) raw_stage("draw_mrt");
        gl->DrawArrays(GL_TRIANGLES, 0, 3);
        p->raw_submitted = true;
        if (p->raw_logged <= 5) raw_stage("draw_mrt_done");
    } else {
        for (int n = 0; n < 3; n++) {
            gl->BindFramebuffer(GL_FRAMEBUFFER, p->raw_fbo[n]);
            gl->Uniform1i(gl->GetUniformLocation(p->raw_program, "plane"), n);
            if (p->raw_logged <= 5) raw_stage(n == 0 ? "draw_y" : n == 1 ? "draw_u" : "draw_v");
            gl->DrawArrays(GL_TRIANGLES, 0, 3);
            p->raw_submitted = true;
            if (p->raw_logged <= 5) raw_stage(n == 0 ? "draw_y_done" : n == 1 ? "draw_u_done" : "draw_v_done");
        }
    }
    if (p->raw_420_sidecar) {
        gl->BindFramebuffer(GL_READ_FRAMEBUFFER, p->raw_fbo[0]);
        gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, p->raw_fbo[1]);
        gl->BlitFramebuffer(0, 0, p->raw_w, p->raw_h,
                            0, 0, p->raw_w / 2, p->raw_h / 2,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (p->raw_logged <= 5)
            raw_stage("blit_420_sidecar_done");
    }
    if (p->raw_timer)
        gl->EndQuery(GL_TIME_ELAPSED);
    raw_probe_pixels(mapper);
    raw_dump_full(mapper, crop, buffer_w, buffer_h);
    GLenum err = gl->GetError();
    if (p->raw_logged <= 5) raw_stage("before_restore");

    gl->BindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fbo);
    gl->Viewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_external);
    gl->BindTexture(GL_TEXTURE_2D, old_texture);
    gl->ActiveTexture(old_active);
    gl->BindVertexArray(old_vao);
    gl->UseProgram(old_program);
    raw_set_enabled(gl, GL_BLEND, blend);
    raw_set_enabled(gl, GL_DITHER, dither);
    raw_set_enabled(gl, GL_SCISSOR_TEST, scissor);
    raw_set_enabled(gl, GL_DEPTH_TEST, depth);
    raw_set_enabled(gl, GL_STENCIL_TEST, stencil);
    raw_set_enabled(gl, GL_CULL_FACE, cull);
    raw_set_enabled(gl, GL_RASTERIZER_DISCARD, discard);
    if (p->raw_logged <= 5) raw_stage("restored");
    if (err != GL_NO_ERROR)
        MP_ERR(mapper, "P5 raw YUV render failed: GL error=%x\n", err);
    return err == GL_NO_ERROR;
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

    char property[PROP_VALUE_MAX] = {0};
    bool p5_auto_yuv = mapper->src_params.dv_profile == 5 &&
                       mapper->src_params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION &&
                       mapper->src_params.repr.dovi;
    p->raw_yuv = p5_auto_yuv ||
                 (__system_property_get("debug.media_kit.p5_raw_yuv", property) > 0 &&
                  !strcmp(property, "1"));
    char section_property[PROP_VALUE_MAX] = {0};
    p->section_perf = p->raw_yuv &&
        __system_property_get("debug.media_kit.p5_section_perf", section_property) > 0 &&
        !strcmp(section_property, "1");
    p5_section_log("P5_SECTION_INIT", "raw=%d enabled=%d property=%s",
                   p->raw_yuv, p->section_perf, section_property);
    char direct_property[PROP_VALUE_MAX] = {0};
    p->direct_yuv = p->raw_yuv &&
        (p5_auto_yuv ||
         (__system_property_get("debug.media_kit.p5_direct_yuv", direct_property) > 0 &&
          !strcmp(direct_property, "1")));
    char mrt_property[PROP_VALUE_MAX] = {0};
    p->raw_mrt = p->raw_yuv &&
                 __system_property_get("debug.media_kit.p5_raw_mrt", mrt_property) > 0 &&
                 !strcmp(mrt_property, "1");
    char half_property[PROP_VALUE_MAX] = {0};
    p->raw_half = p->raw_mrt &&
                  __system_property_get("debug.media_kit.p5_raw_half", half_property) > 0 &&
                  !strcmp(half_property, "1");
    char packed_property[PROP_VALUE_MAX] = {0};
    p->raw_packed10 = p->raw_yuv &&
        __system_property_get("debug.media_kit.p5_raw_packed10", packed_property) > 0 &&
        !strcmp(packed_property, "1");
    char sidecar_property[PROP_VALUE_MAX] = {0};
    p->raw_420_sidecar = p->raw_packed10 &&
        __system_property_get("debug.media_kit.p5_raw_420_sidecar",
                              sidecar_property) > 0 &&
        !strcmp(sidecar_property, "1");
    char code_scale_property[PROP_VALUE_MAX] = {0};
    p->raw_code_scale = p->raw_packed10 &&
        __system_property_get("debug.media_kit.p5_raw_code_scale", code_scale_property) > 0 &&
        !strcmp(code_scale_property, "1");
    char chroma_left_property[PROP_VALUE_MAX] = {0};
    p->raw_chroma_left_probe = p->raw_packed10 &&
        __system_property_get("debug.media_kit.p5_raw_chroma_left_probe",
                              chroma_left_property) > 0 &&
        !strcmp(chroma_left_property, "1");
    if (p->raw_420_sidecar && (p->raw_chroma_left_probe ||
                               !gl->BlitFramebuffer)) {
        MP_ERR(mapper, "P5 420 sidecar requires GL blit and disables 444 phase probe\n");
        return -1;
    }
    char full_dump_property[PROP_VALUE_MAX] = {0};
    p->raw_full_dump = p->raw_packed10 &&
        __system_property_get("debug.media_kit.p5_raw_full_dump",
                              full_dump_property) > 0 &&
        !strcmp(full_dump_property, "1");
    if (p->raw_code_scale)
        raw_stage("code_scale_1020_to_1023_enabled");
    char pixel_property[PROP_VALUE_MAX] = {0};
    p->raw_pixel_probe = p->raw_yuv &&
        __system_property_get("debug.media_kit.p5_raw_pixel_probe", pixel_property) > 0 &&
        !strcmp(pixel_property, "1");
    char spatial_property[PROP_VALUE_MAX] = {0};
    p->raw_spatial_probe = p->raw_pixel_probe &&
        __system_property_get("debug.media_kit.p5_raw_spatial_probe", spatial_property) > 0 &&
        !strcmp(spatial_property, "1");
    char peak_property[PROP_VALUE_MAX] = {0};
    p->raw_peak_probe = p->raw_pixel_probe && !p->raw_spatial_probe &&
        __system_property_get("debug.media_kit.p5_raw_peak_probe", peak_property) > 0 &&
        !strcmp(peak_property, "1");
    char local_property[PROP_VALUE_MAX] = {0};
    p->raw_local_probe = p->raw_peak_probe &&
        __system_property_get("debug.media_kit.p5_raw_local_probe", local_property) > 0 &&
        !strcmp(local_property, "1");
    char error_property[PROP_VALUE_MAX] = {0};
    p->raw_error_probe = p->raw_pixel_probe && p->raw_packed10 &&
        __system_property_get("debug.media_kit.p5_raw_error_probe", error_property) > 0 &&
        !strcmp(error_property, "1");
    if (p->raw_pixel_probe)
        raw_stage("pixel_probe_enabled");
    if (p->raw_packed10 && (p->raw_mrt || p->raw_half)) {
        MP_ERR(mapper, "P5 packed10 is exclusive with MRT/R16F options\n");
        return -1;
    }
    if (p->raw_half &&
        !gl_check_extension(gl->extensions, "GL_EXT_color_buffer_half_float") &&
        gl->es < 320) {
        MP_ERR(mapper, "P5 raw R16F requires ES3.2 or EXT_color_buffer_half_float\n");
        return -1;
    }
    if (p->raw_mrt) {
        GLint max_draw_buffers = 0;
        p->DrawBuffers = (void *)eglGetProcAddress("glDrawBuffers");
        gl->GetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
        if (!p->DrawBuffers || max_draw_buffers < 3) {
            MP_ERR(mapper, "P5 raw MRT requires three draw buffers: max=%d fn=%p\n",
                   max_draw_buffers, p->DrawBuffers);
            return -1;
        }
    }
    char perf_property[PROP_VALUE_MAX] = {0};
    p->raw_perf = p->raw_yuv &&
                  __system_property_get("debug.media_kit.p5_raw_perf", perf_property) > 0 &&
                  !strcmp(perf_property, "1");
    char split_property[PROP_VALUE_MAX] = {0};
    p->raw_split = p->raw_perf &&
                   __system_property_get("debug.media_kit.p5_raw_split", split_property) > 0 &&
                   !strcmp(split_property, "1");
    char fence_property[PROP_VALUE_MAX] = {0};
    p->raw_early_fence = p->raw_yuv && !p->direct_yuv &&
                         __system_property_get("debug.media_kit.p5_raw_early_fence",
                                               fence_property) > 0 &&
                         !strcmp(fence_property, "1") &&
                         gl->FenceSync && gl->ClientWaitSync && gl->DeleteSync;
    char retire_property[PROP_VALUE_MAX] = {0};
    p->raw_retire = p->raw_early_fence &&
                    __system_property_get("debug.media_kit.p5_raw_retire",
                                              retire_property) > 0 &&
                    !strcmp(retire_property, "1");
    char direct_retire_property[PROP_VALUE_MAX] = {0};
    p->direct_retire = p->direct_yuv &&
        (p5_auto_yuv ||
         (__system_property_get("debug.media_kit.p5_dr_retire",
                                direct_retire_property) > 0 &&
          !strcmp(direct_retire_property, "1"))) &&
        gl->FenceSync && gl->ClientWaitSync && gl->DeleteSync && gl->Flush;
    p->raw_retire |= p->direct_retire;
    char timeline_property[PROP_VALUE_MAX] = {0};
    p->image_timeline = p->raw_yuv &&
        __system_property_get("debug.media_kit.p5_image_timeline",
                              timeline_property) > 0 &&
        !strcmp(timeline_property, "1");
    char ahb_identity_property[PROP_VALUE_MAX] = {0};
    p->ahb_identity_probe = p->direct_yuv &&
        __system_property_get("debug.media_kit.p5_ahb_identity_probe",
                              ahb_identity_property) > 0 &&
        !strcmp(ahb_identity_property, "1");
    p5_section_log("P5_DIRECT_RETIRE", "enabled=%d property=%s",
                   p->direct_retire, direct_retire_property);
    char vo_timer_property[PROP_VALUE_MAX] = {0};
    bool vo_gpu_timer = __system_property_get("debug.media_kit.p5_vo_gpu_timer",
                                               vo_timer_property) > 0 &&
                        !strcmp(vo_timer_property, "1");
    p->raw_timer = p->raw_perf && !p->direct_yuv && !vo_gpu_timer &&
                   !p->raw_retire &&
                   gl_check_extension(gl->extensions, "GL_EXT_disjoint_timer_query") &&
                   gl->GenQueries && gl->DeleteQueries && gl->BeginQuery &&
                   gl->EndQuery && gl->GetQueryObjectui64v;
    if (p->raw_timer) {
        gl->GenQueries(1, &p->raw_timer_query);
        p->raw_timer = p->raw_timer_query != 0;
    }
    if (p->raw_perf)
        MP_WARN(mapper, "P5 raw GPU timer available=%d ext=%d query=%u\n",
                p->raw_timer,
                gl_check_extension(gl->extensions, "GL_EXT_disjoint_timer_query"),
                p->raw_timer_query);
    if (p->raw_yuv) {
        void *log_lib = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
        typedef int (*log_print_fn)(int, const char *, const char *, ...);
        log_print_fn log_print = log_lib ? dlsym(log_lib, "__android_log_print") : NULL;
        if (log_print)
            log_print(6, "P5_RAW_TIME", "init perf=%d property=%s", p->raw_perf, perf_property);
        if (log_lib)
            dlclose(log_lib);
    }
    char format_property[PROP_VALUE_MAX] = {0};
    p->raw_format_probe = p->raw_yuv &&
                          __system_property_get("debug.media_kit.p5_raw_format_probe", format_property) > 0 &&
                          !strcmp(format_property, "1");
    char gl_probe_property[PROP_VALUE_MAX] = {0};
    p->raw_gl_preimport_probe = p->raw_yuv &&
        __system_property_get("debug.media_kit.p5_raw_gl_preimport_probe", gl_probe_property) > 0 &&
        !strcmp(gl_probe_property, "1");
    if (p->raw_yuv &&
        (mapper->src_params.repr.sys != PL_COLOR_SYSTEM_DOLBYVISION ||
         !mapper->src_params.repr.dovi)) {
        MP_ERR(mapper, "P5 raw YUV requested without first-frame DOVI metadata\n");
        return -1;
    }
    if (p->raw_yuv) {
        o->AImage_getCropRect = dlsym(o->lib_handle, "AImage_getCropRect");
        o->AImage_getTimestamp = dlsym(o->lib_handle, "AImage_getTimestamp");
        if (gl->es < 300 ||
            !gl_check_extension(gl->extensions, "GL_EXT_YUV_target") ||
            (!p->direct_yuv && !p->raw_packed10 &&
             !gl_check_extension(gl->extensions, "GL_EXT_color_buffer_float")) ||
            !o->AImage_getCropRect || !o->AImage_getTimestamp ||
            (!p->direct_yuv &&
             (!gl->GenVertexArrays || !gl->BindVertexArray ||
              !raw_create_program(mapper)))) {
            if (!p5_auto_yuv) {
                MP_ERR(mapper, "P5 raw YUV requires ES3, YUV_target, renderable FBO and shader support\n");
                return -1;
            }
            MP_WARN(mapper, "P5 direct YUV unavailable; falling back to standard OES import\n");
            p->raw_yuv = false;
            p->direct_yuv = false;
            p->direct_retire = false;
            p->raw_retire = false;
        }
    }

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
    mapper->dst_params.imgfmt = p->direct_yuv ? IMGFMT_MEDIACODEC_YUV :
                                p->raw_420_sidecar ? IMGFMT_YUV420_PACK10 :
                                p->raw_packed10 ? IMGFMT_YUV444_PACK10 :
                                p->raw_yuv ? IMGFMT_444PF : IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;
    // The upstream OES wrapper is non-subsampled and mp_image_params_guess()
    // consequently normalizes its chroma location to CENTER. The sidecar
    // reconstructs the source 4:2:0 planes, whose HEVC chroma sits LEFT.
    if (p->raw_420_sidecar)
        mapper->dst_params.chroma_location = PL_CHROMA_LEFT;
    if (p->raw_420_sidecar)
        MP_WARN(mapper, "P5 420 sidecar chroma_location=%d size=%dx%d\n",
                mapper->dst_params.chroma_location,
                mapper->dst_params.w, mapper->dst_params.h);
    if (p->raw_yuv) {
        // The shader copies normalized 10-bit BL code values, not float32 code words.
        mapper->dst_params.repr.bits = (struct pl_bit_encoding) {
            .sample_depth = 10, .color_depth = 10,
        };
        MP_WARN(mapper, "P5 raw YUV diagnostic enabled; using %s\n",
                p->direct_yuv ? "direct external YUV sampler" :
                p->raw_420_sidecar ? "RGB10_A2 Y plus half-size UV" :
                p->raw_packed10 ? "one RGB10_A2 YUV texture" : "R32F Y/U/V planes");
    }

    // texture creation
    gl->GenTextures(1, &p->gl_texture);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER,
                      p->raw_yuv ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER,
                      p->raw_yuv ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

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

    if (params.format->ctype != RA_CTYPE_UNORM)
        return -1;

    if (!p->raw_yuv || p->direct_yuv) {
        mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
        if (!mapper->tex[0])
            return -1;
    }

    return 0;
}

static void raw_retire_release(struct ra_hwdec_mapper *mapper, int index)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);
    struct retired_image image = p->retired[index];
    gl->DeleteSync(image.fence);
    if (image.wrapped_tex)
        ra_tex_free(mapper->ra, &image.wrapped_tex);
    gl->DeleteTextures(1, &image.texture);
    p->DestroyImageKHR(image.display, image.egl_image);
    o->AImage_delete(image.image);
    mp_image_unrefp(&image.source);
    p->image_deleted++;
    for (int i = index + 1; i < p->retired_count; i++)
        p->retired[i - 1] = p->retired[i];
    p->retired[--p->retired_count] = (struct retired_image){0};
    p->raw_retire_reaped++;
}

static void raw_retire_reap(struct ra_hwdec_mapper *mapper, int max_keep)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    for (int i = 0; i < p->retired_count;) {
        struct retired_image *image = &p->retired[i];
        GLenum status = gl->ClientWaitSync(image->fence,
                                           GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        bool ready = status == GL_ALREADY_SIGNALED ||
                     status == GL_CONDITION_SATISFIED;
        if (!ready && p->retired_count > max_keep) {
            int64_t start_ns = mp_time_ns();
            status = gl->ClientWaitSync(image->fence,
                                        GL_SYNC_FLUSH_COMMANDS_BIT,
                                        1000000000ULL);
            p->raw_retire_waits++;
            p->raw_retire_wait_ns += mp_time_ns() - start_ns;
            ready = status == GL_ALREADY_SIGNALED ||
                    status == GL_CONDITION_SATISFIED;
            if (!ready) {
                gl->Finish();
                p->raw_retire_fallbacks++;
                ready = true;
            }
        }
        if (ready)
            raw_retire_release(mapper, i);
        else
            i++;
    }
}

static bool raw_create_next_external_texture(struct ra_hwdec_mapper *mapper)
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
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_external);
    gl->ActiveTexture(old_active);
    if (gl->GetError() != GL_NO_ERROR) {
        gl->DeleteTextures(1, &p->gl_texture);
        p->gl_texture = 0;
        return false;
    }
    return true;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    if (p->raw_sample_fence) {
        gl->Finish();
        gl->DeleteSync(p->raw_sample_fence);
        p->raw_sample_fence = NULL;
    }

    o->AImageReader_setImageListener(o->reader, NULL);
    mp_mutex_lock(&o->callback_lock);
    o->callback_mapper = NULL;
    while (o->callbacks_active)
        mp_cond_wait(&o->callback_cond, &o->callback_lock);
    mp_mutex_unlock(&o->callback_lock);
    if (p->raw_retire) {
        int held_before = p->retired_count;
        raw_retire_reap(mapper, 0);
        char final_line[256];
        snprintf(final_line, sizeof(final_line),
                 "P5_RETIRE_FINAL maps=%"PRIu64" reaped=%"PRIu64
                 " held_before=%d held_after=%d waits=%"PRIu64
                 " fallbacks=%"PRIu64" callbacks=%"PRIu64
                 " empty_acquires=%"PRIu64" current_image=%d current_egl=%d",
                 p->raw_retire_maps, p->raw_retire_reaped,
                 held_before, p->retired_count, p->raw_retire_waits,
                 p->raw_retire_fallbacks, p->image_callbacks,
                 p->empty_acquires, p->image != NULL, p->egl_image != NULL);
        raw_stage(final_line);
        MP_WARN(mapper, "P5_RETIRE_FINAL maps=%"PRIu64" reaped=%"PRIu64
                " held_before=%d held_after=%d waits=%"PRIu64
                " fallbacks=%"PRIu64" callbacks=%"PRIu64
                " empty_acquires=%"PRIu64" current_image=%d current_egl=%d\n",
                p->raw_retire_maps, p->raw_retire_reaped,
                held_before, p->retired_count, p->raw_retire_waits,
                p->raw_retire_fallbacks, p->image_callbacks,
                p->empty_acquires, p->image != NULL, p->egl_image != NULL);
    }

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;

    if (p->raw_yuv && !p->direct_yuv) {
        if (p->raw_timer_query)
            gl->DeleteQueries(1, &p->raw_timer_query);
        raw_destroy_targets(mapper);
        if (p->raw_vao)
            gl->DeleteVertexArrays(1, &p->raw_vao);
        if (p->raw_program)
            gl->DeleteProgram(p->raw_program);
    } else {
        ra_tex_free(mapper->ra, &mapper->tex[0]);
    }
    MP_WARN(mapper, "P5_IMAGE_FINAL acquired=%"PRIu64" deleted=%"PRIu64
            " current_image=%d retired=%d callbacks=%"PRIu64
            " empty_acquires=%"PRIu64" retire=%d\n",
            p->image_acquired, p->image_deleted, p->image != NULL,
            p->retired_count, p->image_callbacks, p->empty_acquires,
            p->raw_retire);
    char image_final_line[256];
    snprintf(image_final_line, sizeof(image_final_line),
             "P5_IMAGE_FINAL acquired=%"PRIu64" deleted=%"PRIu64
             " current_image=%d retired=%d callbacks=%"PRIu64
             " empty_acquires=%"PRIu64" retire=%d",
             p->image_acquired, p->image_deleted, p->image != NULL,
             p->retired_count, p->image_callbacks, p->empty_acquires,
             p->raw_retire);
    raw_stage(image_final_line);
    mp_mutex_destroy(&p->lock);
    mp_cond_destroy(&p->cond);
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    if (p->direct_retire && p->image && p->egl_image &&
        p->raw_submitted && p->gl_texture && mapper->tex[0]) {
        GL *gl = ra_gl_get(mapper->ra);
        p->raw_sample_fence = gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (p->raw_sample_fence)
            gl->Flush();
    }
    if (p->raw_retire && p->image && p->egl_image &&
        p->raw_submitted && p->raw_sample_fence && p->gl_texture) {
        if (p->retired_count >= MP_ARRAY_SIZE(p->retired))
            raw_retire_reap(mapper, 1);
        mp_assert(p->retired_count < MP_ARRAY_SIZE(p->retired));
        p->retired[p->retired_count++] = (struct retired_image){
            .source = p->direct_retire ? mp_image_new_ref(mapper->src) : NULL,
            .image = p->image,
            .egl_image = p->egl_image,
            .display = eglGetCurrentDisplay(),
            .fence = p->raw_sample_fence,
            .texture = p->gl_texture,
            .wrapped_tex = p->direct_retire ? mapper->tex[0] : NULL,
        };
        if (p->direct_retire)
            mapper->tex[0] = NULL;
        p->image = NULL;
        p->egl_image = NULL;
        p->raw_sample_fence = NULL;
        p->gl_texture = 0;
        p->raw_submitted = false;
        p->raw_perf_pending = false;
        return;
    }

    if (p->raw_yuv && p->image && p->raw_submitted) {
        int64_t start_ns = p->raw_perf && p->raw_perf_pending ? mp_time_ns() : 0;
        GL *gl = ra_gl_get(mapper->ra);
        if (!gl->Finish) {
            raw_stage("finish_pointer_missing");
            abort();
        }
        bool sample_complete = false;
        int wait_result = -1;
        if (p->raw_early_fence && p->raw_sample_fence) {
            wait_result = gl->ClientWaitSync(p->raw_sample_fence,
                                             GL_SYNC_FLUSH_COMMANDS_BIT,
                                             1000000000ULL);
            sample_complete = wait_result == GL_ALREADY_SIGNALED ||
                              wait_result == GL_CONDITION_SATISFIED;
        }
        if (!sample_complete) {
            int64_t section_wall = p->section_perf ? mp_time_ns() : 0;
            int64_t section_cpu = p->section_perf ? p5_mapper_cpu_ns() : 0;
            gl->Finish();
            if (p->section_perf) {
                p->section_finish_wall_ns += mp_time_ns() - section_wall;
                p->section_finish_cpu_ns += p5_mapper_cpu_ns() - section_cpu;
            }
        }
        if (p->raw_sample_fence) {
            gl->DeleteSync(p->raw_sample_fence);
            p->raw_sample_fence = NULL;
        }
        if (p->raw_perf && p->raw_perf_pending) {
            int64_t finish_ns = mp_time_ns() - start_ns;
            GLuint64 gpu_draw_ns = 0;
            GLint gpu_disjoint = 0;
            if (p->raw_timer_pending) {
                gl->GetIntegerv(GL_GPU_DISJOINT_EXT, &gpu_disjoint);
                if (!gpu_disjoint)
                    gl->GetQueryObjectui64v(p->raw_timer_query, GL_QUERY_RESULT,
                                            &gpu_draw_ns);
                p->raw_timer_pending = false;
            }
            p->raw_perf_frames++;
            if (p->raw_perf_frames <= 20 || p->raw_perf_frames % 50 == 0) {
                void *log_lib = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
                typedef int (*log_print_fn)(int, const char *, const char *, ...);
                log_print_fn log_print = log_lib ? dlsym(log_lib, "__android_log_print") : NULL;
                if (log_print)
                    log_print(6, "P5_RAW_TIME", "frame=%"PRIu64" submit_us=%"PRId64" pre_finish_us=%"PRId64" release_wait_us=%"PRId64" wait_result=0x%x fence=%d gpu_draw_us=%"PRIu64" disjoint=%d",
                              p->raw_perf_frames, p->raw_perf_submit_ns / 1000,
                              p->raw_perf_split_ns / 1000, finish_ns / 1000,
                              wait_result, p->raw_early_fence,
                              (uint64_t)(gpu_draw_ns / 1000),
                              gpu_disjoint);
                if (log_lib)
                    dlclose(log_lib);
            }
            p->raw_perf_window_frames++;
            p->raw_perf_submit_window_sum_ns += p->raw_perf_submit_ns;
            p->raw_perf_finish_window_sum_ns += finish_ns;
            p->raw_perf_submit_window_max_ns = MPMAX(p->raw_perf_submit_window_max_ns,
                                                     p->raw_perf_submit_ns);
            p->raw_perf_finish_window_max_ns = MPMAX(p->raw_perf_finish_window_max_ns,
                                                     finish_ns);
            if (p->raw_perf_frames == 1 || p->raw_perf_frames == 10 ||
                p->raw_perf_frames == 50 || p->raw_perf_frames % 250 == 0) {
                MP_WARN(mapper, "P5_RAW_PERF frames=%"PRIu64" window=%"PRIu64" map_submit_cpu_us_avg=%"PRId64" map_submit_cpu_us_max=%"PRId64" unmap_finish_wait_us_avg=%"PRId64" unmap_finish_wait_us_max=%"PRId64"\n",
                        p->raw_perf_frames, p->raw_perf_window_frames,
                        p->raw_perf_submit_window_sum_ns /
                            (int64_t)p->raw_perf_window_frames / 1000,
                        p->raw_perf_submit_window_max_ns / 1000,
                        p->raw_perf_finish_window_sum_ns /
                            (int64_t)p->raw_perf_window_frames / 1000,
                        p->raw_perf_finish_window_max_ns / 1000);
                p->raw_perf_window_frames = 0;
                p->raw_perf_submit_window_sum_ns = 0;
                p->raw_perf_submit_window_max_ns = 0;
                p->raw_perf_finish_window_sum_ns = 0;
                p->raw_perf_finish_window_max_ns = 0;
            }
            p->raw_perf_pending = false;
        }
    }

    if (p->egl_image) {
        p->DestroyImageKHR(eglGetCurrentDisplay(), p->egl_image);
        p->egl_image = 0;
    }
    if (p->image) {
        o->AImage_delete(p->image);
        p->image_deleted++;
        p->image = NULL;
    }
    p->raw_submitted = false;
    if (p->section_perf && ++p->section_frames % 250 == 0) {
        p5_section_log("P5_SECTION_MAP", "frames=250 acquire_wall_us=%"PRId64" acquire_cpu_us=%"PRId64" create_wall_us=%"PRId64" create_cpu_us=%"PRId64" bind_wall_us=%"PRId64" bind_cpu_us=%"PRId64" finish_wall_us=%"PRId64" finish_cpu_us=%"PRId64,
                p->section_acquire_wall_ns / 1000, p->section_acquire_cpu_ns / 1000,
                p->section_create_wall_ns / 1000, p->section_create_cpu_ns / 1000,
                p->section_bind_wall_ns / 1000, p->section_bind_cpu_ns / 1000,
                p->section_finish_wall_ns / 1000, p->section_finish_cpu_ns / 1000);
        p->section_acquire_wall_ns = p->section_acquire_cpu_ns = 0;
        p->section_create_wall_ns = p->section_create_cpu_ns = 0;
        p->section_bind_wall_ns = p->section_bind_cpu_ns = 0;
        p->section_finish_wall_ns = p->section_finish_cpu_ns = 0;
    }
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);
    p->raw_perf_pending = false;
    p->raw_submitted = false;

    if (p->raw_retire) {
        // A redraw may map one MediaCodec output more than once. Reuse its
        // still-owned external texture instead of acquiring a nonexistent
        // second AImage for the same codec buffer.
        if (p->direct_retire) {
            for (int i = 0; i < p->retired_count; i++) {
                struct retired_image *held = &p->retired[i];
                if (!held->source ||
                    held->source->planes[3] != mapper->src->planes[3] ||
                    held->source->pts != mapper->src->pts)
                    continue;
                struct retired_image image = *held;
                for (int j = i + 1; j < p->retired_count; j++)
                    p->retired[j - 1] = p->retired[j];
                p->retired[--p->retired_count] = (struct retired_image){0};
                gl->DeleteSync(image.fence);
                p->image = image.image;
                p->egl_image = image.egl_image;
                p->gl_texture = image.texture;
                mapper->tex[0] = image.wrapped_tex;
                mp_image_unrefp(&image.source);
                p->raw_submitted = true;
                p->raw_retire_maps++;
                return 0;
            }
        }
        raw_retire_reap(mapper, 1);
        p->raw_retire_maps++;
        if (!p->gl_texture && !raw_create_next_external_texture(mapper))
            return -1;
        if (p->direct_retire && !mapper->tex[0]) {
            struct ra_tex_params params = {
                .dimensions = 2,
                .w = mapper->src_params.w,
                .h = mapper->src_params.h,
                .d = 1,
                .format = ra_find_unorm_format(mapper->ra, 1, 4),
                .render_src = true,
                .src_linear = true,
                .external_oes = true,
                .external_yuv = true,
            };
            mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params,
                                                    p->gl_texture);
            if (!mapper->tex[0])
                return -1;
        }
    }

    if (p->raw_yuv &&
        (mapper->src_params.repr.sys != PL_COLOR_SYSTEM_DOLBYVISION ||
         !mapper->src_params.repr.dovi || !mapper->src->dovi)) {
        MP_ERR(mapper, "P5 raw YUV frame lacks DOVI metadata\n");
        return -1;
    }
    if (p->raw_yuv && !p->rpu_hash_logged &&
        fabs(mapper->src->pts - 10.0) < 0.00001) {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.media_kit.p5_post_rpu_probe", prop) > 0 &&
            !strcmp(prop, "1")) {
            int found = 0;
            for (int i = 0; i < mapper->src->num_ff_side_data; i++) {
                struct mp_ff_side_data *sd = &mapper->src->ff_side_data[i];
                if (sd->type != AV_FRAME_DATA_DOVI_RPU_BUFFER || !sd->buf)
                    continue;
                uint32_t hash = 2166136261U;
                for (size_t j = 0; j < sd->buf->size; j++)
                    hash = (hash ^ sd->buf->data[j]) * 16777619U;
                char line[160];
                snprintf(line, sizeof(line),
                         "P5_RPU_ID pts=%.9f size=%zu hash=%08x",
                         mapper->src->pts, sd->buf->size, hash);
                raw_stage(line);
                found++;
            }
            if (!found)
                raw_stage("P5_RPU_ID pts=10 missing_raw_side_data");
            p->rpu_hash_logged = true;
        }
    }

    {
        if (mapper->src->imgfmt != IMGFMT_MEDIACODEC)
            return -1;
        AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)mapper->src->planes[3];
        if (p->image_timeline && p->image_acquired < 12)
            p5_section_log("P5_TIMELINE", "release_begin pts=%.9f mono_us=%"PRId64,
                           mapper->src->pts, mp_time_ns() / 1000);
        int release_ret = av_mediacodec_release_buffer(buffer, 1);
        if (p->image_timeline && p->image_acquired < 12)
            p5_section_log("P5_TIMELINE",
                           "release_end pts=%.9f src=%p codec_buffer=%p ret=%d mono_us=%"PRId64,
                           mapper->src->pts, (void *)mapper->src,
                           (void *)buffer, release_ret, mp_time_ns() / 1000);
    }

    // A queued callback does not guarantee an image is still available.
    // Retry this transient state without spinning the VO.
    media_status_t ret = AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
    int64_t acquire_wall = p->section_perf ? mp_time_ns() : 0;
    int64_t acquire_cpu = p->section_perf ? p5_mapper_cpu_ns() : 0;
    bool image_notified = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        mp_mutex_lock(&p->lock);
        if (!p->image_available)
            mp_cond_timedwait(&p->cond, &p->lock, MP_TIME_MS_TO_NS(10));
        image_notified |= p->image_available;
        p->image_available = false;
        mp_mutex_unlock(&p->lock);

        int64_t attempt_us = p->image_timeline ? mp_time_ns() / 1000 : 0;
        ret = o->AImageReader_acquireLatestImage(o->reader, &p->image);
        if (p->image_timeline && (p->image_acquired < 12 ||
                                  ret != AMEDIA_OK || p->timeline_followup)) {
            mp_mutex_lock(&p->lock);
            uint64_t callbacks_snapshot = p->image_callbacks;
            mp_mutex_unlock(&p->lock);
            p5_section_log("P5_TIMELINE",
                           "acquire attempt=%d pts=%.9f start_us=%"PRId64
                           " end_us=%"PRId64" ret=%d callbacks=%"PRIu64,
                           attempt + 1, mapper->src->pts, attempt_us,
                           mp_time_ns() / 1000, ret, callbacks_snapshot);
        }
        if (ret != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
            break;
        p->empty_acquires++;
    }
    if (p->section_perf) {
        p->section_acquire_wall_ns += mp_time_ns() - acquire_wall;
        p->section_acquire_cpu_ns += p5_mapper_cpu_ns() - acquire_cpu;
    }
    if (ret != AMEDIA_OK) {
        // The ordinary OES mapper can be asked to draw the same codec frame
        // again without a fresh ImageReader callback. Keep its previous
        // texture, as the upstream mapper does on a callback timeout.
        if (!p->raw_yuv && !image_notified)
            return 0;
        if (p->image_timeline) {
            p->timeline_failed_pts = mapper->src->pts;
            p->timeline_failed_us = mp_time_ns() / 1000;
            p->timeline_followup = true;
        }
        mp_mutex_lock(&p->lock);
        uint64_t callbacks = p->image_callbacks;
        bool notification_pending = p->image_available;
        mp_mutex_unlock(&p->lock);
        MP_ERR(mapper, "acquireLatestImage failed after retry: %d pts=%.9f callbacks=%"PRIu64
               " empty_acquires=%"PRIu64" notification_pending=%d retired=%d maps=%"PRIu64"\n",
               ret, mapper->src->pts, callbacks, p->empty_acquires,
               notification_pending, p->retired_count, p->raw_retire_maps);
        return -1;
    }
    mp_assert(p->image);
    if (p->image_timeline &&
        (p->image_acquired < 12 || p->timeline_followup)) {
        int64_t image_ns = -1;
        media_status_t image_ts_ret = o->AImage_getTimestamp(p->image, &image_ns);
        p5_section_log("P5_TIMELINE",
                       "acquire_ok target_pts=%.9f image_ts_ns=%"PRId64
                       " ts_ret=%d mono_us=%"PRId64
                       " prior_failed_pts=%.9f since_failed_us=%"PRId64,
                       mapper->src->pts, image_ns, image_ts_ret,
                       mp_time_ns() / 1000, p->timeline_failed_pts,
                       p->timeline_followup ?
                           mp_time_ns() / 1000 - p->timeline_failed_us : -1);
        if (p->timeline_followup)
            p->timeline_followup = false;
    }
    p->image_acquired++;

    AHardwareBuffer *hwbuf = NULL;
    ret = o->AImage_getHardwareBuffer(p->image, &hwbuf);
    if (ret != AMEDIA_OK) {
        MP_ERR(mapper, "getHardwareBuffer failed: %d\n", ret);
        return -1;
    }
    mp_assert(hwbuf);
    if (p->ahb_identity_probe && ++p->ahb_identity_samples <= 120)
        p5_section_log("P5_AHB_IDENTITY", "sample=%"PRIu64" pts=%.9f ahb=%p image=%p",
                       p->ahb_identity_samples, mapper->src->pts,
                       (void *)hwbuf, (void *)p->image);

    // Update texture size since it may differ
    AHardwareBuffer_Desc d;
    o->AHardwareBuffer_describe(hwbuf, &d);
    if (p->raw_format_probe) {
        if (!p->raw_format_probe_logged) {
            MP_ERR(mapper, "P5_RAW_FORMAT width=%u height=%u layers=%u format=0x%x usage=%"PRIu64" stride=%u\n",
                   d.width, d.height, d.layers, d.format,
                   (uint64_t)d.usage, d.stride);
            p->raw_format_probe_logged = true;
        }
        return -1;
    }
    AImageCropRect crop = {0};
    if (p->raw_yuv) {
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
            MP_ERR(mapper, "P5 raw YUV identity/crop mismatch: frame_pts=%.9f image_ns=%"PRId64" ts=%d crop=%d,%d,%d,%d crop_status=%d buffer=%ux%u expected=%dx%d\n",
                   mapper->src->pts, image_ns, ts_ret,
                   crop.left, crop.top, crop.right, crop.bottom, crop_ret,
                   d.width, d.height, mapper->src_params.w,
                   mapper->src_params.h);
            return -1;
        }
        if (!p->direct_yuv &&
            !raw_create_targets(mapper, mapper->src_params.w,
                                mapper->src_params.h))
            return -1;
        if (p->raw_gl_preimport_probe) {
            GLenum err = gl->GetError();
            if (!p->raw_gl_preimport_probe_logged) {
                MP_ERR(mapper, "P5_RAW_GL_PREIMPORT error=0x%x context=%p display=%p format=0x%x\n",
                       err, eglGetCurrentContext(), eglGetCurrentDisplay(), d.format);
                p->raw_gl_preimport_probe_logged = true;
            }
            return -1;
        }
    } else if (mapper->tex[0]->params.w != d.width ||
               mapper->tex[0]->params.h != d.height) {
        MP_VERBOSE(p, "Texture dimensions changed to %dx%d\n", d.width, d.height);
        mapper->tex[0]->params.w = d.width;
        mapper->tex[0]->params.h = d.height;
    }

    const EGLint basic_attribs[] = {EGL_NONE};
    const EGLint raw_attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    int64_t create_wall = p->section_perf ? mp_time_ns() : 0;
    int64_t create_cpu = p->section_perf ? p5_mapper_cpu_ns() : 0;
    EGLClientBuffer buf = p->GetNativeClientBufferANDROID(hwbuf);
    if (!buf)
        return -1;
    p->egl_image = p->CreateImageKHR(eglGetCurrentDisplay(),
        EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf,
        p->raw_yuv ? raw_attribs : basic_attribs);
    if (!p->egl_image)
        return -1;
    if (p->section_perf) {
        p->section_create_wall_ns += mp_time_ns() - create_wall;
        p->section_create_cpu_ns += p5_mapper_cpu_ns() - create_cpu;
    }

    if (p->raw_yuv) {
        // Bound the optional raw FBO diagnostics even when direct sampling
        // is unavailable. The old first-frame log also advanced this count.
        if (p->raw_logged <= 5)
            p->raw_logged++;
        GLint old_active, old_external;
        gl->GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
        gl->ActiveTexture(GL_TEXTURE0);
        gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_external);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
        int64_t bind_wall = p->section_perf ? mp_time_ns() : 0;
        int64_t bind_cpu = p->section_perf ? p5_mapper_cpu_ns() : 0;
        p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, p->egl_image);
        if (p->section_perf) {
            p->section_bind_wall_ns += mp_time_ns() - bind_wall;
            p->section_bind_cpu_ns += p5_mapper_cpu_ns() - bind_cpu;
        }
        int64_t submit_start_ns = p->raw_perf ? mp_time_ns() : 0;
        GLenum import_error = gl->GetError();
        bool ok = import_error == GL_NO_ERROR &&
                  (p->direct_yuv || raw_render(mapper, &crop, d.width, d.height));
        if (ok && p->direct_yuv)
            p->raw_submitted = true; // unmap waits for libplacebo's later sample
        if (ok && p->raw_early_fence)
            p->raw_sample_fence = gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_external);
        gl->ActiveTexture(old_active);
        if (ok && p->raw_perf) {
            p->raw_perf_submit_ns = mp_time_ns() - submit_start_ns;
            p->raw_perf_split_ns = 0;
            if (p->raw_split) {
                int64_t split_start_ns = mp_time_ns();
                gl->Finish();
                p->raw_perf_split_ns = mp_time_ns() - split_start_ns;
                p->raw_split_frames++;
                if (p->raw_split_frames <= 20 || p->raw_split_frames % 50 == 0) {
                    void *log_lib = dlopen("liblog.so", RTLD_NOW);
                    void (*log_print)(int, const char *, const char *, ...) =
                        log_lib ? dlsym(log_lib, "__android_log_print") : NULL;
                    if (log_print)
                        log_print(6, "P5_RAW_SPLIT", "frame=%"PRIu64" wait_us=%"PRId64" submit_us=%"PRId64" retire=%d",
                                  p->raw_split_frames, p->raw_perf_split_ns / 1000,
                                  p->raw_perf_submit_ns / 1000, p->raw_retire);
                    if (log_lib)
                        dlclose(log_lib);
                }
            }
            p->raw_perf_pending = true;
        }
        if (!ok)
            return -1;
    } else {
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
        p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, p->egl_image);
        gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
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
