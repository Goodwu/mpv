/*
 * Copyright (C) 2021 Niklas Haas
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

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/lut.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/utils/frame_queue.h>

#include "config.h"
#include "common/common.h"
#include "misc/io_utils.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "stream/stream.h"
#include "sub/draw_bmp.h"
#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/mks_frame_gpu_timer.h"
#include "placebo/utils.h"
#include "gpu/context.h"
#include "gpu/hwdec.h"
#include "gpu/video.h"
#include "gpu/video_shaders.h"
#include "sub/osd.h"
#include "gpu_next/context.h"
#include "mks_thermal.h"
#include "yuv_fusion_gate.h"
#include "yuv_compare_vo.h"
#include "yuv_atlas_anchor.h"
#include "yuv_atlas_read.h"
#include "opengl/mks_yuv_compare.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
#include <libplacebo/opengl.h>
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
#include <libavutil/sha.h>
#include <libavutil/mem.h>
#endif
#include "video/out/opengl/ra_gl.h"
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
#include <libplacebo/d3d11.h>
#include "video/out/d3d11/ra_d3d11.h"
#include "osdep/windows_utils.h"
#endif


struct osd_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct osd_state {
    struct osd_entry entries[MAX_OSD_PARTS];
    struct pl_overlay overlays[MAX_OSD_PARTS];
};

struct scaler_params {
    struct pl_filter_config config;
};

struct user_hook {
    char *path;
    const struct pl_hook *hook;
};

struct user_lut {
    char *opt;
    char *path;
    int type;
    struct pl_custom_lut *lut;
};

struct frame_info {
    int count;
    struct pl_dispatch_info info[VO_PASS_PERF_MAX];
};

struct cache {
    struct mp_log *log;
    struct mpv_global *global;
    char *dir;
    const char *name;
    size_t size_limit;
    pl_cache cache;
};

struct priv {
    struct mp_log *log;
    struct mks_thermal_trace thermal;
    struct mpv_global *global;
    struct ra_ctx *ra_ctx;
    struct gpu_ctx *context;
    struct ra_hwdec_ctx hwdec_ctx;
    struct ra_hwdec_mapper *hwdec_mapper;

    // Allocated DR buffers
    mp_mutex dr_lock;
    pl_buf *dr_buffers;
    int num_dr_buffers;

    pl_log pllog;
    pl_gpu gpu;
    pl_renderer rr;
    pl_queue queue;
    pl_swapchain sw;
    pl_tex fusion_target;
    uint64_t fusion_generation;
    uint64_t fusion_gate_rejects, fusion_draw_failures;
    bool fusion_presentation_guard, fusion_disabled, fusion_fault_fired;
    bool fusion_frame_trace;
    bool compare_requested, compare_enabled, compare_repeat, compare_sample_tap;
    bool compare_invariant_b;
    bool compare_uv_oracle, compare_uv_oracle_use_a;
    bool compare_uv_probe;
    bool compare_uv_single;
    bool compare_quad_order, compare_quad_reverse;
    bool compare_atlas, atlas_reuse;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
    pl_opengl_uv_atlas_anchor atlas_anchor;
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    pl_opengl_uv_atlas_cache atlas_cache;
    pl_opengl_uv_atlas_read atlas_read;
    struct pl_opengl_uv_atlas_cache_info atlas_cache_info;
    enum mp_yuv_atlas_read_phase atlas_phase;
    unsigned int atlas_ordinal;
    double atlas_second_pts, atlas_seed_pts;
    uint64_t atlas_seed_id, atlas_seed_frame_id, atlas_seed_frame_token;
    uint64_t atlas_seed_image, atlas_input_image;
    uint64_t atlas_renderer_id, atlas_renderer_generation;
    char atlas_nonce[33], atlas_seed_manifest[128], atlas_seed_sha[65], atlas_signature[65];
    bool atlas_wait_expired;
#endif
#endif
    // Default-off production UV table for the output fusion candidate:
    // OFF -> SEEDING (ordinary path renders while a zero-readback anchor
    // records the validated UV table) -> READY (table attached; generated
    // fusion final passes sample through it). Any failure returns to OFF and
    // the ordinary path; the displayed frame is never a table experiment.
    bool fusion_table_enabled;
    // True when the table property was set at init: production mode pairs
    // fusion with the table, so losing the table must also stop fusing
    // instead of silently reverting to the un-tabled (quality-regressing)
    // fusion path. An explicit table-off config keeps plain fusion.
    bool fusion_table_was_enabled;
    enum { FT_OFF, FT_SEEDING, FT_READY } fusion_table_state;
    pl_opengl_uv_atlas_anchor table_anchor;
    pl_opengl_uv_atlas_cache table_cache;
    uint64_t table_serial;
    int table_seed_frames, table_failures;
    // Consumer-side budget: fused-frame failures after a READY table. NOT
    // reset by a successful seed — only a successfully committed fused frame
    // proves recovery — so a seed/fail loop cannot spin forever.
    int table_consume_failures;
    // Controlled transition harness (diagnostic property
    // debug.mks.yuv_table_transition=<delta>): applies a REAL VO-level
    // resize (dwidth/dheight + resize(vo) -> reconfig chain) so a READY
    // table meets a live geometry change. Default 0 = never active.
    int table_transition_delta;
    // Seed-time identity snapshot: the table is only valid for this exact
    // source geometry + scaler selection. Any later drift detaches it.
    int table_src_w, table_src_h;
    int table_scaler_kernel, table_dscale_kernel;
    bool table_linear_up, table_linear_down, table_correct_down;
    bool compare_started, compare_active, compare_a_ready;
    bool compare_terminal, compare_fatal_context, compare_frame_open;
    double compare_target_pts, compare_source_pts, compare_current_pts;
    int compare_width, compare_height;
    uint64_t compare_id, compare_generation, compare_error_epoch;
    uint64_t compare_frame_id, compare_frame_token;
    bool compare_epoch_valid;
    int compare_x, compare_y, compare_w, compare_h;
    int64_t compare_deadline;
    uint32_t *compare_words;
    size_t compare_num_words;
    uint32_t *compare_probe_words;
    size_t compare_num_probe_words;
    int fusion_fault; // explicit one-shot diagnostics: 1=commit, 2=GL error
    pl_fmt osd_fmt[SUBBITMAP_COUNT];
    pl_tex *sub_tex;
    int num_sub_tex;

    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
    struct osd_state osd_state;

    uint64_t last_id;
    uint64_t osd_sync;
    double last_pts;
    bool is_interpolated;
    bool want_reset;
    bool frame_pending;

    pl_options pars;
    struct m_config_cache *opts_cache;
    struct m_config_cache *next_opts_cache;
    struct gl_next_opts *next_opts;
    struct cache shader_cache, icc_cache;
    struct mp_csp_equalizer_state *video_eq;
    struct scaler_params scalers[SCALER_COUNT];
    const struct pl_hook **hooks; // storage for `params.hooks`
    enum pl_color_levels output_levels;

    struct pl_icc_params icc_params;
    char *icc_path;
    pl_icc_object icc_profile;

    // Cached shaders, preserved across options updates
    struct user_hook *user_hooks;
    int num_user_hooks;

    // Performance data of last frame
    struct frame_info perf_fresh;
    struct frame_info perf_redraw;

    // Opt-in snapshots of libplacebo's asynchronous GPU timing histories.
    bool gpu_timing;
    int64_t gpu_timing_next_ns;
    uint64_t gpu_timing_frame_id;
    double gpu_timing_pts;

    struct mp_image_params target_params;
};

static void update_render_options(struct vo *vo);
static void update_lut(struct priv *p, struct user_lut *lut);

struct gl_next_opts {
    bool delayed_peak;
    int sub_hdr_peak;
    int image_subs_hdr_peak;
    int border_background;
    float background_blur_radius;
    float corner_rounding;
    bool inter_preserve;
    struct user_lut lut;
    struct user_lut image_lut;
    struct user_lut target_lut;
    int target_hint;
    int target_hint_mode;
    bool target_hint_strict;
    // 0 = auto: only for frames imported through the Android external YUV
    // hwdec path; 1 = yes: any Profile 5 frame; -1 = no.
    int dovi_p5_fast_path;
    char **raw_opts;
};

const struct m_opt_choice_alternatives lut_types[] = {
    {"auto",        PL_LUT_UNKNOWN},
    {"native",      PL_LUT_NATIVE},
    {"normalized",  PL_LUT_NORMALIZED},
    {"conversion",  PL_LUT_CONVERSION},
    {0}
};

#define OPT_BASE_STRUCT struct gl_next_opts
const struct m_sub_options gl_next_conf = {
    .opts = (const struct m_option[]) {
        {"sub-hdr-peak", OPT_CHOICE(sub_hdr_peak, {"sdr", PL_COLOR_SDR_WHITE}),
            M_RANGE(10, 10000)},
        {"image-subs-hdr-peak", OPT_CHOICE(image_subs_hdr_peak, {"sdr", PL_COLOR_SDR_WHITE},
            {"video", -1}),  M_RANGE(10, 10000)},
        {"allow-delayed-peak-detect", OPT_BOOL(delayed_peak)},
        {"border-background", OPT_CHOICE(border_background,
            {"none",  BACKGROUND_NONE},
            {"color", BACKGROUND_COLOR},
            {"tiles", BACKGROUND_TILES}
#if PL_API_VER < 355
            )},
#else
            ,{"blur", BACKGROUND_BLUR})},
        {"background-blur-radius", OPT_FLOAT(background_blur_radius)},
#endif
        {"corner-rounding", OPT_FLOAT(corner_rounding), M_RANGE(0, 1)},
        {"interpolation-preserve", OPT_BOOL(inter_preserve)},
        {"lut", OPT_STRING(lut.opt), .flags = M_OPT_FILE},
        {"lut-type", OPT_CHOICE_C(lut.type, lut_types)},
        {"image-lut", OPT_STRING(image_lut.opt), .flags = M_OPT_FILE},
        {"image-lut-type", OPT_CHOICE_C(image_lut.type, lut_types)},
        {"target-lut", OPT_STRING(target_lut.opt), .flags = M_OPT_FILE},
        {"target-colorspace-hint", OPT_CHOICE(target_hint, {"auto", -1}, {"no", 0}, {"yes", 1})},
        {"target-colorspace-hint-mode", OPT_CHOICE(target_hint_mode, {"target", 0}, {"source", 1}, {"source-dynamic", 2})},
        {"target-colorspace-hint-strict", OPT_BOOL(target_hint_strict)},
        {"dovi-p5-fast-path", OPT_CHOICE(dovi_p5_fast_path,
            {"auto", 0}, {"yes", 1}, {"no", -1})},
        // No `target-lut-type` because we don't support non-RGB targets
        {"libplacebo-opts", OPT_KEYVALUELIST(raw_opts)},
        {0},
    },
    .defaults = &(struct gl_next_opts) {
        .border_background = BACKGROUND_COLOR,
        .background_blur_radius = 16.0f,
        .inter_preserve = true,
        .sub_hdr_peak = PL_COLOR_SDR_WHITE,
        .image_subs_hdr_peak = PL_COLOR_SDR_WHITE,
        .target_hint = -1,
        .target_hint_strict = true,
        .dovi_p5_fast_path = 0,
    },
    .size = sizeof(struct gl_next_opts),
    .change_flags = UPDATE_VIDEO,
};

static pl_buf get_dr_buf(struct priv *p, const uint8_t *ptr)
{
    mp_mutex_lock(&p->dr_lock);

    for (int i = 0; i < p->num_dr_buffers; i++) {
        pl_buf buf = p->dr_buffers[i];
        if (ptr >= buf->data && ptr < buf->data + buf->params.size) {
            mp_mutex_unlock(&p->dr_lock);
            return buf;
        }
    }

    mp_mutex_unlock(&p->dr_lock);
    return NULL;
}

static void free_dr_buf(void *opaque, uint8_t *data)
{
    struct priv *p = opaque;
    mp_mutex_lock(&p->dr_lock);

    for (int i = 0; i < p->num_dr_buffers; i++) {
        if (p->dr_buffers[i]->data == data) {
            pl_buf_destroy(p->gpu, &p->dr_buffers[i]);
            MP_TARRAY_REMOVE_AT(p->dr_buffers, p->num_dr_buffers, i);
            mp_mutex_unlock(&p->dr_lock);
            return;
        }
    }

    MP_ASSERT_UNREACHABLE();
}

static struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h,
                                  int stride_align, int flags)
{
    struct priv *p = vo->priv;
    pl_gpu gpu = p->gpu;
    if (!gpu->limits.thread_safe || !gpu->limits.max_mapped_size)
        return NULL;

    if ((flags & VO_DR_FLAG_HOST_CACHED) && !gpu->limits.host_cached)
        return NULL;

    stride_align = mp_lcm(stride_align, gpu->limits.align_tex_xfer_pitch);
    stride_align = mp_lcm(stride_align, gpu->limits.align_tex_xfer_offset);
    int size = mp_image_get_alloc_size(imgfmt, w, h, stride_align);
    if (size < 0)
        return NULL;

    pl_buf buf = pl_buf_create(gpu, &(struct pl_buf_params) {
        .memory_type = PL_BUF_MEM_HOST,
        .host_mapped = true,
        .size = size + stride_align,
    });

    if (!buf)
        return NULL;

    struct mp_image *mpi = mp_image_from_buffer(imgfmt, w, h, stride_align,
                                                buf->data, buf->params.size,
                                                p, free_dr_buf);
    if (!mpi) {
        pl_buf_destroy(gpu, &buf);
        return NULL;
    }

    mp_mutex_lock(&p->dr_lock);
    MP_TARRAY_APPEND(p, p->dr_buffers, p->num_dr_buffers, buf);
    mp_mutex_unlock(&p->dr_lock);

    return mpi;
}

static void update_overlays(struct vo *vo, struct mp_osd_res res,
                            int flags, enum pl_overlay_coords coords,
                            struct osd_state *state, struct pl_frame *frame,
                            struct mp_image *src)
{
    struct priv *p = vo->priv;
    double pts = src ? src->pts : 0;
    struct sub_bitmap_list *subs = osd_render(vo->osd, res, pts, flags, mp_draw_sub_formats);

    frame->overlays = state->overlays;
    frame->num_overlays = 0;

    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;
        struct osd_entry *entry = &state->entries[item->render_index];
        pl_fmt tex_fmt = p->osd_fmt[item->format];
        if (!entry->tex)
            MP_TARRAY_POP(p->sub_tex, p->num_sub_tex, &entry->tex);
        bool ok = pl_tex_recreate(p->gpu, &entry->tex, &(struct pl_tex_params) {
            .format = tex_fmt,
            .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
            .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
            .host_writable = true,
            .sampleable = true,
        });
        if (!ok) {
            MP_ERR(vo, "Failed recreating OSD texture!\n");
            break;
        }
        ok = pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
            .tex        = entry->tex,
            .rc         = { .x1 = item->packed_w, .y1 = item->packed_h, },
            .row_pitch  = item->packed->stride[0],
            .ptr        = item->packed->planes[0],
        });
        if (!ok) {
            MP_ERR(vo, "Failed uploading OSD texture!\n");
            break;
        }

        entry->num_parts = 0;
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            if (b->dw == 0 || b->dh == 0)
                continue;
            uint32_t c = b->libass.color;
            struct pl_overlay_part part = {
                .src = { b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h },
                .dst = { b->x, b->y, b->x + b->dw, b->y + b->dh },
                .color = {
                    (c >> 24) / 255.0f,
                    ((c >> 16) & 0xFF) / 255.0f,
                    ((c >> 8) & 0xFF) / 255.0f,
                    (255 - (c & 0xFF)) / 255.0f,
                }
            };
            MP_TARRAY_APPEND(p, entry->parts, entry->num_parts, part);
        }

        struct pl_overlay *ol = &state->overlays[frame->num_overlays++];
        *ol = (struct pl_overlay) {
            .tex = entry->tex,
            .parts = entry->parts,
            .num_parts = entry->num_parts,
            .color = {
                .primaries = PL_COLOR_PRIM_BT_709,
                .transfer = PL_COLOR_TRC_SRGB,
            },
            .coords = coords,
        };

        switch (item->format) {
        case SUBBITMAP_BGRA:
            ol->mode = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
            // Infer bitmap colorspace from source
            if (src) {
                ol->color = src->params.color;
                if (pl_color_transfer_is_hdr(ol->color.transfer)) {
                    if (!pl_color_transfer_is_hdr(frame->color.transfer)) {
                        // Tone mapping targets SDR white
                        ol->color.hdr = (struct pl_hdr_metadata) {
                            .max_luma = PL_COLOR_SDR_WHITE,
                        };
                    } else if (p->next_opts->image_subs_hdr_peak != -1) {
                        ol->color.hdr = (struct pl_hdr_metadata) {
                            .max_luma = p->next_opts->image_subs_hdr_peak,
                        };
                    }
                }
            }
            break;
        case SUBBITMAP_LIBASS:
            if (src && item->video_color_space && !pl_color_space_is_hdr(&src->params.color))
                ol->color = src->params.color;
            if (src && pl_color_transfer_is_hdr(frame->color.transfer)) {
                ol->color.hdr = (struct pl_hdr_metadata) {
                    .max_luma = p->next_opts->sub_hdr_peak,
                };
            }
            ol->mode = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
            break;
        }
    }

    talloc_free(subs);
}

struct frame_priv {
    struct vo *vo;
    struct osd_state subs;
    uint64_t osd_sync;
    struct ra_hwdec *hwdec;
    // Physical lease dims recorded at acquire after the published per-map
    // geometry verified as a full-frame, orientation-free target-size lease.
    // 0 keeps every consumer in the classic factor-1 source space.
    int lease_w, lease_h;
    // Acquire-local rescaled overlay copies (target-size leases only),
    // freed again by the matching release. The queue frame and the shared
    // subtitle state are never mutated.
    void *rescale_ctx;
};

static int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                  struct pl_bit_encoding *out_bits,
                                  enum mp_imgfmt imgfmt, bool use_uint)
{
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return 0;

    if (desc.flags & MP_IMGFLAG_HWACCEL)
        return 0; // HW-accelerated frames need to be mapped differently

    if (!(desc.flags & MP_IMGFLAG_NE))
        return 0; // GPU endianness follows the host's

    if (desc.flags & MP_IMGFLAG_PAL)
        return 0; // Palette formats (currently) not supported in libplacebo

    if ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV))
        return 0; // Floating-point YUV (currently) unsupported

    bool has_bits = false;
    bool any_padded = false;

    for (int p = 0; p < desc.num_planes; p++) {
        struct pl_plane_data *data = &out_data[p];
        struct mp_imgfmt_comp_desc sorted[MP_NUM_COMPONENTS];
        int num_comps = 0;
        if (desc.bpp[p] % 8)
            return 0; // Pixel size is not byte-aligned

        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != p)
                continue;

            data->component_map[num_comps] = c;
            sorted[num_comps] = desc.comps[c];
            num_comps++;

            // Sort components by offset order, while keeping track of the
            // semantic mapping in `data->component_map`
            for (int i = num_comps - 1; i > 0; i--) {
                if (sorted[i].offset >= sorted[i - 1].offset)
                    break;
                MPSWAP(struct mp_imgfmt_comp_desc, sorted[i], sorted[i - 1]);
                MPSWAP(int, data->component_map[i], data->component_map[i - 1]);
            }
        }

        uint64_t total_bits = 0;

        // Fill in the pl_plane_data fields for each component
        memset(data->component_size, 0, sizeof(data->component_size));
        for (int c = 0; c < num_comps; c++) {
            data->component_size[c] = sorted[c].size;
            data->component_pad[c] = sorted[c].offset - total_bits;
            total_bits += data->component_pad[c] + data->component_size[c];
            any_padded |= sorted[c].pad;

            // Ignore bit encoding of alpha channel
            if (!out_bits || data->component_map[c] == PL_CHANNEL_A)
                continue;

            struct pl_bit_encoding bits = {
                .sample_depth = data->component_size[c],
                .color_depth = sorted[c].size - abs(sorted[c].pad),
                .bit_shift = MPMAX(sorted[c].pad, 0),
            };

            if (!has_bits) {
                *out_bits = bits;
                has_bits = true;
            } else {
                if (!pl_bit_encoding_equal(out_bits, &bits)) {
                    // Bit encoding differs between components/planes,
                    // cannot handle this
                    *out_bits = (struct pl_bit_encoding) {0};
                    out_bits = NULL;
                }
            }
        }

        data->pixel_stride = desc.bpp[p] / 8;
        data->type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT)
                            ? PL_FMT_FLOAT
                            : (use_uint ? PL_FMT_UINT : PL_FMT_UNORM);
    }

    if (any_padded && !out_bits)
        return 0; // can't handle padded components without `pl_bit_encoding`

    return desc.num_planes;
}

static bool hwdec_reconfig(struct priv *p, struct ra_hwdec *hwdec,
                           const struct mp_image_params *par)
{
    if (p->hwdec_mapper) {
        if (mp_image_params_static_equal(par, &p->hwdec_mapper->src_params)) {
            p->hwdec_mapper->src_params.repr.dovi = par->repr.dovi;
            p->hwdec_mapper->dst_params.repr.dovi = par->repr.dovi;
            p->hwdec_mapper->src_params.color.hdr = par->color.hdr;
            p->hwdec_mapper->dst_params.color.hdr = par->color.hdr;
            return p->hwdec_mapper;
        } else {
            ra_hwdec_mapper_free(&p->hwdec_mapper);
        }
    }

    p->hwdec_mapper = ra_hwdec_mapper_create_with_caps(
        hwdec, par, RA_HWDEC_CAP_MAP_GEOMETRY);
    if (!p->hwdec_mapper) {
        MP_ERR(p, "Initializing texture for hardware decoding failed.\n");
        return NULL;
    }

    return p->hwdec_mapper;
}

// For RAs not based on ra_pl, this creates a new pl_tex wrapper
static pl_tex hwdec_get_tex(struct priv *p, int n)
{
    struct ra_tex *ratex = p->hwdec_mapper->tex[n];
    struct ra *ra = p->hwdec_mapper->ra;
    if (ra_pl_get(ra))
        return (pl_tex) ratex->priv;

#if HAVE_GL && defined(PL_HAVE_OPENGL)
    if (ra_is_gl(ra) && pl_opengl_get(p->gpu)) {
#if HAVE_PL_EXTERNAL_YUV
        struct pl_opengl_wrap_params par = {
            .width = ratex->params.w,
            .height = ratex->params.h,
            .sampler_type = ratex->params.external_yuv
                          ? PL_SAMPLER_EXTERNAL_YUV
                          : PL_SAMPLER_NORMAL,
        };
#else
        struct pl_opengl_wrap_params par = {
            .width = ratex->params.w,
            .height = ratex->params.h,
            .sampler_type = PL_SAMPLER_NORMAL,
        };
#endif

        ra_gl_get_format(ratex->params.format, &par.iformat,
                         &(GLenum){0}, &(GLenum){0});
        ra_gl_get_raw_tex(ra, ratex, &par.texture, &par.target);
        return pl_opengl_wrap(p->gpu, &par);
    }
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    if (ra_is_d3d11(ra)) {
        int array_slice = 0;
        ID3D11Resource *res = ra_d3d11_get_raw_tex(ra, ratex, &array_slice);
        pl_tex tex = pl_d3d11_wrap(p->gpu, pl_d3d11_wrap_params(
            .tex = res,
            .array_slice = array_slice,
            .fmt = ra_d3d11_get_format(ratex->params.format),
            .w = ratex->params.w,
            .h = ratex->params.h,
        ));
        SAFE_RELEASE(res);
        return tex;
    }
#endif

    MP_ERR(p, "Failed mapping hwdec frame? Open a bug!\n");
    return false;
}

// Scale one crop-relative overlay endpoint by the fixed crop's effective
// ratio, snapping zero and full-span endpoints exactly. Interior endpoints
// keep float precision: the renderer maps them proportionally, and
// unconditional rounding could collapse legal thin parts to zero width.
static float hwdec_scale_overlay_endpoint(float q, float old_extent,
                                          float new_extent, double ratio)
{
    if (q == 0.0f)
        return 0.0f;
    if (q == old_extent)
        return new_extent;
    return (float)(q * ratio);
}

// The renderer samples this acquire-local frame copy: bring its crop and its
// video-space overlay parts from logical source space into the verified
// physical lease space. The queue frame keeps classic source-space geometry,
// and the shared subtitle state is never mutated: scaled copies are allocated
// per acquire and freed by the matching release. Frames without a verified
// scaled lease (factor 1, oriented frames) are left unchanged.
static void hwdec_lease_rescale_frame(struct priv *p, struct mp_image *mpi,
                                      struct pl_frame *frame)
{
    struct frame_priv *fp = mpi->priv;
    if (!fp->lease_w || !fp->lease_h || mpi->params.w < 1 || mpi->params.h < 1 ||
        mpi->params.rotate || mpi->params.vflip)
        return;

    double sx = (double)fp->lease_w / mpi->params.w;
    double sy = (double)fp->lease_h / mpi->params.h;
    struct pl_rect2df old = frame->crop;
    frame->crop = (struct pl_rect2df) {
        .x0 = old.x0 == 0 ? 0.0 : (float)lrint(old.x0 * sx),
        .y0 = old.y0 == 0 ? 0.0 : (float)lrint(old.y0 * sy),
        .x1 = old.x1 == mpi->params.w ? fp->lease_w : (float)lrint(old.x1 * sx),
        .y1 = old.y1 == mpi->params.h ? fp->lease_h : (float)lrint(old.y1 * sy),
    };
    // A legal non-empty source crop must not collapse under rounding: the
    // overlay layout divides by its extent, and libplacebo re-defaults a
    // zero crop to the full texture.
    if (old.x1 > old.x0 && frame->crop.x1 <= frame->crop.x0) {
        frame->crop.x1 = MPMIN(frame->crop.x0 + 1, (float)fp->lease_w);
        frame->crop.x0 = frame->crop.x1 - 1;
    }
    if (old.y1 > old.y0 && frame->crop.y1 <= frame->crop.y0) {
        frame->crop.y1 = MPMIN(frame->crop.y0 + 1, (float)fp->lease_h);
        frame->crop.y0 = frame->crop.y1 - 1;
    }

    if (!frame->num_overlays)
        return;
    // The renderer maps SRC_CROP overlays through the *fixed* crop extent,
    // so video-space parts must follow the crop's effective scale rather
    // than the raw lease ratio; rounded or collapse-fixed crops would
    // otherwise shift part proportions relative to the video.
    float old_ex = old.x1 - old.x0, old_ey = old.y1 - old.y0;
    float new_ex = frame->crop.x1 - frame->crop.x0;
    float new_ey = frame->crop.y1 - frame->crop.y0;
    double px = old_ex != 0.0f ? (double)new_ex / (double)old_ex : sx;
    double py = old_ey != 0.0f ? (double)new_ey / (double)old_ey : sy;
    // The talloc wrappers abort on OOM; there is no partial-failure path.
    void *ctx = talloc_new(p);
    struct pl_overlay *ols = talloc_array(ctx, struct pl_overlay,
                                          frame->num_overlays);
    for (int n = 0; n < frame->num_overlays; n++) {
        const struct pl_overlay *ol = &frame->overlays[n];
        // Only video-space overlays move with the sampled crop; DST_CROP
        // parts are already in target space and must be copied verbatim.
        bool video_space = ol->coords == PL_OVERLAY_COORDS_SRC_CROP;
        struct pl_overlay_part *parts =
            talloc_array(ctx, struct pl_overlay_part, ol->num_parts);
        for (int i = 0; i < ol->num_parts; i++) {
            parts[i] = ol->parts[i];
            if (!video_space)
                continue;
            // Video-relative layout scales with the sampled space; the
            // subtitle atlas coordinates in part.src must not change.
            parts[i].dst.x0 = hwdec_scale_overlay_endpoint(parts[i].dst.x0,
                                                           old_ex, new_ex, px);
            parts[i].dst.x1 = hwdec_scale_overlay_endpoint(parts[i].dst.x1,
                                                           old_ex, new_ex, px);
            parts[i].dst.y0 = hwdec_scale_overlay_endpoint(parts[i].dst.y0,
                                                           old_ey, new_ey, py);
            parts[i].dst.y1 = hwdec_scale_overlay_endpoint(parts[i].dst.y1,
                                                           old_ey, new_ey, py);
        }
        ols[n] = *ol;
        ols[n].parts = parts;
    }
    frame->overlays = ols;
    fp->rescale_ctx = ctx;
}

static bool hwdec_acquire(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->vo->priv;
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_suspend(fgt, MKS_FGT_SUSP_ACQ); }

    // Any failure or replacement below must not leave a stale lease marker
    // from an earlier acquire of the same frame.
    fp->lease_w = fp->lease_h = 0;

    if (!hwdec_reconfig(p, fp->hwdec, &mpi->params)) {
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_resume(fgt, MKS_FGT_SUSP_ACQ); }
        return false;
    }

    // Offer the current display size as the fresh-copy target. The mapper
    // gates the diagnostic property, geometry and uniform-scale conditions
    // itself; existing cur/retired leases are published with their own
    // geometry regardless of the request.
    struct ra_hwdec_map_request req = {
        .allow_full_frame_scale = true,
        .target_w = fp->vo->dwidth,
        .target_h = fp->vo->dheight,
    };
    const struct ra_hwdec_map_request *reqp =
        req.target_w > 0 && req.target_h > 0 ? &req : NULL;
    if (ra_hwdec_mapper_map_with_request(p->hwdec_mapper, mpi, reqp) < 0) {
        MP_ERR(p, "Mapping hardware decoded surface failed.\n");
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_resume(fgt, MKS_FGT_SUSP_ACQ); }
        return false;
    }

    for (int n = 0; n < frame->num_planes; n++) {
        if (!(frame->planes[n].texture = hwdec_get_tex(p, n))) {
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_resume(fgt, MKS_FGT_SUSP_ACQ); }
            return false;
        }
    }

    // Verify the published per-map geometry before any consumer relies on
    // it. A scaled lease must be a full logical frame stored over the full
    // physical texture, downsample-only, with no orientation; drivers that
    // never publish geometry keep the classic behavior. Contradictions fail
    // closed.
    const struct ra_hwdec_map_geometry *g = &p->hwdec_mapper->map_geometry;
    if ((p->hwdec_mapper->caps & RA_HWDEC_CAP_MAP_GEOMETRY) && g->valid) {
        pl_tex tex0 = frame->planes[0].texture;
        bool full_logical = g->logical_w == mpi->params.w &&
                            g->logical_h == mpi->params.h &&
                            g->logical_rect.x0 == 0 && g->logical_rect.y0 == 0 &&
                            g->logical_rect.x1 == mpi->params.w &&
                            g->logical_rect.y1 == mpi->params.h;
        bool oriented = mpi->params.rotate || mpi->params.vflip;
        if (!tex0 || !full_logical ||
            tex0->params.w < 1 || tex0->params.h < 1 ||
            g->mapped_rect.x0 != 0 || g->mapped_rect.y0 != 0 ||
            g->mapped_rect.x1 != tex0->params.w ||
            g->mapped_rect.y1 != tex0->params.h ||
            g->mapped_rect.x1 > mpi->params.w || g->mapped_rect.y1 > mpi->params.h ||
            (oriented && (tex0->params.w != mpi->params.w ||
                          tex0->params.h != mpi->params.h)))
        {
            MP_ERR(p, "Unexpected hwdec lease geometry: logical %dx%d rect "
                      "%d,%d,%d,%d mapped rect %d,%d,%d,%d tex %dx%d src %dx%d.\n",
                   g->logical_w, g->logical_h,
                   g->logical_rect.x0, g->logical_rect.y0,
                   g->logical_rect.x1, g->logical_rect.y1,
                   g->mapped_rect.x0, g->mapped_rect.y0,
                   g->mapped_rect.x1, g->mapped_rect.y1,
                   tex0 ? tex0->params.w : 0, tex0 ? tex0->params.h : 0,
                   mpi->params.w, mpi->params.h);
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_resume(fgt, MKS_FGT_SUSP_ACQ); }
            return false;
        }
        if (tex0->params.w != mpi->params.w || tex0->params.h != mpi->params.h) {
            fp->lease_w = tex0->params.w;
            fp->lease_h = tex0->params.h;
        }
    }

    // The renderer samples this local copy: move its crop and video-space
    // overlays into the verified physical space. Runs last so that no
    // failure path below can leave half-rescaled state behind.
    hwdec_lease_rescale_frame(p, mpi, frame);
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_resume(fgt, MKS_FGT_SUSP_ACQ); }
    return true;
}

static void hwdec_release(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->vo->priv;
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_suspend(fgt, MKS_FGT_SUSP_REL); }
    // Free the acquire-local rescaled overlay copies before any early return.
    if (fp->rescale_ctx) {
        talloc_free(fp->rescale_ctx);
        fp->rescale_ctx = NULL;
    }
    if (!p->hwdec_mapper)
        return;
    if (!ra_pl_get(p->hwdec_mapper->ra)) {
        for (int n = 0; n < frame->num_planes; n++)
            pl_tex_destroy(p->gpu, &frame->planes[n].texture);
    }

    ra_hwdec_mapper_unmap(p->hwdec_mapper);
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_resume(fgt, MKS_FGT_SUSP_REL); }
}

static bool format_supported(struct vo *vo, int format, bool use_uint)
{
    struct priv *p = vo->priv;
    struct pl_bit_encoding bits;
    struct pl_plane_data data[4] = {0};
    int planes = plane_data_from_imgfmt(data, &bits, format, use_uint);
    if (!planes)
        return false;

    for (int i = 0; i < planes; i++) {
        if (!pl_plane_find_fmt(p->gpu, NULL, &data[i]))
            return false;
    }

    return true;
}

static bool map_frame(pl_gpu gpu, pl_tex *tex, const struct pl_source_frame *src,
                      struct pl_frame *frame)
{
    struct mp_image *mpi = src->frame_data;
    struct mp_image_params par = mpi->params;
    struct frame_priv *fp = mpi->priv;
    struct vo *vo = fp->vo;
    struct priv *p = vo->priv;

    fp->hwdec = ra_hwdec_get(&p->hwdec_ctx, mpi->imgfmt);
    if (fp->hwdec) {
        // Note: We don't actually need the mapper to map the frame yet, we
        // only reconfig the mapper here (potentially creating it) to access
        // `dst_params`. In practice, though, this should not matter unless the
        // image format changes mid-stream.
        if (!hwdec_reconfig(p, fp->hwdec, &mpi->params)) {
            talloc_free(mpi);
            return false;
        }

        par = p->hwdec_mapper->dst_params;
    }

    mp_image_params_guess_csp(&par);

    *frame = (struct pl_frame) {
        .color = par.color,
        .repr = par.repr,
        .profile = {
            .data = mpi->icc_profile ? mpi->icc_profile->data : NULL,
            .len = mpi->icc_profile ? mpi->icc_profile->size : 0,
        },
        .rotation = par.rotate / 90,
        .user_data = mpi,
    };

    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (opts->hdr_reference_white && !pl_color_transfer_is_hdr(frame->color.transfer))
        frame->color.hdr.max_luma = opts->hdr_reference_white;


    if (opts->treat_srgb_as_power22 & 1 && frame->color.transfer == PL_COLOR_TRC_SRGB) {
        // The sRGB EOTF is a pure gamma 2.2 function. See reference display in
        // IEC 61966-2-1-1999. Linearize sRGB to display light.
        frame->color.transfer = PL_COLOR_TRC_GAMMA22;
    }

    if (fp->hwdec) {

        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(par.imgfmt);
        frame->acquire = hwdec_acquire;
        frame->release = hwdec_release;
        frame->num_planes = desc.num_planes;
        for (int n = 0; n < frame->num_planes; n++) {
            struct pl_plane *plane = &frame->planes[n];
            int *map = plane->component_mapping;
            for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
                if (desc.comps[c].plane != n)
                    continue;

                // Sort by component offset
                uint8_t offset = desc.comps[c].offset;
                int index = plane->components++;
                while (index > 0 && desc.comps[map[index - 1]].offset > offset) {
                    map[index] = map[index - 1];
                    index--;
                }
                map[index] = c;
            }
        }

    } else { // swdec

        struct pl_plane_data data[4] = {0};
        bool use_uint = false;

        // At this point, we know that the format is supported, query_format()
        // makes sure of that. Just check if we should use UINT as a fallback.
        if (!format_supported(vo, mpi->imgfmt, false))
            use_uint = true;

        frame->num_planes = plane_data_from_imgfmt(data, &frame->repr.bits, mpi->imgfmt, use_uint);
        for (int n = 0; n < frame->num_planes; n++) {
            struct pl_plane *plane = &frame->planes[n];
            data[n].width = mp_image_plane_w(mpi, n);
            data[n].height = mp_image_plane_h(mpi, n);
            if (mpi->stride[n] < 0) {
                data[n].pixels = mpi->planes[n] + (data[n].height - 1) * mpi->stride[n];
                data[n].row_stride = -mpi->stride[n];
                plane->flipped = true;
            } else {
                data[n].pixels = mpi->planes[n];
                data[n].row_stride = mpi->stride[n];
            }

            pl_buf buf = get_dr_buf(p, data[n].pixels);
            if (buf) {
                data[n].buf = buf;
                data[n].buf_offset = (uint8_t *) data[n].pixels - buf->data;
                data[n].pixels = NULL;
            } else if (gpu->limits.callbacks) {
                data[n].callback = talloc_free;
                data[n].priv = mp_image_new_ref(mpi);
            }

            if (!pl_upload_plane(gpu, plane, &tex[n], &data[n])) {
                MP_ERR(vo, "Failed uploading frame!\n");
                talloc_free(data[n].priv);
                talloc_free(mpi);
                return false;
            }
        }

    }

    // Update chroma location, must be done after initializing planes
    pl_frame_set_chroma_location(frame, par.chroma_location);

    if (mpi->film_grain)
        pl_film_grain_from_av(&frame->film_grain, (AVFilmGrainParams *) mpi->film_grain->data);

    // Compute a unique signature for any attached ICC profile. Wasteful in
    // theory if the ICC profile is the same for multiple frames, but in
    // practice ICC profiles are overwhelmingly going to be attached to
    // still images so it shouldn't matter.
    pl_icc_profile_compute_signature(&frame->profile);

    // Update LUT attached to this frame
    update_lut(p, &p->next_opts->image_lut);
    frame->lut = p->next_opts->image_lut.lut;
    frame->lut_type = p->next_opts->image_lut.type;
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->vo->priv;
    for (int i = 0; i < MP_ARRAY_SIZE(fp->subs.entries); i++) {
        pl_tex tex = fp->subs.entries[i].tex;
        if (tex)
            MP_TARRAY_APPEND(p, p->sub_tex, p->num_sub_tex, tex);
    }
    talloc_free(mpi);
}

static void discard_frame(const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

static void info_callback(void *priv, const struct pl_render_info *info)
{
    struct vo *vo = priv;
    struct priv *p = vo->priv;
    if (info->index >= VO_PASS_PERF_MAX)
        return; // silently ignore clipped passes, whatever

    struct frame_info *frame;
    switch (info->stage) {
    case PL_RENDER_STAGE_FRAME: frame = &p->perf_fresh; break;
    case PL_RENDER_STAGE_BLEND: frame = &p->perf_redraw; break;
    default: abort();
    }

    frame->count = info->index + 1;
    pl_dispatch_info_move(&frame->info[info->index], info->pass);
}

// These are rolling per-shader execution histories, not timings attributed to
// callback_frame_id. The GL queries complete asynchronously and libplacebo does
// not expose the source frame identity of each returned sample.
static void gpu_timing_report(struct vo *vo, bool final)
{
    struct priv *p = vo->priv;
    if (!p->gpu_timing)
        return;
    int64_t now = mp_time_ns();
    if (!final && now < p->gpu_timing_next_ns)
        return;
    p->gpu_timing_next_ns = now + INT64_C(10000000000);
    struct frame_info *frames[] = { &p->perf_fresh, &p->perf_redraw };
    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < frames[f]->count; i++) {
            const struct pl_dispatch_info *pass = &frames[f]->info[i];
            // Keep every line below LG's ~1KiB log limit. Description is bounded
            // and normalized to avoid multi-line records from shader labels.
            char desc[161] = {0};
            if (pass->shader) {
                snprintf(desc, sizeof(desc), "%s", pass->shader->description);
                for (char *c = desc; *c; c++)
                    if (*c == '\n' || *c == '\r') *c = ' ';
            }
            MP_ERR(vo, "MKS-GPU-PASS instance=%p mono_ns=%"PRId64
                   " final=%d stage=%s index=%d signature=%"PRIu64
                   " samples=%d last_ns=%"PRIu64" avg_ns=%"PRIu64
                   " peak_ns=%"PRIu64" callback_frame_id=%"PRIu64
                   " callback_pts=%.9f attribution=rolling-history desc=%s\n",
                   p, now, final, f ? "blend" : "frame", i, pass->signature,
                   pass->num_samples, pass->last, pass->average, pass->peak,
                   p->gpu_timing_frame_id, p->gpu_timing_pts, desc);
        }
    }
}

static void update_options(struct vo *vo)
{
    struct priv *p = vo->priv;
    pl_options pars = p->pars;
    bool changed = m_config_cache_update(p->opts_cache);
    changed = m_config_cache_update(p->next_opts_cache) || changed;
    if (changed)
        update_render_options(vo);

    update_lut(p, &p->next_opts->lut);
    pars->params.lut = p->next_opts->lut.lut;
    pars->params.lut_type = p->next_opts->lut.type;

    // Update equalizer state
    struct mp_csp_params cparams = MP_CSP_PARAMS_DEFAULTS;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    mp_csp_equalizer_state_get(p->video_eq, &cparams);
    pars->color_adjustment.brightness = cparams.brightness;
    pars->color_adjustment.contrast = cparams.contrast;
    pars->color_adjustment.hue = cparams.hue;
    pars->color_adjustment.saturation = cparams.saturation;
    pars->color_adjustment.gamma = cparams.gamma * opts->gamma;
    p->output_levels = cparams.levels_out;

    for (char **kv = p->next_opts->raw_opts; kv && kv[0]; kv += 2)
        pl_options_set_str(pars, kv[0], kv[1]);
}

static void apply_target_contrast(struct priv *p, struct pl_color_space *color, float min_luma)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;

    // Auto mode, use target value if available
    if (!opts->target_contrast) {
        color->hdr.min_luma = min_luma;
        return;
    }

    // Infinite contrast
    if (opts->target_contrast == -1) {
        color->hdr.min_luma = 1e-7;
        mp_assert(color->hdr.min_luma > 0);
        return;
    }

    // Infer max_luma for current pl_color_space
    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
        .color = color,
        // with HDR10 meta to respect value if already set
        .metadata = PL_HDR_METADATA_HDR10,
        .scaling = PL_HDR_NITS,
        .out_max = &color->hdr.max_luma
    ));

    color->hdr.min_luma = color->hdr.max_luma / opts->target_contrast;
}

static void apply_target_options(struct priv *p, struct pl_frame *target,
                                 float min_luma, bool hint)
{
    update_lut(p, &p->next_opts->target_lut);
    target->lut = p->next_opts->target_lut.lut;
    target->lut_type = p->next_opts->target_lut.type;

    // Colorspace overrides
    const struct gl_video_opts *opts = p->opts_cache->opts;
    // If swapchain returned a value use this, override is used in hint
    if (p->output_levels)
        target->repr.levels = p->output_levels;
    if (opts->target_prim && (!target->color.primaries || !hint))
        target->color.primaries = opts->target_prim;
    if (opts->target_trc && (!target->color.transfer || !hint))
        target->color.transfer = opts->target_trc;
    if (opts->target_peak && (!target->color.hdr.max_luma || !hint))
        target->color.hdr.max_luma = opts->target_peak;
    if (opts->hdr_reference_white && (!target->color.hdr.max_luma || !hint) &&
        !pl_color_transfer_is_hdr(target->color.transfer)) {
        target->color.hdr.max_luma = opts->hdr_reference_white;
    }
    if ((!target->color.hdr.min_luma || !hint))
        apply_target_contrast(p, &target->color, min_luma);
    if (opts->target_gamut) {
        // Ensure resulting gamut still fits inside container
        const struct pl_raw_primaries *gamut, *container;
        gamut = pl_raw_primaries_get(opts->target_gamut);
        container = pl_raw_primaries_get(target->color.primaries);
        target->color.hdr.prim = pl_primaries_clip(gamut, container);
    }
    int dither_depth = opts->dither_depth;
    if (dither_depth == 0) {
        struct ra_swapchain *sw = p->ra_ctx->swapchain;
        dither_depth = sw->fns->color_depth ? sw->fns->color_depth(sw) : 0;
    }
    if (dither_depth > 0) {
        struct pl_bit_encoding *tbits = &target->repr.bits;
        tbits->color_depth += dither_depth - tbits->sample_depth;
        tbits->sample_depth = dither_depth;
    }

    if (opts->icc_opts->icc_use_luma) {
        p->icc_params.max_luma = 0.0f;
    } else {
        pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
            .color    = &target->color,
            .metadata = PL_HDR_METADATA_HDR10, // use only static HDR nits
            .scaling  = PL_HDR_NITS,
            .out_max  = &p->icc_params.max_luma,
        ));
    }

    pl_icc_update(p->pllog, &p->icc_profile, NULL, &p->icc_params);
    target->icc = p->icc_profile;
}

static void apply_crop(struct pl_frame *frame, struct mp_rect crop,
                       int width, int height)
{
    frame->crop = (struct pl_rect2df) {
        .x0 = crop.x0,
        .y0 = crop.y0,
        .x1 = crop.x1,
        .y1 = crop.y1,
    };

    // mpv gives us rotated/flipped rects, libplacebo expects unrotated
    pl_rect2df_rotate(&frame->crop, -frame->rotation);
    if (frame->crop.x1 < frame->crop.x0) {
        frame->crop.x0 = width - frame->crop.x0;
        frame->crop.x1 = width - frame->crop.x1;
    }

    if (frame->crop.y1 < frame->crop.y0) {
        frame->crop.y0 = height - frame->crop.y0;
        frame->crop.y1 = height - frame->crop.y1;
    }
}

static void update_tm_viz(struct pl_color_map_params *params,
                          const struct pl_frame *target)
{
    if (!params->visualize_lut)
        return;

    // Use right half of screen for TM visualization, constrain to 1:1 AR
    const float out_w = fabsf(pl_rect_w(target->crop));
    const float out_h = fabsf(pl_rect_h(target->crop));
    const float size = MPMIN(out_w / 2.0f, out_h);
    params->visualize_rect = (pl_rect2df) {
        .x0 = 1.0f - size / out_w,
        .x1 = 1.0f,
        .y0 = 0.0f,
        .y1 = size / out_h,
    };

    // Visualize red-blue plane
    params->visualize_hue = M_PI / 4.0;
}

static enum pl_color_primaries get_best_prim_container(const struct pl_raw_primaries *gamut)
{
    enum pl_color_primaries container = PL_COLOR_PRIM_UNKNOWN;

    if (!pl_primaries_valid(gamut))
        return container;

    const struct pl_raw_primaries *best = NULL;
    for (enum pl_color_primaries prim = 1; prim < PL_COLOR_PRIM_COUNT; prim++) {
        const struct pl_raw_primaries *raw = pl_raw_primaries_get(prim);
        if (pl_raw_primaries_similar(raw, gamut)) {
            container = prim;
            best = raw;
            break;
        }

        if (pl_primaries_superset(raw, gamut) &&
            (!best || pl_primaries_superset(best, raw)))
        {
            container = prim;
            best = raw;
        }
    }

    if (!best)
        container = PL_COLOR_PRIM_BT_2020;

    return container;
}

static void update_hook_opts_dynamic(struct priv *p, const struct pl_hook *hook,
                                     const struct mp_image *mpi);

#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_YUV_OUTPUT)
static bool fusion_full_rect(pl_rect2df r, int w, int h)
{
    return r.x0 == 0 && r.y0 == 0 && r.x1 == w && r.y1 == h;
}

static bool fusion_full_target_rect(pl_rect2df r, int w, int h)
{
    return ((r.x0 == 0 && r.x1 == w) || (r.x1 == 0 && r.x0 == w)) &&
           ((r.y0 == 0 && r.y1 == h) || (r.y1 == 0 && r.y0 == h));
}

// Bit positions are stable diagnostic identifiers, not alternate eligibility.
// 0 backend disabled, 1 no current, 2 config, 3 profile, 4 compat, 5 HLG,
// 6 source orientation, 7 source format, 8 output alpha, 9 mix count,
// 10 interpolation, 11 drawable flip, 12 planes, 13 target overlay,
// 14 target rotation, 15 PQ, 16 primaries, 17 RGB, 18 full range,
// 19 hooks, 20 distortion, 21 blend, 22 error diffusion, 23 ICC, 24 LUT,
// 25 rounded/blurred background, 26 visualization, 27 FBO precision,
// 28 target crop, 29 destination rect, 30 source rect, 31 no image,
// 32 image overlay/rotation/LUT, 33 image alpha, 34 image crop.
static uint64_t fusion_frame_rejects(struct vo *vo, const struct vo_frame *frame,
                                    const struct pl_frame_mix *mix,
                                    const struct pl_frame *target,
                                    const struct pl_render_params *params,
                                    const struct pl_swapchain_frame *swframe,
                                    const struct ra_yuv_fusion_target *desc)
{
    struct priv *p = vo->priv;
    const struct mp_image *mpi = frame->current;
    pl_fmt fmt = swframe->fbo->params.format;
    uint64_t mask = 0;
#define REJECT(bit, condition) do { if (condition) mask |= UINT64_C(1) << (bit); } while (0)
    REJECT(0, p->fusion_disabled || pl_gpu_is_failed(p->gpu));
    REJECT(1, !mpi);
    if (mpi) {
        REJECT(2, !mpi->params.dv_config_present);
        REJECT(3, mpi->params.dv_profile != 8);
        REJECT(4, mpi->params.dv_bl_signal_compatibility_id != 4);
        REJECT(5, mpi->params.color.transfer != PL_COLOR_TRC_HLG &&
                  mpi->params.transfer_orig != PL_COLOR_TRC_HLG);
        REJECT(6, mpi->params.rotate || mpi->params.vflip);
        REJECT(7, mpi->imgfmt != IMGFMT_MEDIACODEC);
        REJECT(30, p->src.x0 || p->src.y0 || p->src.x1 != mpi->w || p->src.y1 != mpi->h);
    }
    REJECT(8, p->ra_ctx->opts.want_alpha);
    REJECT(9, mix->num_frames != 1);
    REJECT(10, p->is_interpolated);
    REJECT(11, desc->flipped);
    REJECT(12, target->num_planes != 1);
    REJECT(13, target->num_overlays);
    REJECT(14, target->rotation);
    REJECT(15, target->color.transfer != PL_COLOR_TRC_PQ);
    REJECT(16, target->color.primaries != PL_COLOR_PRIM_BT_2020);
    REJECT(17, target->repr.sys != PL_COLOR_SYSTEM_RGB);
    REJECT(18, target->repr.levels != PL_COLOR_LEVELS_FULL);
    REJECT(19, params->num_hooks);
    REJECT(20, params->distort_params);
    REJECT(21, params->blend_params);
    REJECT(22, params->error_diffusion);
    REJECT(23, p->icc_profile);
    REJECT(24, target->lut);
    REJECT(25, !mp_yuv_fusion_background_eligible(params->corner_rounding,
                                                          params->background, params->border));
    REJECT(26, params->color_map_params && params->color_map_params->visualize_lut);
    REJECT(27, !fmt || fmt->type != PL_FMT_FLOAT || fmt->num_components != 4 ||
                   fmt->component_depth[0] != 16 || fmt->component_depth[1] != 16 ||
                   fmt->component_depth[2] != 16 || fmt->component_depth[3] != 16);
    REJECT(28, !fusion_full_target_rect(target->crop, desc->w, desc->h));
    REJECT(29, p->dst.x0 || p->dst.y0 || p->dst.x1 != desc->w || p->dst.y1 != desc->h);
    const struct pl_frame *image = mix->num_frames == 1 ? mix->frames[0] : NULL;
    REJECT(31, !image);
    if (image) {
        REJECT(32, image->num_overlays || image->rotation || image->lut);
        REJECT(33, image->repr.alpha == PL_ALPHA_INDEPENDENT ||
                   image->repr.alpha == PL_ALPHA_PREMULTIPLIED);
        // Logical full-frame eligibility, checked in source space against the
        // queue frame's crop. The queue frame always keeps classic
        // source-space geometry; the physical full-frame property of a scaled
        // lease is enforced fail-closed by the mapper gate and the acquire
        // geometry verification, not here.
        REJECT(34, !mpi || !fusion_full_rect(image->crop, mpi->w, mpi->h));
    }
#undef REJECT
    return mask;
}

#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
static bool compare_current(struct priv *p)
{
    return mks_android_yuv_compare_current(p->ra_ctx, p->compare_id,
                                           p->compare_generation);
}

static bool compare_baseline_ok(struct priv *p)
{
#if defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->atlas_reuse && p->atlas_read)
        return pl_opengl_uv_atlas_read_baseline_ok(p->atlas_read);
#endif
    if (p->compare_atlas)
        return p->atlas_anchor && pl_opengl_uv_atlas_anchor_baseline_ok(p->atlas_anchor);
#endif
    return pl_opengl_yuv_compare_baseline_ok(p->gpu, p->fusion_target);
}

#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
static bool atlas_read_seed_saved(struct priv *, const char *, size_t, const char *);
static void atlas_read_seed_ready(struct vo *);
static void atlas_reuse_poll(struct vo *);
static bool atlas_consumer_render(struct vo *, const struct vo_frame *, const struct pl_frame_mix *,
    const struct pl_frame *, const struct pl_render_params *, const struct pl_swapchain_frame *,
    const struct ra_yuv_fusion_target *);
#endif

static void compare_fail_transaction(struct vo *vo, const char *reason)
{
    struct priv *p = vo->priv;
    if (p->compare_terminal && p->compare_fatal_context)
        return;
    bool owner = mks_android_yuv_compare_owner_current(p->ra_ctx);
    bool restored = !owner && mks_android_yuv_compare_restore_owner(p->ra_ctx);
    struct mp_yuv_compare_abort_plan plan = mp_yuv_compare_abort_plan(
        owner, restored, p->compare_frame_open);
    bool frame_closed = !p->compare_frame_open;
    if (plan.submit_frame) {
        // End libplacebo's started-frame ownership, but never swap a failed
        // diagnostic. submit_frame releases its frame lock on either result.
        (void)pl_swapchain_submit_frame(p->sw);
        p->compare_frame_open = false;
        frame_closed = true;
    } else if (plan.abort_no_gl) {
#if defined(PL_HAVE_MKS_YUV_ABORT_FRAME)
        frame_closed = pl_opengl_swapchain_abort_frame_no_gl(p->sw);
        if (frame_closed)
            p->compare_frame_open = false;
#endif
    }
    p->compare_active = false;
    p->compare_terminal = true;
    p->compare_fatal_context = plan.fatal;
    p->frame_pending = false;
    if ((owner || restored) && p->ra_ctx->fns->yuv_fusion_abort)
        p->ra_ctx->fns->yuv_fusion_abort(p->ra_ctx);
    MP_ERR(vo, "MKS-YUV-COMPARE event=capture-failed id=%"PRIu64" reason=%s terminal=1 owner_current=%d owner_restored=%d frame_closed=%d fatal_context=%d requires_force_stop=%d\n",
           p->compare_id, reason, owner, restored, frame_closed,
           p->compare_fatal_context, p->compare_fatal_context);
}

static bool compare_write_exclusive(const char *path, const void *data, size_t bytes)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const unsigned char *src = data;
    bool ok = true;
    while (bytes) {
        ssize_t n = write(fd, src, bytes);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) { ok = false; break; }
        src += n; bytes -= n;
    }
    ok = ok && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

#if defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
static bool atlas_result_valid(struct priv *p,
    const struct pl_opengl_uv_atlas_anchor_result *r)
{
    if (!r || !p->compare_epoch_valid || !p->compare_frame_id || !p->compare_frame_token ||
        r->capture_id != p->compare_id || r->generation != p->compare_generation ||
        r->width != p->compare_width || r->height != p->compare_height ||
        r->width <= 0 || r->height <= 0 ||
        (uint64_t)r->width * r->height > MP_YUV_ATLAS_MAX_PIXELS ||
        r->pixels != (size_t)r->width * r->height || r->rgba_words != r->pixels * 4 ||
        r->atlas_words != r->pixels * 3 || r->read_format != 0x1908 || r->read_type != 0x1406 ||
        !r->source_abi_ok || !r->fence_signaled || !r->atlas_complete || r->bad_atlas_pixels ||
        r->finish_epoch != p->compare_error_epoch || !r->framebuffer[0] ||
        !r->program_tokens[0] || !r->program_tokens[1] ||
        r->program_tokens[0] != r->program_tokens[2] ||
        r->program_tokens[0] == r->program_tokens[1] ||
        !mp_yuv_compare_target_matches(p->compare_source_pts, p->compare_current_pts,
                                       p->compare_target_pts))
        return false;
    for (int i = 0; i < 3; i++) {
        if (!r->draw_ok[i] || !r->readback_ok[i] || !r->half_exact[i] ||
            r->bad_half_components[i] || r->framebuffer[i] != r->framebuffer[0] ||
            r->draw_epoch_before[i] != p->compare_error_epoch ||
            r->draw_epoch_after[i] != p->compare_error_epoch ||
            r->readback_epoch_before[i] != p->compare_error_epoch ||
            r->readback_epoch_after[i] != p->compare_error_epoch)
            return false;
    }
    return true;
}

static bool atlas_save(struct vo *vo,
    const struct pl_opengl_uv_atlas_anchor_result *r,
    const struct mks_yuv_compare_result *android, bool *invalid)
{
    struct priv *p = vo->priv;
    const uint32_t *before = NULL, *middle = NULL, *after = NULL, *atlas = NULL;
    const char *a0 = NULL, *atlas_fragment = NULL, *vertex = NULL, *original = NULL;
    const uint8_t *baseline_vertices = NULL;
    size_t vertex_bytes = 0, mismatches_ba = 0, mismatches_bb = 0;
    uint32_t mask = 0;
    if (!atlas_result_valid(p, r) || !compare_current(p) ||
        !pl_opengl_uv_atlas_anchor_data(p->atlas_anchor, &before, &middle, &after, &atlas) ||
        !mp_yuv_atlas_data_valid(before, middle, after, atlas, r->pixels,
            r->rgba_words, r->atlas_words, &mismatches_ba, &mismatches_bb, &mask) ||
        r->mismatches_before_atlas != mismatches_ba ||
        r->mismatches_before_after != mismatches_bb || r->anchor_equal != !mask ||
        !compare_current(p) ||
        !pl_opengl_uv_atlas_anchor_sources(p->atlas_anchor, &a0, &atlas_fragment, &vertex, &original) ||
        !mp_yuv_atlas_uninstrumented_source(a0) || !atlas_fragment || !*atlas_fragment ||
        !strcmp(a0, atlas_fragment) || !vertex || !*vertex || !original || !*original ||
        !strstr(a0, original) || strstr(strstr(a0, original) + 1, original))
        return false;
    char quad_metadata[8192] = "";
    {
        struct pl_opengl_yuv_compare_quad_layout q = {0};
        if (!compare_current(p) ||
            !pl_opengl_uv_atlas_anchor_vertices(p->atlas_anchor, &baseline_vertices, &q) ||
            q.primitive != PL_PRIM_TRIANGLE_STRIP || q.indexed || q.vertex_count != 4 ||
            q.num_attributes < 1 || q.num_attributes > 16 ||
            q.position_attrib < 0 || q.position_attrib >= q.num_attributes ||
            q.viewport.x0 || q.viewport.y0 || q.viewport.x1 != p->compare_width ||
            q.viewport.y1 != p->compare_height || q.scissors.x0 || q.scissors.y0 ||
            q.scissors.x1 != p->compare_width || q.scissors.y1 != p->compare_height ||
            !mp_yuv_compare_quad_payloads_valid(baseline_vertices, baseline_vertices,
                q.stride, q.byte_size, false))
            return false;
        size_t used = 0;
#define QUAD_APPEND(...) do { \
    int n = snprintf(quad_metadata + used, sizeof(quad_metadata) - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= sizeof(quad_metadata) - used) return false; \
    used += n; \
} while (0)
        QUAD_APPEND("\"vertex_layout\":{\"primitive\":\"triangle_strip\",\"indexed\":false,"
            "\"vertex_count\":4,\"num_attributes\":%d,\"position_attrib\":%d,"
            "\"stride\":%zu,\"byte_size\":%zu,\"viewport\":[%d,%d,%d,%d],"
            "\"scissors\":[%d,%d,%d,%d],\"attributes\":[",
            q.num_attributes,
            q.position_attrib, q.stride, q.byte_size,
            q.viewport.x0, q.viewport.y0, q.viewport.x1, q.viewport.y1,
            q.scissors.x0, q.scissors.y0, q.scissors.x1, q.scissors.y1);
        for (int i = 0; i < q.num_attributes; i++) {
            const struct pl_opengl_yuv_compare_quad_attrib *at = &q.attributes[i];
            if (!mp_yuv_compare_identifier_valid(at->name, sizeof(at->name)) ||
                !mp_yuv_compare_identifier_valid(at->format_name, sizeof(at->format_name)) ||
                !mp_yuv_compare_identifier_valid(at->glsl_type, sizeof(at->glsl_type)) ||
                at->fmt_type != PL_FMT_FLOAT || at->opaque || at->num_planes || at->location < 0 ||
                at->num_components < 1 || at->num_components > 4 ||
                at->texel_size != (size_t)at->num_components * 4 ||
                at->offset % 4 || q.stride % 4 || at->offset > q.stride ||
                at->texel_size > q.stride - at->offset ||
                (i == q.position_attrib &&
                 (at->num_components != 2 || strcmp(at->glsl_type, "vec2"))))
                return false;
            const char *types[] = {"float", "vec2", "vec3", "vec4"};
            if (strcmp(at->glsl_type, types[at->num_components - 1]))
                return false;
            for (int j = 0; j < 4; j++) {
                if (at->component_depth[j] < 0 || at->component_depth[j] > 32 ||
                    (j < at->num_components && (at->component_depth[j] != 32 ||
                     at->host_bits[j] != 32 || at->sample_order[j] != j)))
                    return false;
            }
            for (int j = 0; j < i; j++) {
                const struct pl_opengl_yuv_compare_quad_attrib *prev = &q.attributes[j];
                if (prev->location == at->location || !strcmp(prev->name, at->name) ||
                    (at->offset < prev->offset + prev->texel_size &&
                     prev->offset < at->offset + at->texel_size))
                    return false;
            }
            for (size_t v = 0; v < 4; v++) {
                for (int c = 0; c < at->num_components; c++) {
                    uint32_t bits;
                    memcpy(&bits, baseline_vertices + v * q.stride + at->offset + c * 4, 4);
                    if ((bits & 0x7f800000u) == 0x7f800000u)
                        return false;
                }
            }
            QUAD_APPEND("%s{\"name\":\"%s\",\"format_name\":\"%s\",\"glsl_type\":\"%s\","
                "\"fmt_type\":\"float32\",\"opaque\":false,\"num_planes\":0,\"location\":%d,\"num_components\":%d,"
                "\"component_depth\":[%d,%d,%d,%d],\"host_bits\":[%d,%d,%d,%d],"
                "\"sample_order\":[%d,%d,%d,%d],\"offset\":%zu,\"texel_size\":%zu}",
                i ? "," : "", at->name, at->format_name, at->glsl_type,
                at->location, at->num_components, at->component_depth[0],
                at->component_depth[1], at->component_depth[2], at->component_depth[3],
                at->host_bits[0], at->host_bits[1], at->host_bits[2], at->host_bits[3],
                at->sample_order[0], at->sample_order[1], at->sample_order[2], at->sample_order[3],
                at->offset, at->texel_size);
        }
        if (!mp_yuv_compare_quad_positions_valid(baseline_vertices, q.stride,
                q.byte_size, q.attributes[q.position_attrib].offset))
            return false;
        QUAD_APPEND("]}");
#undef QUAD_APPEND
        vertex_bytes = q.byte_size;
    }
    uint64_t epoch = 0;
    if (!compare_current(p) || !compare_baseline_ok(p) ||
        !pl_opengl_error_epoch(p->gpu, &epoch) || epoch != p->compare_error_epoch)
        return false;
    *invalid = mask != 0;
    char basename[128], base[256], file[320];
    snprintf(basename, sizeof(basename), "mks-yuv-atlas-%ld-%"PRIu64"%s",
        (long)getpid(), p->compare_id, *invalid ? ".diagnostic-invalid" : "");
    snprintf(base, sizeof(base), "/sdcard/Android/data/com.example.media_kit_hdr_lab/files/%s", basename);
#define ATLAS_SAVE(suffix, data, bytes) do { \
    snprintf(file, sizeof(file), "%s%s", base, suffix); \
    if (!compare_write_exclusive(file, data, bytes)) return false; \
} while (0)
    ATLAS_SAVE(".a0-before.f32", before, r->rgba_words * 4);
    ATLAS_SAVE(".atlas-a.f32", middle, r->rgba_words * 4);
    ATLAS_SAVE(".a0-after.f32", after, r->rgba_words * 4);
    ATLAS_SAVE(".atlas.bin", atlas, r->atlas_words * 4);
    ATLAS_SAVE(".a0.frag.glsl", a0, strlen(a0));
    ATLAS_SAVE(".atlas.frag.glsl", atlas_fragment, strlen(atlas_fragment));
    ATLAS_SAVE(".vertex.glsl", vertex, strlen(vertex));
    ATLAS_SAVE(".original-body.glsl", original, strlen(original));
    ATLAS_SAVE(".baseline-vertices.bin", baseline_vertices, vertex_bytes);
    ATLAS_SAVE(".android.bin", android->words, android->num_words * 4);
    char json[16384];
    size_t used = 0;
#define ATLAS_APPEND(...) do { \
    int n = snprintf(json + used, sizeof(json) - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= sizeof(json) - used) return false; \
    used += n; \
} while (0)
    ATLAS_APPEND("{\"schema\":9,\"status\":\"%s\",\"scope\":\"rgb-anchor-capture-only-not-quality-pass\","
        "\"pid\":%ld,\"capture_id\":%"PRIu64",\"generation\":%"PRIu64","
        "\"frame_id\":%"PRIu64",\"frame_token\":%"PRIu64","
        "\"source_pts\":%.9f,\"current_pts\":%.9f,\"target_pts\":%.9f,"
        "\"width\":%d,\"height\":%d,\"pixels\":%zu,\"rgba_words\":%zu,\"atlas_words\":%zu,"
        "\"byte_order\":\"little\",\"read_format\":%u,\"read_type\":%u,"
        "\"readback_representation\":\"RGBA/FLOAT decoded RGB16F numerical values; not physical storage bytes\","
        "\"instrumentation_scope\":\"A0 before and after have no capture SSBO/tap; atlas A is instrumented; final CSC is instrumented only in Android ROI\","
        "\"draw_order\":[\"a0_before\",\"atlas_a\",\"a0_after\"],"
        "\"draw_ok\":[true,true,true],\"readback_ok\":[true,true,true],\"half_exact\":[true,true,true],"
        "\"program_tokens\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],",
        *invalid ? "diagnostic-invalid" : "complete", (long)getpid(), p->compare_id,
        p->compare_generation, p->compare_frame_id, p->compare_frame_token,
        p->compare_source_pts, p->compare_current_pts, p->compare_target_pts,
        r->width, r->height, r->pixels, r->rgba_words, r->atlas_words,
        r->read_format, r->read_type, r->program_tokens[0], r->program_tokens[1], r->program_tokens[2]);
    const uint64_t *epochs[] = {r->draw_epoch_before, r->draw_epoch_after,
                               r->readback_epoch_before, r->readback_epoch_after};
    const char *epoch_names[] = {"draw_epoch_before", "draw_epoch_after",
                                "readback_epoch_before", "readback_epoch_after"};
    for (int i = 0; i < 4; i++)
        ATLAS_APPEND("\"%s\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],",
            epoch_names[i], epochs[i][0], epochs[i][1], epochs[i][2]);
    const char *reasons = mask == 3 ? "[\"a0-replay-mismatch\",\"atlas-rgb-mismatch\"]" :
        mask == 1 ? "[\"a0-replay-mismatch\"]" : mask == 2 ? "[\"atlas-rgb-mismatch\"]" : "[]";
    ATLAS_APPEND("\"finish_epoch\":\"%"PRIu64"\",\"framebuffer\":[%u,%u,%u],"
        "\"source_abi_ok\":true,\"fence_signaled\":true,\"atlas_complete\":true,"
        "\"anchor_equal\":%s,\"mismatches_before_atlas\":%zu,\"mismatches_before_after\":%zu,"
        "\"bad_half_components\":[0,0,0],\"bad_atlas_pixels\":0,"
        "\"anchor_validation_mask\":%u,\"anchor_validation_reasons\":%s,\"comparison_pass\":\"%s\",\"baseline_ok\":true,"
        "\"android_status\":\"ready\",\"android_swap_succeeded\":true,\"android_fence_signaled\":true,"
        "\"android_words\":%zu,\"android_roi\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},%s,\"files\":{",
        r->finish_epoch, r->framebuffer[0], r->framebuffer[1], r->framebuffer[2],
        *invalid ? "false" : "true", mismatches_ba, mismatches_bb, mask, reasons,
        *invalid ? "refused" : "unknown",
        android->num_words, p->compare_x, p->compare_y, p->compare_w, p->compare_h, quad_metadata);
    const char *keys[] = {"a0_before", "atlas_a", "a0_after", "atlas", "a0_fragment",
        "atlas_fragment", "vertex", "original_body", "baseline_vertices", "android"};
    const char *suffixes[] = {".a0-before.f32", ".atlas-a.f32", ".a0-after.f32", ".atlas.bin",
        ".a0.frag.glsl", ".atlas.frag.glsl", ".vertex.glsl", ".original-body.glsl",
        ".baseline-vertices.bin", ".android.bin"};
    for (int i = 0; i < 10; i++)
        ATLAS_APPEND("%s\"%s\":\"%s%s\"", i ? "," : "", keys[i], basename, suffixes[i]);
    ATLAS_APPEND("}}\n");
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->atlas_reuse && !atlas_read_seed_saved(p, json, used, basename))
        return false;
#endif
    ATLAS_SAVE(".json", json, used);
#undef ATLAS_APPEND
#undef ATLAS_SAVE
    MP_ERR(vo, "MKS-YUV-ATLAS event=%s id=%"PRIu64" generation=%"PRIu64" source_pts=%.9f file=%s.json comparison_pass=%s anchor_validation_mask=%u\n",
        *invalid ? "capture-diagnostic-invalid" : "capture-complete", p->compare_id,
        p->compare_generation, p->compare_source_pts, base, *invalid ? "refused" : "unknown", mask);
    return true;
}

static void atlas_poll(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->compare_active || !p->atlas_anchor)
        return;
    if (!compare_current(p)) {
        compare_fail_transaction(vo, "atlas-context-or-identity-mismatch");
        return;
    }
    struct mks_yuv_compare_result android = {0};
    struct pl_opengl_uv_atlas_anchor_result result = {0};
    int astatus = mks_android_yuv_compare_poll(p->ra_ctx, &android);
    int status = PL_OPENGL_UV_ATLAS_ANCHOR_FAILED;
    if (astatus != MKS_YUV_COMPARE_FAILED && compare_current(p))
        status = pl_opengl_uv_atlas_anchor_poll(p->atlas_anchor, &result);
    bool failed = astatus == MKS_YUV_COMPARE_FAILED ||
        status == PL_OPENGL_UV_ATLAS_ANCHOR_FAILED || mp_time_ns() > p->compare_deadline;
    if (!failed && (astatus != MKS_YUV_COMPARE_READY || status != PL_OPENGL_UV_ATLAS_ANCHOR_COMPLETE))
        return;
    bool identities = android.capture_id == p->compare_id &&
        android.generation == p->compare_generation && android.roi.x == p->compare_x &&
        android.roi.y == p->compare_y && android.roi.w == p->compare_w && android.roi.h == p->compare_h;
    bool invalid = false;
    bool saved = !failed && identities && android.swap_succeeded &&
        android.num_words == (size_t)p->compare_w * p->compare_h * 7 &&
        mp_yuv_compare_android_words_valid(android.words, android.num_words,
            (size_t)p->compare_w * p->compare_h, NULL) && atlas_save(vo, &result, &android, &invalid);
    p->compare_active = false;
    if (!saved || invalid)
        p->compare_terminal = true;
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (saved && !invalid && p->atlas_reuse)
        atlas_read_seed_ready(vo);
#endif
    if (!saved)
        compare_fail_transaction(vo, "atlas-poll-or-save-failed");
    if (!saved)
        MP_ERR(vo, "MKS-YUV-ATLAS event=capture-failed id=%"PRIu64" generation=%"PRIu64" source_pts=%.9f lib_status=%d android_status=%d identities=%d saved=0\n",
            p->compare_id, p->compare_generation, p->compare_source_pts, status, astatus, identities);
}

static bool atlas_render(struct vo *vo, const struct vo_frame *frame,
    const struct pl_frame_mix *mix, const struct pl_frame *target,
    const struct pl_render_params *params, const struct pl_swapchain_frame *swframe,
    const struct ra_yuv_fusion_target *desc)
{
    struct priv *p = vo->priv;
    const struct mp_image *source = mix->frames[0]->user_data;
    p->compare_started = p->compare_active = true;
    p->compare_id++; p->compare_generation = desc->generation;
    p->compare_frame_id = frame->frame_id; p->compare_frame_token = desc->token;
    p->compare_source_pts = source->pts; p->compare_current_pts = frame->current->pts;
    p->compare_width = desc->w; p->compare_height = desc->h;
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->atlas_reuse) {
        p->atlas_phase = MP_ATLAS_SEED_PENDING;
        p->atlas_seed_id = p->compare_id;
        p->atlas_seed_frame_id = frame->frame_id;
        p->atlas_seed_frame_token = desc->token;
        p->atlas_seed_pts = source->pts;
        p->atlas_seed_image = (uint64_t)(uintptr_t)source;
    }
#endif
    p->compare_w = MPMIN(64, desc->w); p->compare_h = MPMIN(64, desc->h);
    p->compare_x = (desc->w - p->compare_w) / 2; p->compare_y = (desc->h - p->compare_h) / 2;
    p->compare_deadline = mp_time_ns() + INT64_C(120000000000);
    bool android = mks_android_yuv_compare_arm(p->ra_ctx, p->compare_id, desc->generation,
        (struct mks_yuv_compare_roi) {p->compare_x, p->compare_y, p->compare_w, p->compare_h});
    if (android && compare_current(p))
        p->atlas_anchor = pl_opengl_uv_atlas_anchor_arm(p->gpu, swframe->fbo,
            &(struct pl_opengl_uv_atlas_anchor_params) {
                .capture_id = p->compare_id, .generation = p->compare_generation});
    MP_ERR(vo, "MKS-YUV-ATLAS event=armed id=%"PRIu64" generation=%"PRIu64" source_pts=%.9f android=%d lib=%d\n",
        p->compare_id, p->compare_generation, source->pts, android, !!p->atlas_anchor);
    if (!p->atlas_anchor) {
        compare_fail_transaction(vo, "atlas-arm-failed");
        return false;
    }
    p->compare_epoch_valid = compare_current(p) &&
        pl_opengl_error_epoch(p->gpu, &p->compare_error_epoch);
    if (!p->compare_epoch_valid) {
        compare_fail_transaction(vo, "atlas-prerender-epoch-failed");
        return false;
    }
    // The ordinary target and renderer planning are unchanged. Only the
    // diagnostic backend transaction adds atlas A between the two original A0s.
    bool rendered = pl_render_image_mix(p->rr, mix, target, params);
    if (!compare_current(p)) {
        compare_fail_transaction(vo, "atlas-postrender-owner-mismatch");
        return false;
    }
    if (rendered)
        pl_gpu_flush(p->gpu);
    uint64_t after = 0;
    p->compare_a_ready = rendered && compare_baseline_ok(p) &&
        pl_opengl_error_epoch(p->gpu, &after) && after == p->compare_error_epoch;
    if (!p->compare_a_ready)
        compare_fail_transaction(vo, "atlas-render-or-receipt-failed");
    return false;
}
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
#include "yuv_atlas_read_impl.h"
#endif
#endif

static bool compare_save(struct vo *vo,
                          const struct pl_opengl_yuv_compare_result *lib,
                          const struct mks_yuv_compare_result *android,
                          uint32_t probe_validation_mask)
{
    struct priv *p = vo->priv;
    bool diagnostic_invalid = probe_validation_mask != 0;
    const char *b = NULL, *a = NULL, *vertex = NULL;
    if (!compare_current(p) || !pl_opengl_yuv_compare_sources(p->gpu, p->fusion_target, &b, &a, &vertex) ||
        !b || !a || !vertex || !android->words ||
        (diagnostic_invalid && !p->compare_uv_probe))
        return false;
    // This is a producer assertion about one frozen dispatch's state and live
    // resource handles. It does not claim a texture-pixel hash or A1 CSC data.
    char repeat_metadata[1536] = "";
#if defined(PL_HAVE_MKS_YUV_COMPARE_REPEAT)
#if defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_SINGLE_PAIR)
    if (p->compare_uv_single) {
        if (!p->compare_uv_probe || !p->compare_uv_oracle ||
            !lib->uv_oracle_single_pair ||
            !mp_yuv_compare_draws_valid(false, lib->repetitions,
                lib->draw_ok, lib->program_tokens) || !p->compare_epoch_valid ||
            !mp_yuv_compare_epochs_valid(1, lib->draw_epoch_before,
                lib->draw_epoch_after, p->compare_error_epoch))
            return false;
        snprintf(repeat_metadata, sizeof(repeat_metadata),
            "\"uv_oracle_single_pair\":true,\"repetitions\":1,"
            "\"draw_order\":[\"A1\",\"B1\"],\"draw_ok\":{\"A1\":true,\"B1\":true},"
            "\"same_input_snapshot\":true,\"programs_reused\":false,"
            "\"program_tokens\":[\"%"PRIu64"\",\"%"PRIu64"\"],"
            "\"draw_epoch_before\":[\"%"PRIu64"\",\"%"PRIu64"\"],"
            "\"draw_epoch_after\":[\"%"PRIu64"\",\"%"PRIu64"\"],"
            "\"assertion_scope\":\"same frozen renderer input state and resource handles; no texture pixel hash\",",
            lib->program_tokens[0], lib->program_tokens[1],
            lib->draw_epoch_before[0], lib->draw_epoch_before[1],
            lib->draw_epoch_after[0], lib->draw_epoch_after[1]);
    } else
#endif
    if (p->compare_repeat) {
        if (!mp_yuv_compare_draws_valid(true, lib->repetitions, lib->draw_ok,
                                         lib->program_tokens) ||
            !p->compare_epoch_valid ||
            !mp_yuv_compare_epochs_valid(2, lib->draw_epoch_before,
                                         lib->draw_epoch_after,
                                         p->compare_error_epoch))
            return false;
        const char *names[4] = {"B1", p->compare_invariant_b ? "A01" : "A1",
                               "B2", p->compare_invariant_b ? "A02" : "A2"};
        if (p->compare_uv_oracle) {
            names[0] = "A1"; names[1] = "B1";
            names[2] = "A2"; names[3] = "B2";
        }
        snprintf(repeat_metadata, sizeof(repeat_metadata),
            "\"repetitions\":2,\"draw_order\":[\"%s\",\"%s\",\"%s\",\"%s\"],"
            "\"draw_ok\":{\"%s\":true,\"%s\":true,\"%s\":true,\"%s\":true},"
            "\"same_input_snapshot\":true,\"programs_reused\":true,"
            "\"program_tokens\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
            "\"draw_epoch_before\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
            "\"draw_epoch_after\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
            "\"assertion_scope\":\"same frozen renderer input state and resource handles; no texture pixel hash\",",
            names[0], names[1], names[2], names[3],
            names[0], names[1], names[2], names[3],
            lib->program_tokens[0], lib->program_tokens[1],
            lib->program_tokens[2], lib->program_tokens[3],
            lib->draw_epoch_before[0], lib->draw_epoch_before[1],
            lib->draw_epoch_before[2], lib->draw_epoch_before[3],
            lib->draw_epoch_after[0], lib->draw_epoch_after[1],
            lib->draw_epoch_after[2], lib->draw_epoch_after[3]);
    }
#endif
    char tap_metadata[768] = "";
    const char *original_body = NULL;
#if defined(PL_HAVE_MKS_YUV_COMPARE_SAMPLE_TAP)
    if (p->compare_sample_tap) {
        const char *tap_body = NULL;
        if (!p->compare_repeat || !lib->sample_tap || lib->words_per_pixel != 26 ||
            lib->sample_sites != 1 || !compare_current(p) ||
            !pl_opengl_yuv_compare_bodies(p->gpu, p->fusion_target,
                                           &original_body, &tap_body) ||
            !original_body || !*original_body || !tap_body || !*tap_body ||
            (!p->compare_uv_oracle && !strstr(b, tap_body)) || !strstr(a, tap_body))
            return false;
        snprintf(tap_metadata, sizeof(tap_metadata),
            "\"sample_tap\":true,\"sample_tap_site_count\":1,\"words_per_pixel\":26,"
            "\"sample_tap_body_hash\":\"%"PRIu64"\","
            "\"instrumentation_scope\":\"actual single textureLod site UV and sampled RGB; instrumentation may change shader compilation; not an unmodified shader\",",
            lib->sample_tap_body_hash);
    }
#endif
    char oracle_metadata[768] = "";
#if defined(PL_HAVE_MKS_YUV_COMPARE_UV_ORACLE)
    if (p->compare_uv_oracle) {
        if (!p->compare_repeat || !p->compare_sample_tap || p->compare_invariant_b ||
            !lib->uv_oracle || lib->uv_oracle_use_a != p->compare_uv_oracle_use_a ||
            !p->compare_epoch_valid ||
            !mp_yuv_compare_barriers_valid_count(p->compare_uv_single ? 1 : 2,
                lib->uv_oracle_barriers,
                lib->uv_oracle_barrier_ok, lib->uv_oracle_barrier_epoch_before,
                lib->uv_oracle_barrier_epoch_after, p->compare_error_epoch))
            return false;
        if (p->compare_uv_single) {
            snprintf(oracle_metadata, sizeof(oracle_metadata),
                "\"uv_oracle\":true,\"uv_oracle_use_a\":%s,\"uv_oracle_barriers\":1,"
                "\"uv_oracle_barrier_ok\":[true],"
                "\"uv_oracle_barrier_epoch_before\":[\"%"PRIu64"\"],"
                "\"uv_oracle_barrier_epoch_after\":[\"%"PRIu64"\"],",
                p->compare_uv_oracle_use_a ? "true" : "false",
                lib->uv_oracle_barrier_epoch_before[0], lib->uv_oracle_barrier_epoch_after[0]);
        } else {
            snprintf(oracle_metadata, sizeof(oracle_metadata),
                "\"uv_oracle\":true,\"uv_oracle_use_a\":%s,\"uv_oracle_barriers\":2,"
                "\"uv_oracle_barrier_ok\":[true,true],"
                "\"uv_oracle_barrier_epoch_before\":[\"%"PRIu64"\",\"%"PRIu64"\"],"
                "\"uv_oracle_barrier_epoch_after\":[\"%"PRIu64"\",\"%"PRIu64"\"],",
                p->compare_uv_oracle_use_a ? "true" : "false",
                lib->uv_oracle_barrier_epoch_before[0], lib->uv_oracle_barrier_epoch_before[1],
                lib->uv_oracle_barrier_epoch_after[0], lib->uv_oracle_barrier_epoch_after[1]);
        }
    }
#endif
    char probe_metadata[768] = "";
#if defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_PROBE)
    if (p->compare_uv_probe) {
        if (!p->compare_uv_oracle || !lib->uv_oracle_probe || !p->compare_probe_words ||
            lib->uv_oracle_probe_words != p->compare_num_probe_words ||
            probe_validation_mask > 127)
            return false;
        const char *names[] = {"gpu-dependency-invalid", "reason-mismatch",
            "dependency-flag-mismatch", "selected-source-mismatch", "base-site-mismatch",
            "observed-a-uv-mismatch", "selected-a-uv-mismatch"};
        char reasons[256] = "[";
        size_t reason_used = 1;
        bool first = true;
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (probe_validation_mask & (1u << i)) {
                int n = snprintf(reasons + reason_used, sizeof(reasons) - reason_used,
                                 "%s\"%s\"", first ? "" : ",", names[i]);
                if (n < 0 || (size_t)n >= sizeof(reasons) - reason_used)
                    return false;
                reason_used += n;
                first = false;
            }
        }
        if (reason_used + 2 > sizeof(reasons))
            return false;
        reasons[reason_used++] = ']';
        reasons[reason_used] = 0;
        snprintf(probe_metadata, sizeof(probe_metadata),
            "\"uv_oracle_probe\":true,\"uv_oracle_probe_words\":%zu,"
            "\"uv_oracle_probe_words_per_pixel\":10,\"uv_oracle_probe_status\":\"complete\","
            "\"oracle_preconditions_valid\":%s,\"oracle_cpu_validation_mask\":%u,"
            "\"oracle_cpu_validation_reasons\":%s,",
            lib->uv_oracle_probe_words, diagnostic_invalid ? "false" : "true",
            probe_validation_mask, reasons);
    }
#endif
    char invariant_metadata[256] = "";
    const char *candidate_vertex = NULL;
#if defined(PL_HAVE_MKS_YUV_COMPARE_INVARIANT_B)
    if (p->compare_invariant_b) {
        const char *baseline_vertex = NULL;
        if (!p->compare_repeat || !p->compare_sample_tap || !lib->invariant_b ||
            !compare_current(p) ||
            !pl_opengl_yuv_compare_vertices(p->gpu, p->fusion_target,
                &candidate_vertex, &baseline_vertex) ||
            !baseline_vertex || strcmp(vertex, baseline_vertex) ||
            !mp_yuv_compare_invariant_vertex_valid(candidate_vertex, baseline_vertex,
                lib->invariant_uv_identifier, sizeof(lib->invariant_uv_identifier)))
            return false;
        snprintf(invariant_metadata, sizeof(invariant_metadata),
            "\"invariant_b\":true,\"invariant_uv_identifier\":\"%s\",",
            lib->invariant_uv_identifier);
    }
#endif
    char quad_metadata[8192] = "";
    const uint8_t *baseline_vertices = NULL, *candidate_vertices = NULL;
    size_t vertex_bytes = 0;
#if defined(PL_HAVE_MKS_YUV_COMPARE_QUAD_ORDER)
    if (lib->quad_order != p->compare_quad_order ||
        lib->quad_order_reverse != p->compare_quad_reverse)
        return false;
    if (p->compare_quad_order) {
        struct pl_opengl_yuv_compare_quad_layout q = {0};
        if (!p->compare_uv_single || p->compare_uv_oracle_use_a ||
            !compare_current(p) ||
            !pl_opengl_yuv_compare_quad_payloads(p->gpu, p->fusion_target,
                &baseline_vertices, &candidate_vertices, &q) ||
            q.primitive != PL_PRIM_TRIANGLE_STRIP || q.indexed || q.vertex_count != 4 ||
            q.num_attributes < 1 || q.num_attributes > 16 ||
            q.position_attrib < 0 || q.position_attrib >= q.num_attributes ||
            q.viewport.x0 || q.viewport.y0 || q.viewport.x1 != p->compare_width ||
            q.viewport.y1 != p->compare_height || q.scissors.x0 || q.scissors.y0 ||
            q.scissors.x1 != p->compare_width || q.scissors.y1 != p->compare_height ||
            !mp_yuv_compare_quad_payloads_valid(baseline_vertices, candidate_vertices,
                q.stride, q.byte_size, p->compare_quad_reverse))
            return false;
        size_t used = 0;
#define QUAD_APPEND(...) do { \
    int n = snprintf(quad_metadata + used, sizeof(quad_metadata) - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= sizeof(quad_metadata) - used) return false; \
    used += n; \
} while (0)
        QUAD_APPEND("\"quad_order\":true,\"quad_order_reverse\":%s,"
            "\"quad_vertex_layout\":{\"primitive\":\"triangle_strip\",\"indexed\":false,"
            "\"vertex_count\":4,\"num_attributes\":%d,\"position_attrib\":%d,"
            "\"stride\":%zu,\"byte_size\":%zu,\"viewport\":[%d,%d,%d,%d],"
            "\"scissors\":[%d,%d,%d,%d],\"attributes\":[",
            p->compare_quad_reverse ? "true" : "false", q.num_attributes,
            q.position_attrib, q.stride, q.byte_size,
            q.viewport.x0, q.viewport.y0, q.viewport.x1, q.viewport.y1,
            q.scissors.x0, q.scissors.y0, q.scissors.x1, q.scissors.y1);
        for (int i = 0; i < q.num_attributes; i++) {
            const struct pl_opengl_yuv_compare_quad_attrib *at = &q.attributes[i];
            if (!mp_yuv_compare_identifier_valid(at->name, sizeof(at->name)) ||
                !mp_yuv_compare_identifier_valid(at->format_name, sizeof(at->format_name)) ||
                !mp_yuv_compare_identifier_valid(at->glsl_type, sizeof(at->glsl_type)) ||
                at->fmt_type != PL_FMT_FLOAT || at->opaque || at->num_planes || at->location < 0 ||
                at->num_components < 1 || at->num_components > 4 ||
                at->texel_size != (size_t)at->num_components * 4 ||
                at->offset % 4 || q.stride % 4 || at->offset > q.stride ||
                at->texel_size > q.stride - at->offset ||
                (i == q.position_attrib &&
                 (at->num_components != 2 || strcmp(at->glsl_type, "vec2"))))
                return false;
            const char *types[] = {"float", "vec2", "vec3", "vec4"};
            if (strcmp(at->glsl_type, types[at->num_components - 1]))
                return false;
            for (int j = 0; j < 4; j++) {
                if (at->component_depth[j] < 0 || at->component_depth[j] > 32 ||
                    (j < at->num_components && (at->component_depth[j] != 32 ||
                     at->host_bits[j] != 32 || at->sample_order[j] != j)))
                    return false;
            }
            for (int j = 0; j < i; j++) {
                const struct pl_opengl_yuv_compare_quad_attrib *prev = &q.attributes[j];
                if (prev->location == at->location || !strcmp(prev->name, at->name) ||
                    (at->offset < prev->offset + prev->texel_size &&
                     prev->offset < at->offset + at->texel_size))
                    return false;
            }
            for (size_t v = 0; v < 4; v++) {
                for (int c = 0; c < at->num_components; c++) {
                    uint32_t bits;
                    memcpy(&bits, baseline_vertices + v * q.stride + at->offset + c * 4, 4);
                    if ((bits & 0x7f800000u) == 0x7f800000u)
                        return false;
                }
            }
            QUAD_APPEND("%s{\"name\":\"%s\",\"format_name\":\"%s\",\"glsl_type\":\"%s\","
                "\"fmt_type\":\"float32\",\"opaque\":false,\"num_planes\":0,\"location\":%d,\"num_components\":%d,"
                "\"component_depth\":[%d,%d,%d,%d],\"host_bits\":[%d,%d,%d,%d],"
                "\"sample_order\":[%d,%d,%d,%d],\"offset\":%zu,\"texel_size\":%zu}",
                i ? "," : "", at->name, at->format_name, at->glsl_type,
                at->location, at->num_components, at->component_depth[0],
                at->component_depth[1], at->component_depth[2], at->component_depth[3],
                at->host_bits[0], at->host_bits[1], at->host_bits[2], at->host_bits[3],
                at->sample_order[0], at->sample_order[1], at->sample_order[2], at->sample_order[3],
                at->offset, at->texel_size);
        }
        if (!mp_yuv_compare_quad_positions_valid(baseline_vertices, q.stride,
                q.byte_size, q.attributes[q.position_attrib].offset))
            return false;
        QUAD_APPEND("]},");
#undef QUAD_APPEND
        vertex_bytes = q.byte_size;
    }
#endif
    char basename[128], base[256], file[320];
    snprintf(basename, sizeof(basename), "mks-yuv-compare-%ld-%"PRIu64"%s",
             (long)getpid(), p->compare_id, diagnostic_invalid ? ".diagnostic-invalid" : "");
    snprintf(base, sizeof(base), "/sdcard/Android/data/com.example.media_kit_hdr_lab/files/%s", basename);
    char extra_file_metadata[1024] = "";
    if (p->compare_sample_tap) {
        if (!original_body)
            return false;
        snprintf(extra_file_metadata, sizeof(extra_file_metadata),
            ",\"original_body\":\"%s.original-body.glsl\"", basename);
    }
    if (p->compare_invariant_b) {
        if (!candidate_vertex)
            return false;
        size_t used = strlen(extra_file_metadata);
        snprintf(extra_file_metadata + used, sizeof(extra_file_metadata) - used,
            ",\"candidate_vertex\":\"%s.candidate-vertex.glsl\"", basename);
    }
    if (p->compare_uv_probe) {
        size_t used = strlen(extra_file_metadata);
        snprintf(extra_file_metadata + used, sizeof(extra_file_metadata) - used,
            ",\"oracle_probe\":\"%s.oracle-probe.bin\"", basename);
    }
    if (p->compare_quad_order) {
        size_t used = strlen(extra_file_metadata);
        snprintf(extra_file_metadata + used, sizeof(extra_file_metadata) - used,
            ",\"baseline_vertices\":\"%s.baseline-vertices.bin\",\"candidate_vertices\":\"%s.candidate-vertices.bin\"",
            basename, basename);
    }
#define SAVE(suffix, data, bytes) do { \
    snprintf(file, sizeof(file), "%s%s", base, suffix); \
    if (!compare_write_exclusive(file, data, bytes)) return false; \
} while (0)
    SAVE(".lib.bin", p->compare_words, lib->words * sizeof(uint32_t));
    SAVE(".android.bin", android->words, android->num_words * sizeof(uint32_t));
    SAVE(".candidate.glsl", b, strlen(b));
    SAVE(".baseline.glsl", a, strlen(a));
    SAVE(".vertex.glsl", vertex, strlen(vertex));
    if (p->compare_sample_tap)
        SAVE(".original-body.glsl", original_body, strlen(original_body));
    if (p->compare_invariant_b)
        SAVE(".candidate-vertex.glsl", candidate_vertex, strlen(candidate_vertex));
    if (p->compare_uv_probe)
        SAVE(".oracle-probe.bin", p->compare_probe_words,
             p->compare_num_probe_words * sizeof(uint32_t));
    if (p->compare_quad_order) {
        SAVE(".baseline-vertices.bin", baseline_vertices, vertex_bytes);
        SAVE(".candidate-vertices.bin", candidate_vertices, vertex_bytes);
    }
    char manifest[16384];
    int length = snprintf(manifest, sizeof(manifest),
        "{\"schema\":%d,\"status\":\"%s\",\"scope\":\"capture-only-not-comparison-pass\",%s%s%s%s%s%s"
        "\"pid\":%ld,\"capture_id\":%"PRIu64",\"generation\":%"PRIu64","
        "\"frame_id\":%"PRIu64",\"frame_token\":%"PRIu64","
        "\"source_pts\":%.9f,\"current_pts\":%.9f,\"target_pts\":%.9f,\"shader_body_hash\":\"%"PRIu64"\","
        "\"target_width\":%d,\"target_height\":%d,"
        "\"roi\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"lib_candidate_ok\":true,\"lib_baseline_ok\":true,\"lib_status\":\"complete\","
        "\"android_status\":\"ready\",\"android_swap_succeeded\":true,"
        "\"lib_fence_signaled\":true,\"android_fence_signaled\":true,"
        "\"lib_words\":%zu,\"android_words\":%zu,\"byte_order\":\"little\","
        "\"files\":{\"lib\":\"%s.lib.bin\",\"android\":\"%s.android.bin\","
        "\"candidate\":\"%s.candidate.glsl\",\"baseline\":\"%s.baseline.glsl\",\"vertex\":\"%s.vertex.glsl\"%s}}\n",
        p->compare_quad_order ? 8 : p->compare_uv_single ? 7 : p->compare_uv_probe ? 6 : p->compare_uv_oracle ? 5 : p->compare_invariant_b ? 4 :
            p->compare_sample_tap ? 3 : p->compare_repeat ? 2 : 1,
        diagnostic_invalid ? "diagnostic-invalid" : "complete",
        repeat_metadata, tap_metadata, invariant_metadata, oracle_metadata, probe_metadata, quad_metadata,
        (long)getpid(), p->compare_id, p->compare_generation,
        p->compare_frame_id, p->compare_frame_token,
        p->compare_source_pts, p->compare_current_pts, p->compare_target_pts, lib->shader_body_hash,
        p->compare_width, p->compare_height,
        p->compare_x, p->compare_y, p->compare_w, p->compare_h,
        lib->words, android->num_words, basename, basename, basename, basename, basename,
        extra_file_metadata);
    if (length < 0 || (size_t)length >= sizeof(manifest))
        return false;
    SAVE(".json", manifest, (size_t)length);
#undef SAVE
    MP_ERR(vo, "MKS-YUV-COMPARE event=%s id=%"PRIu64" generation=%"PRIu64" source_pts=%.9f file=%s.json comparison_pass=%s repetitions=%d sample_tap=%d invariant_b=%d uv_oracle=%d uv_oracle_use_a=%d uv_oracle_probe=%d uv_oracle_single_pair=%d quad_order=%d quad_order_reverse=%d\n",
           diagnostic_invalid ? "capture-diagnostic-invalid" : "capture-complete",
           p->compare_id, p->compare_generation, p->compare_source_pts, base,
           diagnostic_invalid ? "refused" : "unknown",
           p->compare_repeat && !p->compare_uv_single ? 2 : 1,
           p->compare_sample_tap, p->compare_invariant_b,
           p->compare_uv_oracle, p->compare_uv_oracle_use_a, p->compare_uv_probe,
           p->compare_uv_single, p->compare_quad_order, p->compare_quad_reverse);
    return true;
}

static void compare_poll(struct vo *vo)
{
    struct priv *p = vo->priv;
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->atlas_reuse) {
        atlas_reuse_poll(vo);
        return;
    }
#endif
#if defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
    if (p->compare_atlas) {
        atlas_poll(vo);
        return;
    }
#endif
    if (!p->compare_active || !p->fusion_target)
        return;
    if (!compare_current(p)) {
        compare_fail_transaction(vo, "context-or-capture-identity-mismatch");
        return;
    }
    struct pl_opengl_yuv_compare_result lib = {0};
    struct mks_yuv_compare_result android = {0};
    int astatus = mks_android_yuv_compare_poll(p->ra_ctx, &android);
    int lstatus = PL_OPENGL_YUV_COMPARE_FAILED;
    if (astatus != MKS_YUV_COMPARE_FAILED && compare_current(p))
        lstatus = pl_opengl_yuv_compare_poll(p->gpu, p->fusion_target,
            p->compare_words, p->compare_num_words, &lib);
    int pstatus = PL_OPENGL_YUV_COMPARE_COMPLETE;
#if defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_PROBE)
    if (p->compare_uv_probe) {
        pstatus = PL_OPENGL_YUV_COMPARE_PENDING;
        if (lstatus == PL_OPENGL_YUV_COMPARE_COMPLETE && compare_current(p))
            pstatus = pl_opengl_yuv_compare_probe_poll(p->gpu, p->fusion_target,
                p->compare_probe_words, p->compare_num_probe_words);
    }
#endif
    bool failed = lstatus == PL_OPENGL_YUV_COMPARE_FAILED ||
        pstatus == PL_OPENGL_YUV_COMPARE_FAILED ||
        astatus == MKS_YUV_COMPARE_FAILED || mp_time_ns() > p->compare_deadline;
    if (!failed && (lstatus != PL_OPENGL_YUV_COMPARE_COMPLETE ||
                    pstatus != PL_OPENGL_YUV_COMPARE_COMPLETE ||
                    astatus != MKS_YUV_COMPARE_READY))
        return;
    bool identities = lib.capture_id == p->compare_id &&
        android.capture_id == p->compare_id &&
        lib.generation == p->compare_generation &&
        android.generation == p->compare_generation &&
        android.roi.x == p->compare_x && android.roi.y == p->compare_y &&
        android.roi.w == p->compare_w && android.roi.h == p->compare_h &&
        lib.words == p->compare_num_words &&
        android.num_words == (size_t)p->compare_w * p->compare_h * 7;
    bool data_valid = identities && p->compare_frame_id && p->compare_frame_token &&
        mp_yuv_compare_target_matches(p->compare_source_pts, p->compare_current_pts,
                                      p->compare_target_pts) &&
        lstatus == PL_OPENGL_YUV_COMPARE_COMPLETE &&
        astatus == MKS_YUV_COMPARE_READY;
#if defined(PL_HAVE_MKS_YUV_COMPARE_REPEAT)
    data_valid = data_valid && mp_yuv_compare_draws_valid(p->compare_repeat && !p->compare_uv_single,
        lib.repetitions, lib.draw_ok, lib.program_tokens);
    if (p->compare_repeat)
        data_valid = data_valid && p->compare_epoch_valid &&
            mp_yuv_compare_epochs_valid(p->compare_uv_single ? 1 : 2, lib.draw_epoch_before,
                lib.draw_epoch_after, p->compare_error_epoch);
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_SAMPLE_TAP)
    data_valid = data_valid && lib.sample_tap == p->compare_sample_tap &&
        lib.words_per_pixel == (p->compare_sample_tap ? 26 : 14) &&
        lib.sample_sites == (p->compare_sample_tap ? 1 : 0);
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_INVARIANT_B)
    data_valid = data_valid && lib.invariant_b == p->compare_invariant_b;
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_UV_ORACLE)
    data_valid = data_valid && lib.uv_oracle == p->compare_uv_oracle &&
        lib.uv_oracle_use_a == p->compare_uv_oracle_use_a;
    if (p->compare_uv_oracle)
        data_valid = data_valid && p->compare_epoch_valid &&
            mp_yuv_compare_barriers_valid_count(p->compare_uv_single ? 1 : 2,
                lib.uv_oracle_barriers,
                lib.uv_oracle_barrier_ok, lib.uv_oracle_barrier_epoch_before,
                lib.uv_oracle_barrier_epoch_after, p->compare_error_epoch);
#endif
    size_t bad_word = SIZE_MAX, bad_probe_word = SIZE_MAX, bad_android_word = SIZE_MAX;
    uint32_t probe_validation_mask = 0;
#if defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_PROBE)
    data_valid = data_valid && lib.uv_oracle_probe == p->compare_uv_probe;
    if (p->compare_uv_probe)
        data_valid = data_valid && lib.uv_oracle_probe_words == p->compare_num_probe_words &&
            mp_yuv_compare_android_words_valid(android.words, android.num_words,
                (size_t)p->compare_w * p->compare_h, &bad_android_word) &&
            mp_yuv_compare_probe_words_valid_count(p->compare_uv_single ? 1 : 2,
                p->compare_words, lib.words,
                p->compare_probe_words, lib.uv_oracle_probe_words,
                (size_t)p->compare_w * p->compare_h, p->compare_uv_oracle_use_a,
                &probe_validation_mask, &bad_word, &bad_probe_word);
    else
#endif
    {
        // Original schemas retain their strict dependency/UV rejection. Only
        // schema 6 can retain fully covered evidence of a failed dependency.
        data_valid = data_valid && mp_yuv_compare_words_valid_layout(p->compare_words,
            lib.words, (size_t)p->compare_w * p->compare_h,
            p->compare_repeat ? 2 : 1, p->compare_sample_tap ? 26 : 14, &bad_word);
        if (p->compare_uv_oracle && p->compare_uv_oracle_use_a)
            data_valid = data_valid && mp_yuv_compare_oracle_uv_valid(p->compare_words,
                lib.words, (size_t)p->compare_w * p->compare_h, &bad_word);
    }
#if defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_SINGLE_PAIR)
    data_valid = data_valid && lib.uv_oracle_single_pair == p->compare_uv_single;
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_QUAD_ORDER)
    data_valid = data_valid && lib.quad_order == p->compare_quad_order &&
        lib.quad_order_reverse == p->compare_quad_reverse;
#endif
    bool diagnostic_invalid = p->compare_uv_probe && probe_validation_mask != 0;
    bool saved = !failed && identities && data_valid && lib.candidate_ok && lib.baseline_ok &&
        android.swap_succeeded && compare_save(vo, &lib, &android, probe_validation_mask);
    p->compare_active = false;
    if (!saved || diagnostic_invalid)
        p->compare_terminal = true;
    if (!saved)
        MP_ERR(vo, "MKS-YUV-COMPARE event=capture-failed id=%"PRIu64" generation=%"PRIu64" source_pts=%.9f lib_status=%d android_status=%d probe_status=%d identities=%d data_valid=%d bad_word=%zu bad_probe_word=%zu bad_android_word=%zu saved=0\n",
               p->compare_id, p->compare_generation, p->compare_source_pts,
               lstatus, astatus, pstatus, identities, data_valid, bad_word, bad_probe_word,
               bad_android_word);
    // Keep objects and Android's swap guard alive until teardown. Subsequent
    // draws are intentionally frozen for this one-frame diagnostic session.
}
#endif

#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_READ)
static void resize(struct vo *vo);

// Default-off production UV table state machine for the fusion candidate.
// The seeding frame renders the ordinary path; the zero-readback anchor rides
// the very same final pass and records the UV table validated by schema10.
// Subsequent gated fusion frames sample through the attached table; any
// failure or identity detach returns to the ordinary path immediately.
// A fused frame is only presented when the table-drive receipt advanced, so
// a renderer-internal retry can never silently present untabled fusion.
// Tear the table down. Returns false when the anchor or cache could not be
// released (e.g. a foreign EGL owner); the stale pointer is kept so seeding
// stays blocked instead of silently forgetting uncleaned resources.
static bool fusion_table_detach(struct vo *vo)
{
    struct priv *p = vo->priv;
    bool clean = true;
    if (p->table_cache) {
        pl_opengl_uv_atlas_table_detach(p->gpu);
        clean &= pl_opengl_uv_atlas_cache_destroy(&p->table_cache);
    }
    if (p->table_anchor)
        clean &= pl_opengl_uv_atlas_anchor_cancel(&p->table_anchor);
    p->fusion_table_state = FT_OFF;
    p->table_seed_frames = 0;
    if (!clean)
        MP_ERR(vo, "MKS-FUSION-TABLE event=cleanup-deferred anchor=%d cache=%d\n",
               p->table_anchor ? 1 : 0, p->table_cache ? 1 : 0);
    return clean;
}

// Every seeding failure funnels through here: two consecutive failures
// disable the table for the session, and in production mode (the table was
// enabled at init) the now-tableless fusion is blocked as well so playback
// returns to the split ordinary path rather than the un-tabled fusion with
// its known UV rounding regression.
static void fusion_table_note_failure(struct vo *vo, const char *reason)
{
    struct priv *p = vo->priv;
    p->table_failures++;
    if (p->table_failures >= 2 && p->fusion_table_enabled) {
        p->fusion_table_enabled = false;
        MP_ERR(vo, "MKS-FUSION-TABLE event=table-disabled reason=%s failures=%d\n",
               reason, p->table_failures);
    }
}

static void fusion_table_poll(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->fusion_table_enabled || p->fusion_table_state != FT_SEEDING ||
        !p->table_anchor)
        return;
    if (++p->table_seed_frames > 10) {
        MP_ERR(vo, "MKS-FUSION-TABLE event=seed-timeout\n");
        fusion_table_note_failure(vo, "seed-timeout");
        fusion_table_detach(vo);
        return;
    }
    struct pl_opengl_uv_atlas_anchor_result result = {0};
    int status = pl_opengl_uv_atlas_anchor_poll(p->table_anchor, &result);
    if (status == PL_OPENGL_UV_ATLAS_ANCHOR_PENDING)
        return;
    if (status != PL_OPENGL_UV_ATLAS_ANCHOR_COMPLETE) {
        // A failed poll drops this anchor and reseeds with a fresh epoch;
        // the shared failure budget bounds the retries (transient stale
        // errors recover on the next attempt, persistent failures disable
        // the table for the session).
        MP_ERR(vo, "MKS-FUSION-TABLE event=seed-retry status=%d failures=%d\n",
               status, p->table_failures + 1);
        fusion_table_note_failure(vo, "poll-failed");
        fusion_table_detach(vo);
        return;
    }
    pl_opengl_uv_atlas_cache cache = pl_opengl_uv_atlas_promote(&p->table_anchor);
    if (!cache || !pl_opengl_uv_atlas_table_attach(p->gpu, cache)) {
        MP_ERR(vo, "MKS-FUSION-TABLE event=promote-or-attach-failed\n");
        if (cache) {
            // An undestroyable cache must not leak: keep it owned for the
            // deferred-cleanup retry (it also blocks reseeding).
            if (!pl_opengl_uv_atlas_cache_destroy(&cache)) {
                if (p->table_cache)
                    MP_ERR(vo, "MKS-FUSION-TABLE event=cache-leak-on-double-defer\n");
                else
                    p->table_cache = cache;
            }
        }
        fusion_table_note_failure(vo, "promote-or-attach");
        fusion_table_detach(vo);
        return;
    }
    p->table_cache = cache;
    p->fusion_table_state = FT_READY;
    // A successful seed clears the consecutive-failure budget.
    p->table_failures = 0;
    MP_ERR(vo, "MKS-FUSION-TABLE event=ready cache_id=%"PRIu64" failures=0\n",
           result.capture_id);
}

static bool fusion_table_seed(struct vo *vo, const struct vo_frame *frame,
                              const struct pl_swapchain_frame *swframe,
                              const struct ra_yuv_fusion_target *desc)
{
    struct priv *p = vo->priv;
    // A stale cache from a deferred cleanup must block seeding too: promoting
    // a new cache would orphan the unreleased one.
    if (p->table_anchor || p->table_cache || p->fusion_table_state != FT_OFF ||
        !frame || !frame->current)
        return false;
    {
        // Diagnostic-only persistent seed-failure injection: proves the
        // shared failure budget disables the table under continuous failure
        // instead of retrying forever. Default off.
        char inject[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.mks.yuv_table_fail_seed", inject);
        if (!strcmp(inject, "1")) {
            MP_ERR(vo, "MKS-FUSION-TABLE event=arm-failed failures=%d (injected)\n",
                   p->table_failures + 1);
            fusion_table_note_failure(vo, "arm-failed-injected");
            return false;
        }
    }
    p->table_anchor = pl_opengl_uv_atlas_anchor_arm(p->gpu, swframe->fbo,
        &(struct pl_opengl_uv_atlas_anchor_params) {
            .capture_id = ++p->table_serial,
            .generation = desc->generation,
            .production_seed = true,
        });
    if (!p->table_anchor) {
        MP_ERR(vo, "MKS-FUSION-TABLE event=arm-failed failures=%d\n",
                p->table_failures + 1);
        fusion_table_note_failure(vo, "arm-failed");
        return false;
    }
    const struct mp_image *mpi_seed = frame->current;
    p->table_src_w = mpi_seed->w;
    p->table_src_h = mpi_seed->h;
    const struct gl_video_opts *topts = p->opts_cache->opts;
    p->table_scaler_kernel = topts->scaler[SCALER_SCALE].kernel.function;
    p->table_dscale_kernel = topts->scaler[SCALER_DSCALE].kernel.function == SCALER_INHERIT
        ? topts->scaler[SCALER_SCALE].kernel.function
        : topts->scaler[SCALER_DSCALE].kernel.function;
    p->table_linear_up = topts->linear_upscaling;
    p->table_linear_down = topts->linear_downscaling;
    p->table_correct_down = topts->correct_downscaling;
    p->fusion_table_state = FT_SEEDING;
    return true;
}
#endif

static bool fusion_render(struct vo *vo, const struct vo_frame *frame,
                           const struct pl_frame_mix *mix,
                           const struct pl_frame *target,
                           const struct pl_render_params *params,
                           const struct pl_swapchain_frame *swframe,
                           const struct ra_yuv_fusion_target *desc)
{
    struct priv *p = vo->priv;
    uint64_t rejects = fusion_frame_rejects(vo, frame, mix, target, params, swframe, desc);
    if (rejects) {
        if (++p->fusion_gate_rejects == 1 || p->fusion_gate_rejects % 300 == 0) {
            MP_ERR(vo, "MKS-FUSION gate-rejected=%"PRIu64" mask=0x%"PRIx64" compat_present=%d compat=%d flipped=%d mix=%d overlays=%d hooks=%d format=%s\n",
                   p->fusion_gate_rejects, rejects, frame->current && frame->current->params.dv_config_present,
                   frame->current ? frame->current->params.dv_bl_signal_compatibility_id : -1,
                   swframe->flipped, mix->num_frames, target->num_overlays,
                   params->num_hooks, swframe->fbo->params.format ?
                   swframe->fbo->params.format->name : "unknown");
            MP_ERR(vo, "MKS-FUSION target crop=%g,%g,%g,%g repr=%d/%d trc=%d prim=%d dst=%d,%d,%d,%d size=%dx%d\n",
                   target->crop.x0, target->crop.y0, target->crop.x1, target->crop.y1,
                   target->repr.sys, target->repr.levels, target->color.transfer,
                   target->color.primaries, p->dst.x0, p->dst.y0, p->dst.x1, p->dst.y1,
                   desc->w, desc->h);
            const struct mp_image *mpi = frame->current;
            if (mpi)
                MP_ERR(vo, "MKS-FUSION source size=%dx%d params=%dx%d fmt=%d profile=%u trc=%d orig_trc=%d rotate=%d vflip=%d src=%d,%d,%d,%d want_alpha=%d\n",
                       mpi->w, mpi->h, mpi->params.w, mpi->params.h, mpi->imgfmt,
                       mpi->params.dv_profile, mpi->params.color.transfer,
                       mpi->params.transfer_orig, mpi->params.rotate, mpi->params.vflip,
                       p->src.x0, p->src.y0, p->src.x1, p->src.y1, p->ra_ctx->opts.want_alpha);
            pl_fmt fmt = swframe->fbo->params.format;
            if (fmt)
                MP_ERR(vo, "MKS-FUSION format type=%d components=%d depths=%d,%d,%d,%d fbo=%dx%d\n",
                       fmt->type, fmt->num_components, fmt->component_depth[0],
                       fmt->component_depth[1], fmt->component_depth[2], fmt->component_depth[3],
                       swframe->fbo->params.w, swframe->fbo->params.h);
            MP_ERR(vo, "MKS-FUSION params distort=%d blend=%d error_diffusion=%d icc=%d target_lut=%d corner=%g blur=%g background=%d border=%d transparency=%g visualize=%d interpolated=%d drawable_flip=%d planes=%d target_rotation=%d\n",
                   !!params->distort_params, !!params->blend_params, !!params->error_diffusion,
                   !!p->icc_profile, !!target->lut, params->corner_rounding,
                   params->blur_radius, params->background,
                   params->border, params->background_transparency,
                   params->color_map_params && params->color_map_params->visualize_lut,
                   p->is_interpolated, desc->flipped, target->num_planes, target->rotation);
            const struct pl_frame *image = mix->num_frames == 1 ? mix->frames[0] : NULL;
            if (image)
                MP_ERR(vo, "MKS-FUSION image crop=%g,%g,%g,%g alpha=%d rotation=%d overlays=%d lut=%d planes=%d\n",
                       image->crop.x0, image->crop.y0, image->crop.x1, image->crop.y1,
                       image->repr.alpha, image->rotation, image->num_overlays,
                       !!image->lut, image->num_planes);
        }
        return false;
    }
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    const struct mp_image *source = mix->frames[0]->user_data;
    bool compare_this_frame = p->compare_enabled && !p->compare_started && source &&
        isfinite(source->pts) && source->pts != MP_NOPTS_VALUE && frame->current &&
        isfinite(frame->current->pts) && frame->current->pts != MP_NOPTS_VALUE &&
        (mp_yuv_compare_target_matches(source->pts, frame->current->pts,
                                      p->compare_target_pts)
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
         || (p->atlas_reuse && p->atlas_phase == MP_ATLAS_WAIT_T2 &&
             mp_yuv_compare_target_matches(source->pts, frame->current->pts,
                                           p->atlas_second_pts))
#endif
        );
    // A compare session always displays ordinary A. Preliminary B is legal
    // only inside the explicitly armed, swap-protected one-frame transaction.
    if (p->compare_requested && !compare_this_frame)
        return false;
#else
    if (p->compare_requested)
        return false; // unsupported diagnostic must never become production B
#endif
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (compare_this_frame && p->atlas_reuse &&
        (p->atlas_phase == MP_ATLAS_T1_REDRAW || p->atlas_phase == MP_ATLAS_WAIT_T2))
        return atlas_consumer_render(vo, frame, mix, target, params, swframe, desc);
    // With reuse enabled only the pending seed may open the anchor
    // transaction; every later phase must never re-seed or rebuild a table.
    if (compare_this_frame && p->compare_atlas &&
        (!p->atlas_reuse || p->atlas_phase == MP_ATLAS_SEED ||
         p->atlas_phase == MP_ATLAS_SEED_PENDING))
        return atlas_render(vo, frame, mix, target, params, swframe, desc);
    // While the reuse session waits (ACK, pending polls, done, rejected) a
    // seed-PTS redraw must never fall into the legacy ROI compare arm: the
    // reuse poll owns all transactions there and nobody would consume it.
    // Keep displaying original A through the ordinary fused path instead.
    if (compare_this_frame && p->atlas_reuse)
        return false;
#elif defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
    if (compare_this_frame && p->compare_atlas)
        return atlas_render(vo, frame, mix, target, params, swframe, desc);
#endif
#endif
    // A fresh wrapper scopes the backend's sticky success receipt to this draw.
    {
        pl_tex_destroy(p->gpu, &p->fusion_target);
        p->fusion_target = pl_opengl_wrap(p->gpu, pl_opengl_wrap_params(
            .framebuffer = desc->framebuffer, .width = desc->w, .height = desc->h,
            .iformat = GL_RGBA16F, // logical output; physical YUV drawable
            .output_encoding = PL_OPENGL_OUTPUT_RGB16F_TO_BT2020_FULL_YUV,
        ));
        p->fusion_generation = desc->generation;
    }
    if (!p->fusion_target)
        return false;
    struct pl_frame fused = *target;
    fused.planes[0].texture = p->fusion_target;
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (compare_this_frame)
    {
        p->compare_started = true;
        p->compare_active = true;
        p->compare_id++;
        p->compare_generation = desc->generation;
        p->compare_frame_id = frame->frame_id; p->compare_frame_token = desc->token;
        p->compare_source_pts = source->pts;
        p->compare_current_pts = frame->current->pts;
        p->compare_width = desc->w; p->compare_height = desc->h;
        p->compare_w = MPMIN(64, desc->w);
        p->compare_h = MPMIN(64, desc->h);
        p->compare_x = (desc->w - p->compare_w) / 2;
        p->compare_y = (desc->h - p->compare_h) / 2;
        p->compare_num_words = (size_t)p->compare_w * p->compare_h *
                               (p->compare_sample_tap ? 26 : 14) *
                               (p->compare_repeat && !p->compare_uv_single ? 2 : 1);
        p->compare_words = talloc_zero_array(p, uint32_t, p->compare_num_words);
        if (p->compare_uv_probe) {
            p->compare_num_probe_words = (size_t)p->compare_w * p->compare_h *
                                         (p->compare_uv_single ? 10 : 20);
            p->compare_probe_words = talloc_zero_array(p, uint32_t,
                                                       p->compare_num_probe_words);
        }
        p->compare_deadline = mp_time_ns() + INT64_C(15000000000);
        struct mks_yuv_compare_roi roi = {
            p->compare_x, p->compare_y, p->compare_w, p->compare_h,
        };
        // Android must prohibit any preliminary B swap before lib can issue B.
        bool android_armed = mks_android_yuv_compare_arm(p->ra_ctx,
            p->compare_id, desc->generation, roi);
        bool lib_armed = android_armed && p->compare_words &&
            (!p->compare_uv_probe || p->compare_probe_words) &&
            pl_opengl_yuv_compare_arm(p->gpu, p->fusion_target,
                &(struct pl_opengl_yuv_compare_params) {
                    .capture_id = p->compare_id, .generation = desc->generation,
                    .baseline = swframe->fbo, .x = roi.x, .y = roi.y,
                    .width = roi.w, .height = roi.h,
#if defined(PL_HAVE_MKS_YUV_COMPARE_REPEAT)
                    .repeat = p->compare_repeat,
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_SAMPLE_TAP)
                    .sample_tap = p->compare_sample_tap,
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_INVARIANT_B)
                    .invariant_b = p->compare_invariant_b,
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_UV_ORACLE)
                    .uv_oracle = p->compare_uv_oracle,
                    .uv_oracle_use_a = p->compare_uv_oracle_use_a,
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_PROBE)
                    .uv_oracle_probe = p->compare_uv_probe,
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_SINGLE_PAIR)
                    .uv_oracle_single_pair = p->compare_uv_single,
#endif
#if defined(PL_HAVE_MKS_YUV_COMPARE_QUAD_ORDER)
                    .quad_order = p->compare_quad_order,
                    .quad_order_reverse = p->compare_quad_reverse,
#endif
                });
        MP_ERR(vo, "MKS-YUV-COMPARE event=armed id=%"PRIu64" generation=%"PRIu64" source_pts=%.9f android=%d lib=%d\n",
               p->compare_id, desc->generation, source->pts, android_armed, lib_armed);
        if (!lib_armed) {
            const struct pl_tex_params *base = &swframe->fbo->params;
            pl_fmt fmt = base->format;
            MP_ERR(vo, "MKS-YUV-COMPARE event=arm-input id=%"PRIu64" target=%dx%d baseline=%dx%d depth=%d renderable=%d format=%s type=%d components=%d bits=%d,%d,%d,%d roi=%d,%d,%d,%d\n",
                   p->compare_id, p->fusion_target->params.w, p->fusion_target->params.h,
                   base->w, base->h, base->d, base->renderable, fmt ? fmt->name : "unknown",
                   fmt ? fmt->type : -1, fmt ? fmt->num_components : -1,
                   fmt ? fmt->component_depth[0] : -1, fmt ? fmt->component_depth[1] : -1,
                   fmt ? fmt->component_depth[2] : -1, fmt ? fmt->component_depth[3] : -1,
                   roi.x, roi.y, roi.w, roi.h);
            // Generic unwrap cannot expose a pseudo framebuffer wrapper.
            // The library's private arm-rejected log is authoritative for IDs.
            compare_fail_transaction(vo, "arm-failed");
            return false;
        }
    }
#endif
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_active) {
        if (!compare_current(p)) {
            compare_fail_transaction(vo, "pre-render-context-mismatch");
            return false;
        }
        p->compare_epoch_valid = pl_opengl_error_epoch(p->gpu, &p->compare_error_epoch);
        if (!p->compare_epoch_valid)
            return false;
    }
#endif
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    // Table-drive receipt: in production table mode a fused frame is only
    // valid when the render actually sampled through the attached table. A
    // renderer-internal retry (e.g. after the dispatch dropped a
    // size-mismatched table) can otherwise report success while the frame
    // was fused untabled — the known quality regression. Reject such frames
    // so the fusion failure path re-renders the ordinary output.
    uint64_t table_serial_before = p->fusion_table_was_enabled
        ? pl_opengl_uv_atlas_table_pass_serial(p->gpu) : 0;
#endif
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_render_begin(fgt, 1); }
    bool rendered = pl_render_image_mix(p->rr, mix, &fused, params);
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && mks_fgt_ready(fgt)) mks_fgt_render_end(fgt, rendered); }
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (rendered && p->fusion_table_was_enabled &&
        !p->compare_requested && !p->compare_started && !p->compare_active &&
        pl_opengl_uv_atlas_table_pass_serial(p->gpu) == table_serial_before) {
        MP_ERR(vo, "MKS-FUSION-TABLE event=table-pass-missed\n");
        rendered = false;
    }
#endif
    // Validate submission errors before granting the one-shot swap receipt.
    // This replaces the ordinary end-of-draw flush on successful fusion.
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_active && !compare_current(p)) {
        compare_fail_transaction(vo, "post-render-context-mismatch");
        return false;
    }
#endif
    if (rendered)
        pl_gpu_flush(p->gpu);
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_active) {
        // One renderer invocation and lib dispatch use frozen input state.
        // Oracle orders A1/barrier/B1 (then A2/barrier/B2 unless single-pair).
        // Only the last A receives presentation credit after all required
        // draws/barriers succeed. The original CSC later overwrites the last
        // diagnostic B before the unique swap; no B swap is allowed.
        bool baseline_ok = rendered && compare_current(p) &&
            compare_baseline_ok(p);
        uint64_t after = 0;
        p->compare_a_ready = baseline_ok && p->compare_epoch_valid &&
            pl_opengl_error_epoch(p->gpu, &after) && after == p->compare_error_epoch;
        return false;
    }
#endif
    bool confirmed = rendered && pl_opengl_yuv_output_ok(p->gpu, p->fusion_target);
    // Faults are opt-in, once per VO, and only after a proven legal draw.
    if (confirmed && p->fusion_fault && !p->fusion_fault_fired) {
        p->fusion_fault_fired = true;
        if (p->fusion_fault == 2) {
            uint64_t before = 0, after = 0;
            bool before_ok = pl_opengl_error_epoch(p->gpu, &before);
            // INVALID_ENUM is a standard recoverable error with no GL state
            // mutation. Backend checking must observe it and set its epoch.
            if (before_ok) {
                struct GL *gl = ra_gl_get(p->ra_ctx->ra);
                gl->Enable(0xffffffffu);
                confirmed = pl_opengl_yuv_output_ok(p->gpu, p->fusion_target);
            } else {
                confirmed = false;
            }
            bool after_ok = before_ok && pl_opengl_error_epoch(p->gpu, &after);
            MP_ERR(vo, "MKS-FUSION fault=gl-invalid-enum once=1 injected=%d before_ok=%d after_ok=%d epoch_before=%"PRIu64" epoch_after=%"PRIu64" confirmed=%d\n",
                   before_ok, before_ok, after_ok, before, after, confirmed);
        } else {
            confirmed = false;
            MP_ERR(vo, "MKS-FUSION fault=simulated-commit once=1 no_gl_error_injected=1\n");
        }
    }
    if (p->ra_ctx->fns->yuv_fusion_commit(p->ra_ctx, desc, confirmed)) {
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
        // Only a committed fused frame proves the consumer path recovered.
        p->table_consume_failures = 0;
#endif
        return true;
    }
    p->fusion_draw_failures++;
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->fusion_table_was_enabled && p->fusion_table_state != FT_OFF) {
        // Production pairing: this failure originated in the table path (the
        // table is dropped and its failure budget governs retries). Keep
        // fusion armed so the state machine can reseed at the new geometry —
        // the pairing gate still blocks any untabled fusion, and the table
        // budget bounds reseeds. A geometry change must not permanently
        // downgrade the session when the designed recovery is a rebuild.
        p->table_consume_failures++;
        MP_ERR(vo, "MKS-FUSION table-path-failure=%"PRIu64" consume-failures=%d (armed unless budget trips)\n",
               p->fusion_draw_failures, p->table_consume_failures);
        MP_ERR(vo, "MKS-FUSION-TABLE event=fusion-fallback-table-dropped failures=%d\n",
               p->table_failures + 1);
        if (p->table_consume_failures >= 2) {
            p->fusion_table_enabled = false;
            p->fusion_disabled = true;
            MP_ERR(vo, "MKS-FUSION-TABLE event=table-disabled reason=consume-failures count=%d\n",
                   p->table_consume_failures);
        }
        fusion_table_note_failure(vo, "fusion-fallback");
        fusion_table_detach(vo);
        // Caller must re-render the same mix into the original RGB target.
        return false;
    }
#endif
    p->fusion_disabled = true;
    MP_ERR(vo, "MKS-FUSION disabled-after-failure=%"PRIu64" sticky_gpu_error=%d; rendering original RGB path\n",
           p->fusion_draw_failures, pl_gpu_is_failed(p->gpu));
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->fusion_table_state != FT_OFF) {
        MP_ERR(vo, "MKS-FUSION-TABLE event=fusion-fallback-table-dropped failures=%d\n",
               p->table_failures + 1);
        fusion_table_note_failure(vo, "fusion-fallback");
        fusion_table_detach(vo);
    }
#endif
    // Caller must re-render the same mix into the original RGB target.
    return false;
}
#endif

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_fatal_context)
        return VO_FALSE;
    if (p->compare_started) {
        compare_poll(vo);
        // A validated ACK or seed promotion may have unblocked this same
        // invocation. Do not swallow the sole paused seek/redraw frame.
        if (p->compare_started || p->compare_fatal_context)
            return VO_FALSE;
    }
#endif
    if (p->thermal.enabled) {
        p->thermal.draw++;
        p->thermal.redraw += frame->redraw;
        p->thermal.display_synced += frame->display_synced;
        p->thermal.frame_id = frame->frame_id;
        if (frame->current)
            p->thermal.pts = frame->current->pts;
    }
    if (p->gpu_timing) {
        p->gpu_timing_frame_id = frame->frame_id;
        if (frame->current)
            p->gpu_timing_pts = frame->current->pts;
    }
    pl_options pars = p->pars;
    pl_gpu gpu = p->gpu;
    update_options(vo);

    struct pl_render_params params = pars->params;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    bool will_redraw = frame->display_synced && frame->num_vsyncs > 1;
    bool cache_frame = will_redraw || frame->still;
    bool can_interpolate = opts->interpolation && frame->display_synced &&
                           !frame->still && frame->num_frames > 1;
    double pts_offset = can_interpolate ? frame->ideal_frame_vsync : 0;
    params.info_callback = info_callback;
    params.info_priv = vo;
    params.skip_caching_single_frame = !cache_frame;
    params.preserve_mixing_cache = p->next_opts->inter_preserve && !frame->still;
    if (frame->still)
        params.frame_mixer = NULL;

    if (frame->current && frame->current->params.vflip) {
        pl_matrix2x2 m = { .m = {{1, 0}, {0, -1}}, };
        pars->distort_params.transform.mat = m;
        params.distort_params = &pars->distort_params;
    } else {
        params.distort_params = NULL;
    }

    // pl_queue advances its internal virtual PTS and culls available frames
    // based on this value and the VPS/FPS ratio. Requesting a non-monotonic PTS
    // is an invalid use of pl_queue. Reset it if this happens in an attempt to
    // recover as much as possible. Ideally, this should never occur, and if it
    // does, it should be corrected. The ideal_frame_vsync may be negative if
    // the last draw did not align perfectly with the vsync. In this case, we
    // should have the previous frame available in pl_queue, or a reset is
    // already requested. Clamp the check to 0, as we don't have the previous
    // frame in vo_frame anyway.
    struct pl_source_frame vpts;
    if (frame->current && !p->want_reset) {
        if (pl_queue_peek(p->queue, 0, &vpts) &&
            frame->current->pts + MPMAX(0, pts_offset) < vpts.pts)
        {
            MP_VERBOSE(vo, "Forcing queue refill, PTS(%f + %f | %f) < VPTS(%f)\n",
                       frame->current->pts, pts_offset,
                       frame->ideal_frame_vsync_duration, vpts.pts);
            p->want_reset = true;
        }
    }

    // Push all incoming frames into the frame queue
    for (int n = 0; n < frame->num_frames; n++) {
        int id = frame->frame_id + n;

        if (p->want_reset) {
            if (p->thermal.enabled)
                p->thermal.queue_resets++;
            pl_renderer_flush_cache(p->rr);
            pl_queue_reset(p->queue);
            p->last_pts = 0.0;
            p->last_id = 0;
            p->want_reset = false;
        }

        if (id <= p->last_id)
            continue; // ignore already seen frames

        struct mp_image *mpi = mp_image_new_ref(frame->frames[n]);
        struct frame_priv *fp = talloc_zero(mpi, struct frame_priv);
        mpi->priv = fp;
        fp->vo = vo;

        pl_queue_push(p->queue, &(struct pl_source_frame) {
            .pts = mpi->pts,
            .duration = can_interpolate ? frame->approx_duration : 0,
            .frame_data = mpi,
            .map = map_frame,
            .unmap = unmap_frame,
            .discard = discard_frame,
        });
        if (p->thermal.enabled)
            p->thermal.queue_frames++;

        p->last_id = id;
    }

    struct ra_swapchain *sw = p->ra_ctx->swapchain;

    bool pass_colorspace = false;
    struct pl_color_space target_csp = {0};
    // TODO: Implement this for all backends
    if (sw->fns->target_csp)
        target_csp = sw->fns->target_csp(sw);
    if (target_csp.primaries == PL_COLOR_PRIM_UNKNOWN)
        target_csp.primaries = get_best_prim_container(&target_csp.hdr.prim);
    if (!pl_color_transfer_is_hdr(target_csp.transfer)) {
        // limit min_luma to 1000:1 contrast ratio in SDR mode
        if (target_csp.hdr.min_luma > PL_COLOR_SDR_WHITE / PL_COLOR_SDR_CONTRAST)
            target_csp.hdr.min_luma = 0;
        // Don't use reported display peak in SDR mode. Mostly because libplacebo
        // forcefully switches to PQ if hinting hdr metadata, ignoring the transfer
        // set in the hint. But also because setting target peak in SDR mode is
        // very specific usecase, needs proper calibration, users can set it manually.
        target_csp.hdr.max_luma = 0;
        target_csp.hdr.max_cll = 0;
        target_csp.hdr.max_fall = 0;
    }
    // maxFALL in display metadata is in fact MaxFullFrameLuminance. Wayland
    // reports it as maxFALL directly, but this doesn't mean the same thing.
    target_csp.hdr.max_fall = 0;

    struct pl_color_space hint = {0};
    bool target_hint = p->next_opts->target_hint == 1 ||
                       (p->next_opts->target_hint == -1 &&
                        target_csp.transfer != PL_COLOR_TRC_UNKNOWN);
    // Assume HDR is supported, if target_csp() is not available
    // TODO: Remove this fallback when all backends support target_csp()
    bool target_unknown = target_csp.transfer == PL_COLOR_TRC_UNKNOWN;
    if (target_unknown) {
        target_csp = (struct pl_color_space){
            .transfer = opts->target_trc ? opts->target_trc : pl_color_space_hdr10.transfer };
    }
    if (target_hint && frame->current) {
        const struct pl_color_space *source = &frame->current->params.color;
        const struct pl_color_space *target = &target_csp;
        hint = *source;
        // Apply target contrast to the hint, this is important for SDR, because
        // libplacebo defaults to 1000:1 contrast ratio otherwise.
        if (!hint.hdr.min_luma)
            hint.hdr.min_luma = target->hdr.min_luma;
        if (p->next_opts->target_hint_mode == 0) {
            hint = *target;
            if (pl_color_transfer_is_hdr(hint.transfer) && !pl_primaries_valid(&hint.hdr.prim))
                pl_color_space_merge(&hint, source);
            if (target_unknown && !opts->target_trc && !pl_color_transfer_is_hdr(source->transfer))
                hint = *source;
            // Vulkan doesn't have support for gamma 2.2 transfer function,
            // so even though requested preferred color space is gamma 2.2, we
            // fallback to sRGB. sRGB itself is ambiguous, but at least we have
            // options to control the behavior.
            // TODO: Revise this after fix for linear transfers lands in libplacebo.
            // <https://code.videolan.org/videolan/libplacebo/-/merge_requests/759>
            if (hint.transfer == PL_COLOR_TRC_GAMMA22)
                hint.transfer = PL_COLOR_TRC_SRGB;
            // Restore target luminance if it was present, note that we check
            // max_luma only, this make sure that max_cll/max_fall is not take
            // from source.
            if (target->hdr.max_luma) {
                hint.hdr.max_luma = target->hdr.max_luma;
                hint.hdr.min_luma = target->hdr.min_luma;
                hint.hdr.max_cll  = target->hdr.max_cll;
                hint.hdr.max_fall = target->hdr.max_fall;
            }
        }
        if (p->next_opts->target_hint_mode == 2) { // source-dynamic
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                .color      = &hint,
                .metadata   = PL_HDR_METADATA_ANY,
                .scaling    = PL_HDR_NITS,
                .out_min    = !hint.hdr.min_luma ? &hint.hdr.min_luma : NULL,
                .out_max    = &hint.hdr.max_luma,
            ));
            // Set maxCLL to dynamic max luminance. Note that libplacebo uses
            // max luminace as maxCLL in practice.
            hint.hdr.max_cll = hint.hdr.max_luma;
            // Keep maxFALL from static metadata, unless its value is too high.
            // Could be set to 0, but let's keep it for now.
            if (hint.hdr.max_fall > hint.hdr.max_cll)
                hint.hdr.max_fall = 0;
        }
        // Infer missing bits now. This is important so that we don't lose
        // information after user option overrides. For example, if the user
        // sets target_trc to PQ, but the hint(source) is SDR, we want to fill
        // in SDR luminance values instead of the default PQ range.
        struct pl_color_space source_csp = *source;
        pl_color_space_infer_map(&source_csp, &hint);
        // Always prefer target luminance and transfer for inverse tone mapping
        if (pl_color_transfer_is_hdr(target->transfer) && opts->tone_map.inverse) {
            hint.transfer     = target->transfer;
            hint.hdr.max_luma = target->hdr.max_luma;
            hint.hdr.min_luma = target->hdr.min_luma;
            hint.hdr.max_cll  = target->hdr.max_cll;
            hint.hdr.max_fall = target->hdr.max_fall;
        }
        if (p->ra_ctx->fns->pass_colorspace && p->ra_ctx->fns->pass_colorspace(p->ra_ctx))
            pass_colorspace = true;
        if (opts->target_prim)
            hint.primaries = opts->target_prim;
        if (opts->target_gamut) {
            // Ensure resulting gamut still fits inside container
            const struct pl_raw_primaries *gamut, *container;
            gamut = pl_raw_primaries_get(opts->target_gamut);
            container = pl_raw_primaries_get(hint.primaries);
            hint.hdr.prim = pl_primaries_clip(gamut, container);
        }
        if (opts->target_trc)
            hint.transfer = opts->target_trc;
        if (opts->target_peak)
            hint.hdr.max_luma = opts->target_peak;
        if (opts->hdr_reference_white && !pl_color_transfer_is_hdr(hint.transfer))
            hint.hdr.max_luma = opts->hdr_reference_white;
        // Always set maxCLL, display uses this metadata and we shouldn't let it
        // fallback to default value.
        if (!hint.hdr.max_cll)
            hint.hdr.max_cll = hint.hdr.max_luma;
        // If tone mapping is required, adjust maxCLL and maxFALL
        if (source->hdr.max_luma > hint.hdr.max_luma || opts->tone_map.inverse) {
            // Set maxCLL to the target luminance if it's not already lower
            if (!hint.hdr.max_cll || hint.hdr.max_luma < hint.hdr.max_cll || opts->tone_map.inverse)
                hint.hdr.max_cll = hint.hdr.max_luma;
            // There's no reliable way to estimate maxFALL here
            hint.hdr.max_fall = 0;
        }
        if (hint.hdr.max_cll && hint.hdr.max_fall > hint.hdr.max_cll)
            hint.hdr.max_fall = 0;
        apply_target_contrast(p, &hint, hint.hdr.min_luma);
        if (p->icc_profile)
            hint = p->icc_profile->csp;
        if (opts->icc_opts->icc_use_luma) {
            p->icc_params.max_luma = 0.0f;
        } else {
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                .color    = &hint,
                .metadata = PL_HDR_METADATA_HDR10, // use only static HDR nits
                .scaling  = PL_HDR_NITS,
                .out_max  = &p->icc_params.max_luma,
            ));
        }
        pl_icc_update(p->pllog, &p->icc_profile, NULL, &p->icc_params);
        // Update again after possible max_luma change
        if (p->icc_profile)
            hint = p->icc_profile->csp;
        if (!pass_colorspace)
            pl_swapchain_colorspace_hint(p->sw, &hint);
    } else if (!target_hint) {
        if (!hint.hdr.min_luma)
            hint.hdr.min_luma = target_csp.hdr.min_luma;
        pl_swapchain_colorspace_hint(p->sw, NULL);
    }

    struct pl_swapchain_frame swframe;
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    {
        // Diagnostic-only controlled transition harness: on a property delta
        // change, apply a REAL VO-level resize — the same dwidth + resize(vo)
        // chain a live surface change runs — BEFORE the swapchain frame is
        // started (an in-flight frame makes pl_swapchain_resize refuse, and
        // only the VO rectangle would move). The resulting frame size is
        // logged so the evidence shows the resize actually took effect.
        char transition[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.mks.yuv_table_transition", transition);
        int delta = atoi(transition);
        if (delta != p->table_transition_delta) {
            int adjust = delta - p->table_transition_delta;
            p->table_transition_delta = delta;
            if (vo->dwidth && vo->dheight && adjust) {
                // Preserve the video aspect: p->dst is the aspect-fitted rect
                // inside dwidth/dheight, so an equal-delta resize would
                // letterbox and permanently trip the fusion geometry gate.
                int old_w = vo->dwidth, old_h = vo->dheight;
                vo->dwidth += adjust;
                vo->dheight = (int) ((int64_t) vo->dwidth * old_h / old_w);
                // The full real reconfig chain first (native window + YUV
                // wrapper + RA swapchain; android_reconfig honors the armed
                // property as its authoritative size), then VO bookkeeping.
                if (p->ra_ctx->fns->reconfig)
                    p->ra_ctx->fns->reconfig(p->ra_ctx);
                resize(vo);
                MP_ERR(vo, "MKS-FUSION-TABLE event=transition-inject dwidth=%d dheight=%d (delta %+d)\n",
                       vo->dwidth, vo->dheight, adjust);
            }
        }
    }
#endif
    bool should_draw = sw->fns->start_frame(sw, NULL); // for wayland logic
    { struct mks_fgt_owner *fgt = mks_fgt_get_global();
      if (fgt && fgt->enabled && fgt->armed && should_draw)
          mks_fgt_ticket_open(fgt, frame->current ? frame->current->pts : 0,
                              frame->frame_id, 1, 1, MKS_FGT_ROUTE_FUSED); }
    if (!should_draw || !pl_swapchain_start_frame(p->sw, &swframe)) {
        if (frame->current) {
            // Advance the queue state to the current PTS to discard unused frames
            struct pl_queue_params qparams = *pl_queue_params(
                .pts = frame->current->pts + pts_offset,
                .radius = pl_frame_mix_radius(&params),
                .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
            );
#if PL_API_VER >= 340
            qparams.drift_compensation = 0;
#endif
            pl_queue_update(p->queue, NULL, &qparams);
        }
        if ((p->fusion_presentation_guard || p->fusion_frame_trace) &&
            p->ra_ctx->fns->yuv_fusion_abort)
            p->ra_ctx->fns->yuv_fusion_abort(p->ra_ctx);
        return VO_FALSE;
    }

    if (p->compare_requested)
        p->compare_frame_open = true;
    struct ra_yuv_fusion_target fusion_desc = {0};
    bool fusion_available = p->ra_ctx->fns->yuv_fusion_begin &&
        p->ra_ctx->fns->yuv_fusion_commit &&
        p->ra_ctx->fns->yuv_fusion_rgb_ready &&
        p->ra_ctx->fns->yuv_fusion_begin(p->ra_ctx, &fusion_desc);
    p->fusion_presentation_guard |= fusion_available;
    bool fused = false, rgb_rendered = false;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_YUV_OUTPUT)
    uint64_t rgb_error_epoch = 0;
    bool rgb_epoch_valid = false;
#endif
    bool valid = false;
    p->is_interpolated = false;

    // Calculate target
    struct pl_frame target;
    pl_frame_from_swapchain(&target, &swframe);
    bool strict_sw_params = target_hint && !pass_colorspace && p->next_opts->target_hint_strict;
    apply_target_options(p, &target, hint.hdr.min_luma, strict_sw_params);
    if (target.color.transfer == PL_COLOR_TRC_SRGB && frame->current &&
        ((opts->sdr_adjust_gamma == 0 && opts->target_trc == PL_COLOR_TRC_UNKNOWN) ||
         opts->sdr_adjust_gamma == -1))
    {
        switch (frame->current->params.color.transfer) {
        case PL_COLOR_TRC_BT_1886:
        case PL_COLOR_TRC_GAMMA22:
        case PL_COLOR_TRC_SRGB:
            target.color.transfer = frame->current->params.color.transfer;
        }
    }
    if (target.color.transfer == PL_COLOR_TRC_SRGB) {
        // sRGB reference display is pure 2.2 power function, see IEC 61966-2-1-1999.
        if (opts->treat_srgb_as_power22 & 2)
            target.color.transfer = PL_COLOR_TRC_GAMMA22;

        // TODO: Vulkan on Wayland currently interprets VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
        // in ambiguous way, depending if compositor advertises sRGB support.
        // There is currently no clear path forward to resolve this ambiguity.
        // Depending how it's resolved in Wayland Protocol, Mesa, things will
        // change.
        // See: <https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/456>
#ifdef _WIN32
        // Windows uses the sRGB piecewise function. Send piecewise sRGB to
        // Windows in HDR mode so that it can be converted to PQ, the same way
        // as mpv does internally. Note that in SDR mode, even with ACM enabled,
        // Windows assumes the display is sRGB. It doesn't perform gamma
        // conversion, or any conversions would roundtrip back to sRGB.
        // In which case the EOTF depends on the display.
        // Ideally, compositors would agree on how to handle sRGB, but I’ll
        // leave that part of the story for the reader to explore.
        // Note: Older Windows versions, without ACM, were not able to convert
        // sRGB to PQ output. We are not concerned about this case, as it would
        // look wrong anyway.
        bool target_pq = !target_unknown && target_csp.transfer == PL_COLOR_TRC_PQ;
        if (opts->treat_srgb_as_power22 & 4 && target_pq)
            target.color.transfer = PL_COLOR_TRC_SRGB;
#endif
    }
    update_overlays(vo, p->osd_res,
                    (frame->current && opts->blend_subs) ? OSD_DRAW_OSD_ONLY : 0,
                    PL_OVERLAY_COORDS_DST_FRAME, &p->osd_state, &target, frame->current);
    apply_crop(&target, p->dst, swframe.fbo->params.w, swframe.fbo->params.h);
    update_tm_viz(&pars->color_map_params, &target);

    struct pl_frame_mix mix = {0};
    if (frame->current) {
        // Update queue state
        struct pl_queue_params qparams = *pl_queue_params(
            .pts = frame->current->pts + pts_offset,
            .radius = pl_frame_mix_radius(&params),
            .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
            .interpolation_threshold = opts->interpolation_threshold,
        );
#if PL_API_VER >= 340
        qparams.drift_compensation = 0;
#endif

        // Depending on the vsync ratio, we may be up to half of the vsync
        // duration before the current frame time. This works fine because
        // pl_queue will have this frame, unless it's after a reset event. In
        // this case, start from the first available frame.
        struct pl_source_frame first;
        if (pl_queue_peek(p->queue, 0, &first) && qparams.pts < first.pts) {
            if (first.pts != frame->current->pts)
                MP_VERBOSE(vo, "Current PTS(%f) != VPTS(%f)\n", frame->current->pts, first.pts);
            MP_VERBOSE(vo, "Clamping first frame PTS from %f to %f\n", qparams.pts, first.pts);
            qparams.pts = first.pts;
        }
        p->last_pts = qparams.pts;

        switch (pl_queue_update(p->queue, &mix, &qparams)) {
        case PL_QUEUE_ERR:
            MP_ERR(vo, "Failed updating frames!\n");
            goto done;
        case PL_QUEUE_EOF:
            abort(); // we never signal EOF
        case PL_QUEUE_MORE:
            // This is expected to happen semi-frequently near the start and
            // end of a file, so only log it at high verbosity and move on.
            MP_DBG(vo, "Render queue underrun.\n");
            break;
        case PL_QUEUE_OK:
            break;
        }

        // Update source crop and overlays on all existing frames. We
        // technically own the `pl_frame` struct so this is kosher. This could
        // be partially avoided by instead flushing the queue on resizes, but
        // doing it this way avoids unnecessarily re-uploading frames.
        for (int i = 0; i < mix.num_frames; i++) {
            struct pl_frame *image = (struct pl_frame *) mix.frames[i];
            struct mp_image *mpi = image->user_data;
            struct frame_priv *fp = mpi->priv;
            // The queue frame always carries classic source-space geometry;
            // the renderer's acquire moves its own local copy into the
            // verified physical lease space (hwdec_lease_rescale_frame).
            apply_crop(image, p->src, vo->params->w, vo->params->h);
            if (opts->blend_subs) {
                if (frame->redraw)
                    p->osd_sync++;
                if (fp->osd_sync < p->osd_sync) {
                    float w = pl_rect_w(opts->blend_subs == BLEND_SUBS_VIDEO ? image->crop : target.crop);
                    float h = pl_rect_h(opts->blend_subs == BLEND_SUBS_VIDEO ? image->crop : target.crop);
                    float rx = w / pl_rect_w(image->crop);
                    float ry = h / pl_rect_h(image->crop);
                    struct mp_osd_res res = {
                        .w = w,
                        .h = h,
                        .ml = -image->crop.x0 * rx,
                        .mr = (image->crop.x1 - vo->params->w) * rx,
                        .mt = -image->crop.y0 * ry,
                        .mb = (image->crop.y1 - vo->params->h) * ry,
                        .display_par = 1.0,
                    };
                    enum pl_overlay_coords rel = opts->blend_subs == BLEND_SUBS_VIDEO
                        ? PL_OVERLAY_COORDS_SRC_CROP : PL_OVERLAY_COORDS_DST_CROP;
                    update_overlays(vo, res, OSD_DRAW_SUB_ONLY,
                                    rel, &fp->subs, image, mpi);
                    fp->osd_sync = p->osd_sync;
                }
            } else {
                // Disable overlays when blend_subs is disabled
                image->num_overlays = 0;
                fp->osd_sync = 0;
            }

            // Update the frame signature to include the current OSD sync
            // value, in order to disambiguate between identical frames with
            // modified OSD. Shift the OSD sync value by a lot to avoid
            // collisions with low signature values.
            //
            // This is safe to do because `pl_frame_mix.signature` lives in
            // temporary memory that is only valid for this `pl_queue_update`.
            ((uint64_t *) mix.signatures)[i] ^= fp->osd_sync << 48;
        }

        // Update dynamic hook parameters
        for (int i = 0; i < pars->params.num_hooks; i++)
            update_hook_opts_dynamic(p, p->hooks[i], frame->current);
    }

    // Optional fast path for Profile 5 rendered to an SDR (BT.1886) target:
    // the libplacebo optimization has its own strict path checks and falls
    // back to regular rendering whenever this frame needs other color work.
    // --dovi-p5-fast-path=auto (the default) limits it to frames actually
    // imported through the Android external YUV hwdec path, and never
    // overrides a --gamut-mapping-mode or --dscale the user set explicitly.
#if HAVE_PL_EXTERNAL_YUV && \
    (HAVE_PL_DOVI_LINEAR_OPTIMIZE || HAVE_PL_DOVI_LINEAR_DISABLE)
    struct pl_color_map_params p5_sdr_color_map;
    if (frame->current && frame->current->params.dv_profile == 5 &&
        frame->current->params.repr.dovi &&
        target.color.transfer == PL_COLOR_TRC_BT_1886 &&
        params.color_map_params &&
        (p->next_opts->dovi_p5_fast_path == 1 ||
         (p->next_opts->dovi_p5_fast_path == 0 &&
          frame->current->imgfmt == IMGFMT_MEDIACODEC &&
          p->hwdec_mapper &&
          p->hwdec_mapper->dst_params.imgfmt == IMGFMT_MEDIACODEC_YUV))) {
        p5_sdr_color_map = *params.color_map_params;
        if (opts->tone_map.gamut_mode == GAMUT_AUTO)
            p5_sdr_color_map.gamut_mapping = &pl_gamut_map_clip;
        params.color_map_params = &p5_sdr_color_map;
#if HAVE_PL_DOVI_LINEAR_OPTIMIZE
        params.optimize_dovi_linear_decode = true;
#else
        // Current libplacebo fork: the optimization is enabled by default,
        // the switch only turns it off.
        params.disable_dovi_linear_decode = false;
#endif
        if (opts->scaler[SCALER_DSCALE].kernel.function == SCALER_INHERIT)
            params.downscaler = NULL;
    }
#endif

#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_READ)
    // Only the consumer redraws (t1/t2) run with single-frame caching
    // disabled; the seed anchor must keep the exact pass reuse the schema9
    // anchor was validated with, and an invalid configuration must behave
    // exactly like the ordinary path.
    if (p->atlas_reuse && p->compare_enabled &&
        (p->atlas_phase == MP_ATLAS_T1_REDRAW || p->atlas_phase == MP_ATLAS_WAIT_T2)) {
        params.skip_caching_single_frame = true;
        params.preserve_mixing_cache = false;
    }
#endif
    // The original swapchain FBO is retained for a same-draw fallback.
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_YUV_OUTPUT)
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    // Production UV table: poll any in-flight seed, drop dead tables, and
    // seed on the first fully gated frame so the ordinary path stays on
    // screen until the table is ready. The identity gate runs after the
    // poll so a seed promoted to READY this frame is checked against this
    // frame's source/scaler identity before its first table-driven draw.
    bool fusion_table_block = false;
    bool compare_session = p->compare_requested || p->compare_started || p->compare_active;
    if (p->fusion_table_enabled || p->fusion_table_was_enabled) {
        fusion_table_poll(vo);
        if (p->fusion_table_state == FT_READY) {
            const struct mp_image *mpi_now = frame->current;
            const struct gl_video_opts *gopts = p->opts_cache->opts;
            int dscale = gopts->scaler[SCALER_DSCALE].kernel.function == SCALER_INHERIT
                ? gopts->scaler[SCALER_SCALE].kernel.function
                : gopts->scaler[SCALER_DSCALE].kernel.function;
            if (!pl_opengl_uv_atlas_table_active(p->gpu) || !mpi_now ||
                mpi_now->w != p->table_src_w || mpi_now->h != p->table_src_h ||
                gopts->scaler[SCALER_SCALE].kernel.function != p->table_scaler_kernel ||
                dscale != p->table_dscale_kernel ||
                gopts->linear_upscaling != p->table_linear_up ||
                gopts->linear_downscaling != p->table_linear_down ||
                gopts->correct_downscaling != p->table_correct_down) {
                MP_ERR(vo, "MKS-FUSION-TABLE event=table-invalidated\n");
                fusion_table_detach(vo);
            }
        }
        // Mutual exclusion: a configured diagnostic compare session owns the
        // final-pass machinery (its arm hooks live inside fusion_render);
        // production seeding waits until it is gone and must not block the
        // fusion path the diagnostics ride on.
        if (p->fusion_table_enabled && p->fusion_table_state == FT_OFF &&
            !can_interpolate && fusion_available && !compare_session &&
            fusion_frame_rejects(vo, frame, &mix, &target, &params, &swframe, &fusion_desc) == 0)
            fusion_table_block = fusion_table_seed(vo, frame, &swframe, &fusion_desc);
        // Production pairing: without a READY table the tableless fusion
        // (whose UV rounding regression motivated the table) must not run;
        // fall back to the split ordinary path. An explicit table-off
        // session (fusion_table_was_enabled == false) keeps plain fusion,
        // and an active compare session keeps its own fusion entry alive.
        if ((p->fusion_table_enabled || p->fusion_table_was_enabled) &&
            p->fusion_table_state != FT_READY && !compare_session)
            fusion_table_block = true;
    }
    if (fusion_available && !can_interpolate && !fusion_table_block)
        fused = fusion_render(vo, frame, &mix, &target, &params, &swframe, &fusion_desc);
#else
    if (fusion_available && !can_interpolate)
        fused = fusion_render(vo, frame, &mix, &target, &params, &swframe, &fusion_desc);
#endif
#else
    (void) fusion_available;
#endif
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_terminal)
        goto done;
    if (p->compare_active) {
        if (!p->compare_a_ready)
            goto done;
        rgb_rendered = true;
    }
#endif
    // Render frame; never present old offscreen content after a failed fusion.
    if (!fused && !rgb_rendered) {
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_YUV_OUTPUT)
        if (fusion_available)
            rgb_epoch_valid = pl_opengl_error_epoch(gpu, &rgb_error_epoch);
#endif
        { struct mks_fgt_owner *fgt = mks_fgt_get_global();
          if (fgt && mks_fgt_ready(fgt)) mks_fgt_render_begin(fgt, 2); }
        bool rendered = pl_render_image_mix(p->rr, &mix, &target, &params);
        { struct mks_fgt_owner *fgt = mks_fgt_get_global();
          if (fgt && mks_fgt_ready(fgt)) mks_fgt_render_end(fgt, rendered); }
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
        if (!rendered && p->fusion_table_state == FT_SEEDING) {
            // The seed anchor transaction rides inside this render's final
            // pass; its failure must not drop the frame. Count the failed
            // seed, tear the dead anchor down, and re-render once through
            // the anchor-free ordinary path. When the anchor cannot be
            // released (foreign owner / held transaction) the retry is
            // skipped and the frame falls through as before.
            MP_ERR(vo, "MKS-FUSION-TABLE event=seed-frame-failed failures=%d\n",
                   p->table_failures + 1);
            fusion_table_note_failure(vo, "seed-frame-render");
            if (fusion_table_detach(vo)) {
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_YUV_OUTPUT)
                // Re-arm the presentation receipt BEFORE the salvaged render:
                // the failed attempt may have consumed a recoverable error
                // and bumped the epoch, but the second render's own errors
                // must still fail the receipt, not be baked into its base.
                if (fusion_available)
                    rgb_epoch_valid = pl_opengl_error_epoch(gpu, &rgb_error_epoch);
#endif
                { struct mks_fgt_owner *fgt = mks_fgt_get_global();
                  if (fgt && mks_fgt_ready(fgt)) mks_fgt_render_begin(fgt, 3); }
                rendered = pl_render_image_mix(p->rr, &mix, &target, &params);
                { struct mks_fgt_owner *fgt = mks_fgt_get_global();
                  if (fgt && mks_fgt_ready(fgt)) mks_fgt_render_end(fgt, rendered); }
                if (rendered)
                    MP_ERR(vo, "MKS-FUSION-TABLE event=seed-frame-salvaged\n");
            }
        }
#endif
        if (!rendered) {
            MP_ERR(vo, "Failed rendering frame!\n");
            goto done;
        }
        rgb_rendered = true;
    }

    struct pl_frame ref_frame;
    pl_frames_infer_mix(p->rr, &mix, &target, &ref_frame);

    mp_mutex_lock(&vo->params_mutex);
    p->target_params = (struct mp_image_params){
        .imgfmt_name = swframe.fbo->params.format
                        ? swframe.fbo->params.format->name : NULL,
        .w = mp_rect_w(p->dst),
        .h = mp_rect_h(p->dst),
        .color = pass_colorspace ? hint : target.color,
        .repr = target.repr,
        .rotate = target.rotation,
    };
    vo->target_params = &p->target_params;

    if (vo->params) {
        // Augment metadata with peak detection max_pq_y / avg_pq_y
        vo->has_peak_detect_values = pl_renderer_get_hdr_metadata(p->rr, &vo->params->color.hdr);
    }
    mp_mutex_unlock(&vo->params_mutex);

    p->is_interpolated = pts_offset != 0 && mix.num_frames > 1;
    if (p->thermal.enabled) {
        p->thermal.w = mp_rect_w(p->dst);
        p->thermal.h = mp_rect_h(p->dst);
    }
    valid = true;
    // fall through

done:
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_terminal)
        return VO_TRUE;
    if (p->compare_active && !compare_current(p)) {
        compare_fail_transaction(vo, "pre-flush-context-mismatch");
        return VO_TRUE;
    }
#endif
    if (!valid && !fusion_available) // clear with purple to indicate error
        pl_tex_clear(gpu, swframe.fbo, (float[4]){ 0.5, 0.0, 1.0, 1.0 });

    bool flush_context_ok = true;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_active)
        flush_context_ok = compare_current(p);
#endif
    if (!fused && flush_context_ok)
        pl_gpu_flush(gpu);
    if (fusion_available && !fused) {
        bool current_rgb_ok = false;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_YUV_OUTPUT)
        uint64_t after = 0;
        current_rgb_ok = valid && rgb_rendered && rgb_epoch_valid && flush_context_ok &&
            pl_opengl_error_epoch(gpu, &after) && after == rgb_error_epoch;
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
        if (p->compare_active) {
            uint64_t compare_after = 0;
            bool baseline_ok = valid && rgb_rendered && p->compare_a_ready &&
                compare_current(p) && compare_baseline_ok(p);
            current_rgb_ok = baseline_ok && p->compare_epoch_valid &&
                pl_opengl_error_epoch(gpu, &compare_after) &&
                compare_after == p->compare_error_epoch;
        }
#endif
#endif
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
        if (p->compare_atlas && p->compare_active && !current_rgb_ok) {
            compare_fail_transaction(vo, "atlas-postflush-receipt-failed");
            return VO_TRUE;
        }
#endif
        p->ra_ctx->fns->yuv_fusion_rgb_ready(p->ra_ctx, &fusion_desc, current_rgb_ok);
    }
    (void) rgb_rendered;
    if (valid && p->fusion_frame_trace && p->ra_ctx->fns->yuv_frame_trace &&
        mix.num_frames == 1 && mix.frames[0] && mix.frames[0]->user_data && frame->current)
    {
        const struct mp_image *source = mix.frames[0]->user_data;
        struct ra_yuv_frame_trace trace = {
            .frame_id = frame->frame_id, .source_pts = source->pts,
            .current_pts = frame->current->pts, .duration = frame->approx_duration,
            .still = frame->still, .redraw = frame->redraw, .repeat = frame->repeat,
        };
        if (source->pts != MP_NOPTS_VALUE && frame->current->pts != MP_NOPTS_VALUE &&
            isfinite(source->pts) && isfinite(frame->current->pts))
            p->ra_ctx->fns->yuv_frame_trace(p->ra_ctx, &fusion_desc, &trace);
    }
    p->frame_pending = flush_context_ok;
    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    if (p->thermal.enabled)
        p->thermal.flip++;

    // vo.c can flip after draw_frame returned false. Do not submit/swap an
    // old offscreen after any successful experimental fusion session.
    if (p->fusion_presentation_guard && !p->frame_pending)
        return;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_fatal_context)
        return;
    if (p->compare_started && p->frame_pending && !compare_current(p)) {
        compare_fail_transaction(vo, "pre-submit-context-or-identity-mismatch");
        return;
    }
#endif
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
    if (p->compare_atlas && p->compare_started && p->frame_pending &&
        !compare_baseline_ok(p)) {
        compare_fail_transaction(vo, "atlas-presubmit-receipt-failed");
        return;
    }
#endif
    if (p->frame_pending) {
        if (p->thermal.enabled)
            p->thermal.submit++;
        bool submitted = pl_swapchain_submit_frame(p->sw);
        { struct mks_fgt_owner *fgt = mks_fgt_get_global();
          if (fgt && mks_fgt_ready(fgt)) mks_fgt_submit_result(fgt, submitted); }
        p->compare_frame_open = false;
        p->frame_pending = false;
        // B2: flip_close NOT called here — swap_result in android_swap_buffers
        // handles the presentation axis for the successful submit path.
        if (!submitted) {
            MP_ERR(vo, "Failed presenting frame!\n");
            { struct mks_fgt_owner *fgt = mks_fgt_get_global();
              if (fgt && mks_fgt_ready(fgt)) mks_fgt_flip_close(fgt, true, false, false); }
            if ((p->fusion_presentation_guard || p->fusion_frame_trace) &&
                p->ra_ctx->fns->yuv_fusion_abort)
                p->ra_ctx->fns->yuv_fusion_abort(p->ra_ctx);
            if (p->fusion_presentation_guard)
                return;
        }
    }

#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
    if (p->compare_atlas && p->compare_started) {
        uint64_t epoch = 0;
        if (!compare_current(p) || !compare_baseline_ok(p) ||
            !pl_opengl_error_epoch(p->gpu, &epoch) || epoch != p->compare_error_epoch) {
            compare_fail_transaction(vo, "atlas-postsubmit-receipt-failed");
            return;
        }
    }
#endif
    sw->fns->swap_buffers(sw);
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    compare_poll(vo);
#endif
    mks_thermal_report(&p->thermal, false);
    gpu_timing_report(vo, false);
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    if (sw->fns->get_vsync)
        sw->fns->get_vsync(sw, info);
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;
    if (ra_hwdec_get(&p->hwdec_ctx, format))
        return true;

    bool supported = format_supported(vo, format, false);
    if (!supported)
        supported = format_supported(vo, format, true);

    return supported;
}

static void resize(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->compare_fatal_context)
        return;
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    if (vo->dwidth && vo->dheight) {
        gpu_ctx_resize(p->context, vo->dwidth, vo->dheight);
        vo->want_redraw = true;
    }

    if (mp_rect_equals(&p->src, &src) &&
        mp_rect_equals(&p->dst, &dst) &&
        osd_res_equals(p->osd_res, osd))
        return;

    p->osd_sync++;
    p->osd_res = osd;
    p->src = src;
    p->dst = dst;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    if (p->compare_fatal_context)
        return -1;
    if (!p->ra_ctx->fns->reconfig(p->ra_ctx))
        return -1;

    resize(vo);
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = NULL;
    mp_mutex_unlock(&vo->params_mutex);
    return 0;
}

// Takes over ownership of `icc`. Can be used to unload profile (icc.len == 0)
static bool update_icc(struct priv *p, struct bstr icc)
{
    struct pl_icc_profile profile = {
        .data = icc.start,
        .len  = icc.len,
    };

    pl_icc_profile_compute_signature(&profile);

    bool ok = pl_icc_update(p->pllog, &p->icc_profile, &profile, &p->icc_params);
    talloc_free(icc.start);
    return ok;
}

// Returns whether the ICC profile was updated (even on failure)
static bool update_auto_profile(struct priv *p, int *events)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (!opts->icc_opts || !opts->icc_opts->profile_auto || p->icc_path)
        return false;

    MP_VERBOSE(p, "Querying ICC profile...\n");
    bstr icc = {0};
    int r = p->ra_ctx->fns->control(p->ra_ctx, events, VOCTRL_GET_ICC_PROFILE, &icc);

    if (r != VO_NOTAVAIL) {
        if (r == VO_FALSE) {
            MP_WARN(p, "Could not retrieve an ICC profile.\n");
        } else if (r == VO_NOTIMPL) {
            MP_ERR(p, "icc-profile-auto not implemented on this platform.\n");
        }

        update_icc(p, icc);
        return true;
    }

    return false;
}

static void video_screenshot(struct vo *vo, struct voctrl_screenshot *args)
{
    struct priv *p = vo->priv;
    pl_options pars = p->pars;
    pl_gpu gpu = p->gpu;
    pl_tex fbo = NULL;
    struct osd_state screenshot_osd = {0};
    args->res = NULL;

    update_options(vo);
    struct pl_render_params params = pars->params;
    params.info_callback = NULL;
    params.skip_caching_single_frame = true;
    params.preserve_mixing_cache = false;
    params.frame_mixer = NULL;

    struct pl_peak_detect_params peak_params;
    if (params.peak_detect_params) {
        peak_params = *params.peak_detect_params;
        params.peak_detect_params = &peak_params;
        peak_params.allow_delayed = false;
    }

    // Retrieve the current frame from the frame queue
    struct pl_frame_mix mix;
    enum pl_queue_status status;
    struct pl_queue_params qparams = *pl_queue_params(
        .pts = p->last_pts,
    );
#if PL_API_VER >= 340
        qparams.drift_compensation = 0;
#endif
    status = pl_queue_update(p->queue, &mix, &qparams);
    mp_assert(status != PL_QUEUE_EOF);
    if (status == PL_QUEUE_ERR) {
        MP_ERR(vo, "Unknown error occurred while trying to take screenshot!\n");
        return;
    }
    if (!mix.num_frames) {
        MP_ERR(vo, "No frames available to take screenshot of, is a file loaded?\n");
        return;
    }

    // Passing an interpolation radius of 0 guarantees that the first frame in
    // the resulting mix is the correct frame for this PTS
    struct pl_frame image = *(struct pl_frame *) mix.frames[0];
    struct mp_image *mpi = image.user_data;
    struct mp_rect src = p->src, dst = p->dst;
    struct mp_osd_res osd = p->osd_res;
    if (!args->scaled) {
        int w, h;
        mp_image_params_get_dsize(&mpi->params, &w, &h);
        if (w < 1 || h < 1)
            return;

        int src_w = mpi->params.w;
        int src_h = mpi->params.h;
        src = (struct mp_rect) {0, 0, src_w, src_h};
        dst = (struct mp_rect) {0, 0, w, h};

        if (mp_image_crop_valid(&mpi->params))
            src = mpi->params.crop;

        if (mpi->params.rotate % 180 == 90) {
            MPSWAP(int, w, h);
            MPSWAP(int, src_w, src_h);
        }
        mp_rect_rotate(&src, src_w, src_h, mpi->params.rotate);
        mp_rect_rotate(&dst, w, h, mpi->params.rotate);

        osd = (struct mp_osd_res) {
            .display_par = 1.0,
            .w = mp_rect_w(dst),
            .h = mp_rect_h(dst),
        };
    }

    // Create target FBO, try high bit depth first
    int mpfmt;
    for (int depth = args->high_bit_depth ? 16 : 8; depth; depth -= 8) {
        if (depth == 16) {
            mpfmt = IMGFMT_RGBA64;
        } else {
            mpfmt = p->ra_ctx->opts.want_alpha ? IMGFMT_RGBA : IMGFMT_RGB0;
        }
        pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, depth, depth,
                                 PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
        if (!fmt)
            continue;

        fbo = pl_tex_create(gpu, pl_tex_params(
            .w = osd.w,
            .h = osd.h,
            .format = fmt,
            .blit_dst = true,
            .renderable = true,
            .host_readable = true,
            .storable = fmt->caps & PL_FMT_CAP_STORABLE,
        ));
        if (fbo)
            break;
    }

    if (!fbo) {
        MP_ERR(vo, "Failed creating target FBO for screenshot!\n");
        return;
    }

    struct pl_frame target = {
        .repr = pl_color_repr_rgb,
        .num_planes = 1,
        .planes[0] = {
            .texture = fbo,
            .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
    };

    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (args->scaled) {
        // Apply target LUT, ICC profile and CSP override only in window mode
        apply_target_options(p, &target, 0, false);
    } else if (args->native_csp) {
        target.color = image.color;
    } else {
        target.color = pl_color_space_srgb;
    }

    // sRGB reference display is pure 2.2 power function, see IEC 61966-2-1-1999.
    // Round-trip back to sRGB if the source is also sRGB. In other cases, we
    // use piecewise sRGB transfer function, as this is likely the be expected
    // for file encoding.
    if (opts->treat_srgb_as_power22 & 1 &&
        target.color.transfer == PL_COLOR_TRC_SRGB &&
        mpi->params.color.transfer == PL_COLOR_TRC_SRGB)
    {
        target.color.transfer = PL_COLOR_TRC_GAMMA22;
    }

    // The queue-frame crop stays in classic source space; the renderer's
    // acquire moves its own local copy into the verified physical space.
    apply_crop(&image, src, mpi->params.w, mpi->params.h);
    apply_crop(&target, dst, fbo->params.w, fbo->params.h);
    update_tm_viz(&pars->color_map_params, &target);

    int osd_flags = 0;
    if (!args->subs)
        osd_flags |= OSD_DRAW_OSD_ONLY;
    if (!args->osd)
        osd_flags |= OSD_DRAW_SUB_ONLY;

    if (opts->blend_subs) {
        float w = pl_rect_w(opts->blend_subs == BLEND_SUBS_VIDEO ? image.crop : target.crop);
        float h = pl_rect_h(opts->blend_subs == BLEND_SUBS_VIDEO ? image.crop : target.crop);
        float rx = w / pl_rect_w(image.crop);
        float ry = h / pl_rect_h(image.crop);
        struct mp_osd_res res = {
            .w = w,
            .h = h,
            .ml = -image.crop.x0 * rx,
            .mr = (image.crop.x1 - vo->params->w) * rx,
            .mt = -image.crop.y0 * ry,
            .mb = (image.crop.y1 - vo->params->h) * ry,
            .display_par = 1.0,
        };
        enum pl_overlay_coords rel = opts->blend_subs == BLEND_SUBS_VIDEO
            ? PL_OVERLAY_COORDS_SRC_CROP : PL_OVERLAY_COORDS_DST_CROP;
        update_overlays(vo, res, osd_flags,
                        rel, &screenshot_osd, &image, mpi);
    } else {
        // Disable overlays when blend_subs is disabled
        update_overlays(vo, osd, osd_flags, PL_OVERLAY_COORDS_DST_FRAME,
                        &screenshot_osd, &target, mpi);
        image.num_overlays = 0;
    }

    if (!pl_render_image(p->rr, &image, &target, &params)) {
        MP_ERR(vo, "Failed rendering frame!\n");
        goto done;
    }

    args->res = mp_image_alloc(mpfmt, fbo->params.w, fbo->params.h);
    if (!args->res)
        goto done;

    args->res->params.color.primaries = target.color.primaries;
    args->res->params.color.transfer = target.color.transfer;
    args->res->params.repr.levels = target.repr.levels;
    args->res->params.color.hdr = target.color.hdr;
    if (args->scaled)
        args->res->params.p_w = args->res->params.p_h = 1;

    bool ok = pl_tex_download(gpu, pl_tex_transfer_params(
        .tex = fbo,
        .ptr = args->res->planes[0],
        .row_pitch = args->res->stride[0],
    ));

    if (!ok)
        TA_FREEP(&args->res);

    // fall through
done:
    for (size_t i = 0; i < MP_ARRAY_SIZE(screenshot_osd.entries); i++) {
        pl_tex_destroy(gpu, &screenshot_osd.entries[i].tex);
        talloc_free(screenshot_osd.entries[i].parts);
    }
    pl_tex_destroy(gpu, &fbo);
}

static inline void copy_frame_info_to_mp(struct frame_info *pl,
                                         struct mp_frame_perf *mp) {
    static_assert(MP_ARRAY_SIZE(pl->info) == MP_ARRAY_SIZE(mp->perf), "");
    mp_assert(pl->count <= VO_PASS_PERF_MAX);
    mp->count = MPMIN(pl->count, VO_PASS_PERF_MAX);

    for (int i = 0; i < mp->count; ++i) {
        const struct pl_dispatch_info *pass = &pl->info[i];

        static_assert(VO_PERF_SAMPLE_COUNT >= MP_ARRAY_SIZE(pass->samples), "");
        mp_assert(pass->num_samples <= MP_ARRAY_SIZE(pass->samples));

        struct mp_pass_perf *perf = &mp->perf[i];
        perf->count = MPMIN(pass->num_samples, VO_PERF_SAMPLE_COUNT);
        memcpy(perf->samples, pass->samples, perf->count * sizeof(pass->samples[0]));
        perf->last = pass->last;
        perf->peak = pass->peak;
        perf->avg = pass->average;

        strncpy(mp->desc[i], pass->shader->description, sizeof(mp->desc[i]) - 1);
        mp->desc[i][sizeof(mp->desc[i]) - 1] = '\0';
    }
}

static void update_ra_ctx_options(struct vo *vo, struct ra_ctx_opts *ctx_opts)
{
    struct priv *p = vo->priv;
    struct gl_video_opts *gl_opts = p->opts_cache->opts;
    bool border_alpha = (p->next_opts->border_background == BACKGROUND_COLOR &&
                         gl_opts->background_color.a != 255) ||
                         p->next_opts->border_background == BACKGROUND_NONE;
    ctx_opts->want_alpha = (gl_opts->background == BACKGROUND_COLOR &&
                            gl_opts->background_color.a != 255) ||
                            gl_opts->background == BACKGROUND_NONE ||
                            border_alpha;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    if (p->compare_fatal_context)
        return VO_NOTIMPL;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (request == VOCTRL_CHECK_EVENTS)
        compare_poll(vo);
    if (p->compare_fatal_context)
        return VO_NOTIMPL;
#endif

    switch (request) {
    case VOCTRL_SET_PANSCAN:
        resize(vo);
        return VO_TRUE;
    case VOCTRL_PAUSE:
        if (p->is_interpolated)
            vo->want_redraw = true;
        return VO_TRUE;

    case VOCTRL_UPDATE_RENDER_OPTS: {
        update_ra_ctx_options(vo, &p->ra_ctx->opts);
        if (p->ra_ctx->fns->update_render_opts)
            p->ra_ctx->fns->update_render_opts(p->ra_ctx);
        vo->want_redraw = true;

        // Special case for --image-lut which requires a full reset.
        int old_type = p->next_opts->image_lut.type;
        update_options(vo);
        struct user_lut image_lut = p->next_opts->image_lut;
        p->want_reset |= image_lut.opt && ((!image_lut.path && image_lut.opt) ||
                         (image_lut.path && strcmp(image_lut.path, image_lut.opt)) ||
                         (old_type != image_lut.type));

        // Also re-query the auto profile, in case `update_render_options`
        // unloaded a manually specified icc profile in favor of
        // icc-profile-auto
        int events = 0;
        update_auto_profile(p, &events);
        vo_event(vo, events);
        return VO_TRUE;
    }

    case VOCTRL_RESET:
        // Defer until the first new frame (unique ID) actually arrives
        p->want_reset = true;
        return VO_TRUE;

    case VOCTRL_PERFORMANCE_DATA: {
        struct voctrl_performance_data *perf = data;
        copy_frame_info_to_mp(&p->perf_fresh, &perf->fresh);
        copy_frame_info_to_mp(&p->perf_redraw, &perf->redraw);
        return true;
    }

    case VOCTRL_SCREENSHOT:
        video_screenshot(vo, data);
        return true;

    case VOCTRL_EXTERNAL_RESIZE:
        reconfig(vo, NULL);
        return true;

    case VOCTRL_LOAD_HWDEC_API:
        ra_hwdec_ctx_load_fmt(&p->hwdec_ctx, vo->hwdec_devs, data);
        return true;
    }

    int events = 0;
    int r = p->ra_ctx->fns->control(p->ra_ctx, &events, request, data);
    if (events & VO_EVENT_ICC_PROFILE_CHANGED) {
        if (update_auto_profile(p, &events))
            vo->want_redraw = true;
    }
    if (events & VO_EVENT_RESIZE)
        resize(vo);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);

    return r;
}

static void wakeup(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wakeup)
        p->ra_ctx->fns->wakeup(p->ra_ctx);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct priv *p = vo->priv;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->atlas_reuse && (p->atlas_phase == MP_ATLAS_WAIT_ACK ||
                           p->atlas_phase == MP_ATLAS_T1_REDRAW ||
                           p->atlas_phase == MP_ATLAS_WAIT_T2)) {
        until_time_ns = MPMIN(until_time_ns, mp_time_ns() + INT64_C(100000000));
#if HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
        // Property/deadline watch only; GL-owning cleanup waits for the next
        // draw or flip poll so the owner context is never touched blind here.
        atlas_read_wait_poll(vo);
#endif
    }
#endif
    if (p->compare_active)
        until_time_ns = MPMIN(until_time_ns, mp_time_ns() + INT64_C(10000000));
    if (p->ra_ctx && p->ra_ctx->fns->wait_events) {
        p->ra_ctx->fns->wait_events(p->ra_ctx, until_time_ns);
    } else {
        vo_wait_default(vo, until_time_ns);
    }
}

static char *cache_filepath(void *ta_ctx, char *dir, const char *prefix, uint64_t key)
{
    bstr filename = {0};
    bstr_xappend_asprintf(ta_ctx, &filename, "%s_%016" PRIx64, prefix, key);
    return mp_path_join_bstr(ta_ctx, bstr0(dir), filename);
}

static pl_cache_obj cache_load_obj(void *p, uint64_t key)
{
    struct cache *c = p;
    void *ta_ctx = talloc_new(NULL);
    pl_cache_obj obj = {0};

    if (!c->dir)
        goto done;

    char *filepath = cache_filepath(ta_ctx, c->dir, c->name, key);
    if (!filepath)
        goto done;

    if (stat(filepath, &(struct stat){0}))
        goto done;

    int64_t load_start = mp_time_ns();
    struct bstr data = stream_read_file(filepath, ta_ctx, c->global, STREAM_MAX_READ_SIZE);
    int64_t load_end = mp_time_ns();
    MP_DBG(c, "%s: key(%" PRIx64 "), size(%zu), load time(%.3f ms)\n",
           __func__, key, data.len,
           MP_TIME_NS_TO_MS(load_end - load_start));

    obj = (pl_cache_obj){
        .key = key,
        .data = talloc_steal(NULL, data.start),
        .size = data.len,
        .free = talloc_free,
    };

done:
    talloc_free(ta_ctx);
    return obj;
}

static void cache_save_obj(void *p, pl_cache_obj obj)
{
    const struct cache *c = p;
    void *ta_ctx = talloc_new(NULL);

    if (!c->dir)
        goto done;

    char *filepath = cache_filepath(ta_ctx, c->dir, c->name, obj.key);
    if (!filepath)
        goto done;

    if (!obj.data || !obj.size) {
        unlink(filepath);
        goto done;
    }

    // Don't save if already exists
    struct stat st;
    if (!stat(filepath, &st) && st.st_size == obj.size) {
        MP_DBG(c, "%s: key(%"PRIx64"), size(%zu)\n", __func__, obj.key, obj.size);
        goto done;
    }

    int64_t save_start = mp_time_ns();
    mp_save_to_file(filepath, obj.data, obj.size);
    int64_t save_end = mp_time_ns();
    MP_DBG(c, "%s: key(%" PRIx64 "), size(%zu), save time(%.3f ms)\n",
           __func__, obj.key, obj.size,
           MP_TIME_NS_TO_MS(save_end - save_start));

done:
    talloc_free(ta_ctx);
}

static void cache_init(struct vo *vo, struct cache *cache, size_t max_size,
                       const char *dir_opt)
{
    struct priv *p = vo->priv;
    const char *name = cache == &p->shader_cache ? "shader" : "icc";
    const size_t limit = cache == &p->shader_cache ? 128 << 20 : 1536 << 20;

    char *dir;
    if (dir_opt && dir_opt[0]) {
        dir = mp_get_user_path(vo, p->global, dir_opt);
    } else {
        dir = mp_find_user_file(vo, p->global, "cache", "");
    }
    if (!dir || !dir[0])
        return;

    mp_mkdirp(dir);
    *cache = (struct cache){
        .log        = p->log,
        .global     = p->global,
        .dir        = dir,
        .name       = name,
        .size_limit = limit,
        .cache = pl_cache_create(pl_cache_params(
            .log = p->pllog,
            .get = cache_load_obj,
            .set = cache_save_obj,
            .priv = cache
        )),
    };
}

struct file_entry {
    char *filepath;
    size_t size;
    time_t atime;
};

static int compare_atime(const void *a, const void *b)
{
    return (((struct file_entry *)b)->atime - ((struct file_entry *)a)->atime);
}

static void cache_uninit(struct priv *p, struct cache *cache)
{
    if (!cache->cache)
        return;

    void *ta_ctx = talloc_new(NULL);
    struct file_entry *files = NULL;
    size_t num_files = 0;
    mp_assert(cache->dir);
    mp_assert(cache->name);

    DIR *d = opendir(cache->dir);
    if (!d)
        goto done;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        char *filepath = mp_path_join(ta_ctx, cache->dir, dir->d_name);
        if (!filepath)
            continue;
        struct stat filestat;
        if (stat(filepath, &filestat))
            continue;
        if (!S_ISREG(filestat.st_mode))
            continue;
        bstr fname = bstr0(dir->d_name);
        if (!bstr_eatstart0(&fname, cache->name))
            continue;
        if (!bstr_eatstart0(&fname, "_"))
            continue;
        if (fname.len != 16) // %016x
            continue;
        MP_TARRAY_APPEND(ta_ctx, files, num_files,
                         (struct file_entry){
                             .filepath = filepath,
                             .size     = filestat.st_size,
                             .atime    = filestat.st_atime,
                         });
    }
    closedir(d);

    if (!num_files)
        goto done;

    qsort(files, num_files, sizeof(struct file_entry), compare_atime);

    time_t t = time(NULL);
    size_t cache_size = 0;
    size_t cache_limit = cache->size_limit ? cache->size_limit : SIZE_MAX;
    for (int i = 0; i < num_files; i++) {
        // Remove files that exceed the size limit but are older than one day.
        // This allows for temporary maintaining a larger cache size while
        // adjusting the configuration. The cache will be cleared the next day
        // for unused entries. We don't need to be overly aggressive with cache
        // cleaning; in most cases, it will not grow much, and in others, it may
        // actually be useful to cache more.
        cache_size += files[i].size;
        double rel_use = difftime(t, files[i].atime);
        if (cache_size > cache_limit && rel_use > 60 * 60 * 24) {
            MP_VERBOSE(p, "Removing %s | size: %9zu bytes | last used: %9d seconds ago\n",
                       files[i].filepath, files[i].size, (int)rel_use);
            unlink(files[i].filepath);
        }
    }

done:
    talloc_free(ta_ctx);
    pl_cache_destroy(&cache->cache);
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_requested && p->ra_ctx &&
        !mks_android_yuv_compare_owner_current(p->ra_ctx))
        compare_fail_transaction(vo, "pre-uninit-owner-mismatch");
#endif
    if (p->compare_fatal_context) {
        // No queue unmap, hwdec, renderer, texture or context destructor is
        // safe under a foreign context. This explicit diagnostic terminal
        // state requires host force-stop; resources remain until process exit.
        MP_ERR(vo, "MKS-YUV-COMPARE event=fatal-cleanup-deferred requires_force_stop=1\n");
        return;
    }
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_READ)
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->table_cache || p->table_anchor)
        fusion_table_detach(vo);
#endif
    if ((p->atlas_read && !pl_opengl_uv_atlas_read_cancel(&p->atlas_read)) ||
        (p->atlas_cache && !pl_opengl_uv_atlas_cache_destroy(&p->atlas_cache))) {
        p->compare_terminal = p->compare_fatal_context = true;
        MP_ERR(vo, "MKS-UV-ATLAS-READ event=cleanup-deferred requires_force_stop=1\n");
        return;
    }
#endif
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
    if (p->atlas_anchor && !pl_opengl_uv_atlas_anchor_cancel(&p->atlas_anchor)) {
        p->compare_terminal = p->compare_fatal_context = true;
        MP_ERR(vo, "MKS-YUV-ATLAS event=cleanup-deferred id=%"PRIu64" requires_force_stop=1\n", p->compare_id);
        return;
    }
#endif
    mks_thermal_report(&p->thermal, true);
    gpu_timing_report(vo, true);
    pl_queue_destroy(&p->queue); // destroy this first
    for (int i = 0; i < MP_ARRAY_SIZE(p->osd_state.entries); i++)
        pl_tex_destroy(p->gpu, &p->osd_state.entries[i].tex);
    for (int i = 0; i < p->num_sub_tex; i++)
        pl_tex_destroy(p->gpu, &p->sub_tex[i]);
    for (int i = 0; i < p->num_user_hooks; i++)
        pl_mpv_user_shader_destroy(&p->user_hooks[i].hook);

    if (vo->hwdec_devs) {
        ra_hwdec_mapper_free(&p->hwdec_mapper);
        ra_hwdec_ctx_uninit(&p->hwdec_ctx);
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
    }

    mp_assert(p->num_dr_buffers == 0);
    mp_mutex_destroy(&p->dr_lock);

    cache_uninit(p, &p->shader_cache);
    cache_uninit(p, &p->icc_cache);

    pl_lut_free(&p->next_opts->image_lut.lut);
    pl_lut_free(&p->next_opts->lut.lut);
    pl_lut_free(&p->next_opts->target_lut.lut);

    pl_icc_close(&p->icc_profile);
#if HAVE_GL && defined(PL_HAVE_OPENGL) && HAVE_EGL_ANDROID && defined(PL_HAVE_MKS_YUV_COMPARE)
    if (p->compare_started) {
        if (mks_android_yuv_compare_owner_current(p->ra_ctx)) {
            if (p->fusion_target)
                pl_opengl_yuv_compare_cancel(p->gpu, p->fusion_target);
            mks_android_yuv_compare_cancel(p->ra_ctx);
        } else {
            // Do not delete query/SSBO/FBO names in an unrelated context.
            // This default-off diagnostic abandons its borrowed wrapper until
            // original context/process destruction rather than risking another.
            MP_ERR(vo, "MKS-YUV-COMPARE event=cleanup-deferred id=%"PRIu64" reason=foreign-context-or-generation\n",
                   p->compare_id);
            p->fusion_target = NULL;
        }
    }
#endif
    pl_tex_destroy(p->gpu, &p->fusion_target);
    pl_renderer_destroy(&p->rr);

    for (int i = 0; i < VO_PASS_PERF_MAX; ++i) {
        pl_shader_info_deref(&p->perf_fresh.info[i].shader);
        pl_shader_info_deref(&p->perf_redraw.info[i].shader);
    }

    pl_options_free(&p->pars);

    p->ra_ctx = NULL;
    p->pllog = NULL;
    p->gpu = NULL;
    p->sw = NULL;
    gpu_ctx_destroy(&p->context);
}

static void load_hwdec_api(void *ctx, struct hwdec_imgfmt_request *params)
{
    vo_control(ctx, VOCTRL_LOAD_HWDEC_API, params);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mks_thermal_init(&p->thermal, vo->log, "vo", p);
#if HAVE_ANDROID
    char compare[PROP_VALUE_MAX] = {0}, compare_pts[PROP_VALUE_MAX] = {0};
    char compare_repeat[PROP_VALUE_MAX] = {0};
    char compare_tap[PROP_VALUE_MAX] = {0};
    char compare_invariant[PROP_VALUE_MAX] = {0};
    char compare_oracle[PROP_VALUE_MAX] = {0}, compare_oracle_a[PROP_VALUE_MAX] = {0};
    char compare_probe[PROP_VALUE_MAX] = {0};
    char compare_single[PROP_VALUE_MAX] = {0};
    char compare_quad[PROP_VALUE_MAX] = {0}, compare_reverse[PROP_VALUE_MAX] = {0};
    char compare_atlas[PROP_VALUE_MAX] = {0};
    char atlas_reuse[PROP_VALUE_MAX] = {0}, atlas_second[PROP_VALUE_MAX] = {0};
    char atlas_nonce[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.mks.yuv_compare", compare);
    __system_property_get("debug.mks.yuv_compare_pts", compare_pts);
    __system_property_get("debug.mks.yuv_compare_repeat", compare_repeat);
    __system_property_get("debug.mks.yuv_tap", compare_tap);
    __system_property_get("debug.mks.yuv_inv", compare_invariant);
    __system_property_get("debug.mks.yuv_oracle", compare_oracle);
    __system_property_get("debug.mks.yuv_oracle_a", compare_oracle_a);
    __system_property_get("debug.mks.yuv_probe", compare_probe);
    __system_property_get("debug.mks.yuv_single", compare_single);
    __system_property_get("debug.mks.yuv_quad", compare_quad);
    __system_property_get("debug.mks.yuv_rev", compare_reverse);
    __system_property_get("debug.mks.yuv_atlas", compare_atlas);
    __system_property_get("debug.mks.atlas_reuse", atlas_reuse);
    __system_property_get("debug.mks.atlas_pts2", atlas_second);
    __system_property_get("debug.mks.atlas_nonce", atlas_nonce);
    char *compare_end = NULL;
    p->compare_target_pts = strtod(compare_pts, &compare_end);
    // Seed each VO instance before its local transaction increment. O_EXCL
    // remains the final collision guard across PID reuse and old artifacts.
    p->compare_id = (uint64_t)mp_time_ns();
    p->compare_requested = !strcmp(compare, "1");
    bool repeat_config_valid = mp_yuv_compare_repeat_config(compare_repeat,
                                                           &p->compare_repeat);
#if !defined(PL_HAVE_MKS_YUV_COMPARE_REPEAT)
    repeat_config_valid = repeat_config_valid && !p->compare_repeat;
#endif
    bool tap_config_valid = mp_yuv_compare_tap_config(compare_tap, p->compare_repeat,
                                                     &p->compare_sample_tap);
#if !defined(PL_HAVE_MKS_YUV_COMPARE_SAMPLE_TAP)
    tap_config_valid = tap_config_valid && !p->compare_sample_tap;
#endif
    bool invariant_config_valid = mp_yuv_compare_invariant_config(compare_invariant,
        p->compare_repeat, p->compare_sample_tap, &p->compare_invariant_b);
#if !defined(PL_HAVE_MKS_YUV_COMPARE_INVARIANT_B)
    invariant_config_valid = invariant_config_valid && !p->compare_invariant_b;
#endif
    bool oracle_config_valid = mp_yuv_compare_oracle_config(compare_oracle,
        compare_oracle_a, p->compare_repeat, p->compare_sample_tap,
        p->compare_invariant_b, &p->compare_uv_oracle, &p->compare_uv_oracle_use_a);
#if !defined(PL_HAVE_MKS_YUV_COMPARE_UV_ORACLE)
    oracle_config_valid = oracle_config_valid && !p->compare_uv_oracle;
#endif
    bool probe_config_valid = mp_yuv_compare_probe_config(compare_probe,
        p->compare_uv_oracle, &p->compare_uv_probe);
#if !defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_PROBE)
    probe_config_valid = probe_config_valid && !p->compare_uv_probe;
#endif
    bool single_config_valid = mp_yuv_compare_single_config(compare_single,
        p->compare_uv_oracle, p->compare_uv_probe, p->compare_repeat,
        p->compare_sample_tap, p->compare_invariant_b, &p->compare_uv_single);
#if !defined(PL_HAVE_MKS_YUV_COMPARE_ORACLE_SINGLE_PAIR)
    single_config_valid = single_config_valid && !p->compare_uv_single;
#endif
    bool quad_config_valid = mp_yuv_compare_quad_config(compare_quad, compare_reverse,
        p->compare_uv_single, p->compare_uv_oracle_use_a,
        &p->compare_quad_order, &p->compare_quad_reverse);
#if !defined(PL_HAVE_MKS_YUV_COMPARE_QUAD_ORDER)
    quad_config_valid = quad_config_valid && !p->compare_quad_order;
#endif
    bool atlas_config_valid = mp_yuv_atlas_config(compare_atlas,
        p->compare_repeat || p->compare_sample_tap || p->compare_invariant_b ||
        p->compare_uv_oracle || p->compare_uv_oracle_use_a || p->compare_uv_probe ||
        p->compare_uv_single || p->compare_quad_order || p->compare_quad_reverse,
        &p->compare_atlas);
#if !defined(PL_HAVE_MKS_UV_ATLAS_ANCHOR)
    atlas_config_valid = atlas_config_valid && !p->compare_atlas;
#endif
    bool atlas_requested = compare_atlas[0] && strcmp(compare_atlas, "0");
    char *atlas_end = NULL;
    double second_pts = strtod(atlas_second, &atlas_end);
    bool reuse_config_valid = mp_yuv_atlas_read_config(atlas_reuse, p->compare_atlas,
        p->compare_target_pts, second_pts, atlas_nonce, &p->atlas_reuse);
    reuse_config_valid = reuse_config_valid && (!p->atlas_reuse ||
        (atlas_second[0] && atlas_end && !*atlas_end));
#if defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (reuse_config_valid && p->atlas_reuse) {
        p->atlas_second_pts = second_pts;
        memcpy(p->atlas_nonce, atlas_nonce, sizeof(p->atlas_nonce));
        p->atlas_phase = MP_ATLAS_SEED;
    }
#else
    reuse_config_valid = reuse_config_valid && !p->atlas_reuse;
#endif
    bool reuse_requested = atlas_reuse[0] && strcmp(atlas_reuse, "0");
    p->compare_requested |= atlas_requested || reuse_requested;
    p->compare_enabled = p->compare_requested && compare_pts[0] &&
        compare_end && !*compare_end && isfinite(p->compare_target_pts) &&
        p->compare_target_pts >= 0 && repeat_config_valid && tap_config_valid &&
        invariant_config_valid && oracle_config_valid && probe_config_valid && single_config_valid && quad_config_valid &&
        atlas_config_valid && reuse_config_valid &&
        (!(atlas_requested || reuse_requested) || !strcmp(compare, "1"));
    if (p->compare_requested && !p->compare_enabled)
        MP_ERR(vo, "%s event=config-invalid forcing_original_rgb=1\n",
               reuse_requested ? "MKS-UV-ATLAS-READ" : atlas_requested ? "MKS-YUV-ATLAS" : "MKS-YUV-COMPARE");
    char frame_trace[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.mks.yuv_frame_trace", frame_trace);
    p->fusion_frame_trace = !strcmp(frame_trace, "1");
    char fault[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.mks.yuv_fail_commit", fault);
    if (!strcmp(fault, "1"))
        p->fusion_fault = 1;
    memset(fault, 0, sizeof(fault));
    __system_property_get("debug.mks.yuv_fail_gl", fault);
    if (!strcmp(fault, "1"))
        p->fusion_fault = 2;
    memset(fault, 0, sizeof(fault));
    __system_property_get("debug.mks.yuv_fusion_table", fault);
    // Production default (user ruling 2026-11-02): output fusion runs
    // UNTABLED by default — the measured table gain over untabled fusion
    // (~14 GPU-busy points) buys bit-exact UV coordinates, but the
    // untabled difference (0.12% last-bit components) was never shown to be
    // perceptually or safety-worse than the original path. The table stays
    // a fully validated opt-in: debug.mks.yuv_fusion_table=1.
    p->fusion_table_enabled = !strcmp(fault, "1");
    p->fusion_table_was_enabled = p->fusion_table_enabled;
    char timing[PROP_VALUE_MAX] = {0};
    p->gpu_timing = __system_property_get("debug.mks.gpu_timing", timing) > 0 &&
                    strcmp(timing, "render") == 0;
#endif
    p->opts_cache = m_config_cache_alloc(p, vo->global, &gl_video_conf);
    p->next_opts_cache = m_config_cache_alloc(p, vo->global, &gl_next_conf);
    p->next_opts = p->next_opts_cache->opts;
    p->video_eq = mp_csp_equalizer_create(p, vo->global);
    p->global = vo->global;
    p->log = vo->log;

    struct gl_video_opts *gl_opts = p->opts_cache->opts;
    struct ra_ctx_opts *ctx_opts = mp_get_config_group(vo, vo->global, &ra_ctx_conf);
    update_ra_ctx_options(vo, ctx_opts);
    p->context = gpu_ctx_create(vo, ctx_opts);
    talloc_free(ctx_opts);
    if (!p->context)
        goto err_out;
    // For the time being
    p->ra_ctx = p->context->ra_ctx;
    p->pllog = p->context->pllog;
    p->gpu = p->context->gpu;
    p->sw = p->context->swapchain;
    p->hwdec_ctx = (struct ra_hwdec_ctx) {
        .log = p->log,
        .global = p->global,
        .ra_ctx = p->ra_ctx,
    };

    vo->hwdec_devs = hwdec_devices_create();
    hwdec_devices_set_loader(vo->hwdec_devs, load_hwdec_api, vo);
    ra_hwdec_ctx_init(&p->hwdec_ctx, vo->hwdec_devs, gl_opts->hwdec_interop, false);
    mp_mutex_init(&p->dr_lock);

    if (gl_opts->shader_cache)
        cache_init(vo, &p->shader_cache, 10 << 20, gl_opts->shader_cache_dir);
    if (gl_opts->icc_opts->cache)
        cache_init(vo, &p->icc_cache, 20 << 20, gl_opts->icc_opts->cache_dir);

    pl_gpu_set_cache(p->gpu, p->shader_cache.cache);
    p->rr = pl_renderer_create(p->pllog, p->gpu);
#if HAVE_GL && defined(PL_HAVE_OPENGL) && defined(PL_HAVE_MKS_UV_ATLAS_READ)
    if (p->atlas_reuse) {
        p->atlas_renderer_id = (uint64_t)(uintptr_t)p->rr;
        p->atlas_renderer_generation = (uint64_t)mp_time_ns();
    }
#endif
    p->queue = pl_queue_create(p->gpu);
    p->osd_fmt[SUBBITMAP_LIBASS] = pl_find_named_fmt(p->gpu, "r8");
    p->osd_fmt[SUBBITMAP_BGRA] = pl_find_named_fmt(p->gpu, "bgra8");
    p->osd_sync = 1;

    p->pars = pl_options_alloc(p->pllog);
    update_render_options(vo);
    return 0;

err_out:
    uninit(vo);
    return -1;
}

static const struct pl_filter_config *map_scaler(struct priv *p,
                                                 enum scaler_unit unit)
{
    const struct pl_filter_preset fixed_scalers[] = {
        { "bilinear",       &pl_filter_bilinear },
        { "bicubic_fast",   &pl_filter_bicubic },
        { "nearest",        &pl_filter_nearest },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset fixed_frame_mixers[] = {
        { "linear",         &pl_filter_bilinear },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset *fixed_presets =
        unit == SCALER_TSCALE ? fixed_frame_mixers : fixed_scalers;

    const struct gl_video_opts *opts = p->opts_cache->opts;
    const struct scaler_config *cfg = &opts->scaler[unit];
    struct scaler_config tmp;
    if (cfg->kernel.function == SCALER_INHERIT) {
        tmp = *cfg;
        scaler_conf_merge(&tmp, &opts->scaler[SCALER_SCALE], unit);
        cfg = &tmp;
    }

    const char *kernel_name = m_opt_choice_str(cfg->kernel.functions,
                                               cfg->kernel.function);

    for (int i = 0; fixed_presets[i].name; i++) {
        if (strcmp(kernel_name, fixed_presets[i].name) == 0)
            return fixed_presets[i].filter;
    }

    // Attempt loading filter preset first, fall back to raw filter function
    struct scaler_params *par = &p->scalers[unit];
    const struct pl_filter_preset *preset;
    const struct pl_filter_function_preset *fpreset;
    if ((preset = pl_find_filter_preset(kernel_name))) {
        par->config = *preset->filter;
    } else if ((fpreset = pl_find_filter_function_preset(kernel_name))) {
        par->config = (struct pl_filter_config) {
            .kernel = fpreset->function,
            .params[0] = fpreset->function->params[0],
            .params[1] = fpreset->function->params[1],
        };
    } else {
        MP_ERR(p, "Failed mapping filter function '%s', no libplacebo analog?\n",
               kernel_name);
        return &pl_filter_bilinear;
    }

    const struct pl_filter_function_preset *wpreset;
    if ((wpreset = pl_find_filter_function_preset(
             m_opt_choice_str(cfg->window.functions, cfg->window.function)))) {
        par->config.window = wpreset->function;
        par->config.wparams[0] = wpreset->function->params[0];
        par->config.wparams[1] = wpreset->function->params[1];
    }

    for (int i = 0; i < 2; i++) {
        if (!isnan(cfg->kernel.params[i]))
            par->config.params[i] = cfg->kernel.params[i];
        if (!isnan(cfg->window.params[i]))
            par->config.wparams[i] = cfg->window.params[i];
    }

    par->config.clamp = cfg->clamp;
    if (cfg->antiring > 0.0)
        par->config.antiring = cfg->antiring;
    if (cfg->kernel.blur > 0.0)
        par->config.blur = cfg->kernel.blur;
    if (cfg->kernel.taper > 0.0)
        par->config.taper = cfg->kernel.taper;
    if (cfg->radius > 0.0) {
        if (par->config.kernel->resizable) {
            par->config.radius = cfg->radius;
        } else {
            MP_WARN(p, "Filter radius specified but filter '%s' is not "
                    "resizable, ignoring\n", kernel_name);
        }
    }

    return &par->config;
}

static const struct pl_hook *load_hook(struct priv *p, const char *path)
{
    if (!path || !path[0])
        return NULL;

    for (int i = 0; i < p->num_user_hooks; i++) {
        if (strcmp(p->user_hooks[i].path, path) == 0)
            return p->user_hooks[i].hook;
    }

    char *fname = mp_get_user_path(NULL, p->global, path);
    bstr shader = stream_read_file(fname, p, p->global, 1000000000); // 1GB
    talloc_free(fname);

    const struct pl_hook *hook = NULL;
    if (shader.len)
        hook = pl_mpv_user_shader_parse(p->gpu, shader.start, shader.len);

    MP_TARRAY_APPEND(p, p->user_hooks, p->num_user_hooks, (struct user_hook) {
        .path = talloc_strdup(p, path),
        .hook = hook,
    });

    return hook;
}

static void update_icc_opts(struct priv *p, const struct mp_icc_opts *opts)
{
    if (!opts)
        return;

    if (!opts->profile_auto && !p->icc_path) {
        // Un-set any auto-loaded profiles if icc-profile-auto was disabled
        update_icc(p, (bstr) {0});
    }

    int s_r = 0, s_g = 0, s_b = 0;
    gl_parse_3dlut_size(opts->size_str, &s_r, &s_g, &s_b);
    p->icc_params = pl_icc_default_params;
    p->icc_params.intent = opts->intent;
    p->icc_params.size_r = s_r;
    p->icc_params.size_g = s_g;
    p->icc_params.size_b = s_b;
    p->icc_params.cache = p->icc_cache.cache;

    if (!opts->profile || !opts->profile[0]) {
        // No profile enabled, un-load any existing profiles
        update_icc(p, (bstr) {0});
        TA_FREEP(&p->icc_path);
        return;
    }

    if (p->icc_path && strcmp(opts->profile, p->icc_path) == 0)
        return; // ICC profile hasn't changed

    char *fname = mp_get_user_path(NULL, p->global, opts->profile);
    MP_VERBOSE(p, "Opening ICC profile '%s'\n", fname);
    struct bstr icc = stream_read_file(fname, p, p->global, 100000000); // 100 MB
    talloc_free(fname);
    update_icc(p, icc);

    // Update cached path
    talloc_replace(p, p->icc_path, opts->profile);
}

static void update_lut(struct priv *p, struct user_lut *lut)
{
    if (!lut->opt || !lut->opt[0]) {
        pl_lut_free(&lut->lut);
        TA_FREEP(&lut->path);
        return;
    }

    if (lut->path && strcmp(lut->path, lut->opt) == 0)
        return; // no change

    // Update cached path
    pl_lut_free(&lut->lut);
    talloc_replace(p, lut->path, lut->opt);

    // Load LUT file
    char *fname = mp_get_user_path(NULL, p->global, lut->path);
    MP_VERBOSE(p, "Loading custom LUT '%s'\n", fname);
    const int lut_max_size = 1536 << 20; // 1.5 GiB, matches lut cache limit
    struct bstr lutdata = stream_read_file(fname, NULL, p->global, lut_max_size);
    if (!lutdata.len) {
        MP_ERR(p, "Failed to read LUT data from %s, make sure it's a valid file "
                  "and smaller or equal to %d bytes\n", fname, lut_max_size);
    } else {
        lut->lut = pl_lut_parse_cube(p->pllog, lutdata.start, lutdata.len);
    }
    talloc_free(fname);
    talloc_free(lutdata.start);
}

static void update_hook_opts_dynamic(struct priv *p, const struct pl_hook *hook,
                                     const struct mp_image *mpi)
{
    float chroma_offset_x, chroma_offset_y;
    pl_chroma_location_offset(mpi->params.chroma_location,
                              &chroma_offset_x, &chroma_offset_y);
    const struct {
        const char *name;
        double value;
    } opts[] = {
        {             "PTS", mpi->pts                           },
        { "chroma_offset_x", chroma_offset_x                    },
        { "chroma_offset_y", chroma_offset_y                    },
        {        "min_luma", mpi->params.color.hdr.min_luma     },
        {        "max_luma", mpi->params.color.hdr.max_luma     },
        {         "max_cll", mpi->params.color.hdr.max_cll      },
        {        "max_fall", mpi->params.color.hdr.max_fall     },
        {     "scene_max_r", mpi->params.color.hdr.scene_max[0] },
        {     "scene_max_g", mpi->params.color.hdr.scene_max[1] },
        {     "scene_max_b", mpi->params.color.hdr.scene_max[2] },
        {       "scene_avg", mpi->params.color.hdr.scene_avg    },
        {        "max_pq_y", mpi->params.color.hdr.max_pq_y     },
        {        "avg_pq_y", mpi->params.color.hdr.avg_pq_y     },
    };

    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *hp = &hook->parameters[i];
        for (int n = 0; n < MP_ARRAY_SIZE(opts); n++) {
            if (strcmp(hp->name, opts[n].name) != 0)
                continue;

            switch (hp->type) {
                case PL_VAR_FLOAT: hp->data->f = opts[n].value; break;
                case PL_VAR_SINT:  hp->data->i = lrint(opts[n].value); break;
                case PL_VAR_UINT:  hp->data->u = lrint(opts[n].value); break;
            }
        }
    }
}

static void update_hook_opts(struct priv *p, char **opts, const char *shaderpath,
                             const struct pl_hook *hook)
{
    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *hp = &hook->parameters[i];
        memcpy(hp->data, &hp->initial, sizeof(*hp->data));
    }

    if (!opts)
        return;

    const char *basename = mp_basename(shaderpath);
    struct bstr shadername;
    if (!mp_splitext(basename, &shadername))
        shadername = bstr0(basename);

    for (int n = 0; opts[n * 2]; n++) {
        struct bstr k = bstr0(opts[n * 2 + 0]);
        struct bstr v = bstr0(opts[n * 2 + 1]);
        int pos;
        if ((pos = bstrchr(k, '/')) >= 0) {
            if (!bstr_equals(bstr_splice(k, 0, pos), shadername))
                continue;
            k = bstr_cut(k, pos + 1);
        }

        for (int i = 0; i < hook->num_parameters; i++) {
            const struct pl_hook_par *hp = &hook->parameters[i];
            if (!bstr_equals0(k, hp->name) != 0)
                continue;

            m_option_t opt = {
                .name = hp->name,
            };

            if (hp->names) {
                for (int j = hp->minimum.i; j <= hp->maximum.i; j++) {
                    if (bstr_equals0(v, hp->names[j])) {
                        hp->data->i = j;
                        goto next_hook;
                    }
                }
            }

            switch (hp->type) {
            case PL_VAR_FLOAT:
                opt.type = &m_option_type_float;
                opt.min = hp->minimum.f;
                opt.max = hp->maximum.f;
                break;
            case PL_VAR_SINT:
                opt.type = &m_option_type_int;
                opt.min = hp->minimum.i;
                opt.max = hp->maximum.i;
                break;
            case PL_VAR_UINT:
                opt.type = &m_option_type_int;
                opt.min = MPMIN(hp->minimum.u, INT_MAX);
                opt.max = MPMIN(hp->maximum.u, INT_MAX);
                break;
            }

            if (!opt.type)
                goto next_hook;

            opt.type->parse(p->log, &opt, k, v, hp->data);
            goto next_hook;
        }

    next_hook:;
    }
}

static void update_render_options(struct vo *vo)
{
    struct priv *p = vo->priv;
    pl_options pars = p->pars;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    pars->params.background_color[0] = opts->background_color.r / 255.0;
    pars->params.background_color[1] = opts->background_color.g / 255.0;
    pars->params.background_color[2] = opts->background_color.b / 255.0;
    pars->params.background_transparency = 1 - opts->background_color.a / 255.0;
    pars->params.skip_anti_aliasing = !opts->correct_downscaling;
    pars->params.disable_linear_scaling = !opts->linear_downscaling && !opts->linear_upscaling;
    pars->params.disable_fbos = opts->dumb_mode == 1;

#if PL_API_VER >= 346
    static const int map_background_types[] = {
        [BACKGROUND_NONE]  = PL_CLEAR_SKIP,
        [BACKGROUND_COLOR] = PL_CLEAR_COLOR,
        [BACKGROUND_TILES] = PL_CLEAR_TILES,
#if PL_API_VER >= 355
        [BACKGROUND_BLUR]  = PL_CLEAR_BLUR,
#endif
    };
    pars->params.background = map_background_types[opts->background];
    pars->params.border = map_background_types[p->next_opts->border_background];
#if PL_API_VER >= 355
    pars->params.blur_radius = p->next_opts->background_blur_radius;
#endif
#else
    pars->params.blend_against_tiles = opts->background == BACKGROUND_TILES;
#endif
    pars->params.tile_size = opts->background_tile_size * 2;
    for (int i = 0; i < 2; ++i) {
        pars->params.tile_colors[i][0] = opts->background_tile_color[i].r / 255.0f;
        pars->params.tile_colors[i][1] = opts->background_tile_color[i].g / 255.0f;
        pars->params.tile_colors[i][2] = opts->background_tile_color[i].b / 255.0f;
    }

    pars->params.corner_rounding = p->next_opts->corner_rounding;
    pars->params.correct_subpixel_offsets = !opts->scaler_resizes_only;

    // Map scaler options as best we can
    pars->params.upscaler = map_scaler(p, SCALER_SCALE);
    pars->params.downscaler = map_scaler(p, SCALER_DSCALE);
    pars->params.plane_upscaler = map_scaler(p, SCALER_CSCALE);
    pars->params.frame_mixer = opts->interpolation ? map_scaler(p, SCALER_TSCALE) : NULL;

    // Request as many frames as required from the decoder, depending on the
    // speed VPS/FPS ratio libplacebo may need more frames. Request frames up to
    // ratio of 1/2, but only if anti aliasing is enabled.
    int req_frames = 2;
    if (pars->params.frame_mixer) {
        req_frames += ceilf(pars->params.frame_mixer->kernel->radius) *
                      (pars->params.skip_anti_aliasing ? 1 : 2);
    }
    vo_set_queue_params(vo, 0, MPMIN(VO_MAX_REQ_FRAMES, req_frames));

    pars->params.deband_params = opts->deband ? &pars->deband_params : NULL;
    pars->deband_params.iterations = opts->deband_opts->iterations;
    pars->deband_params.radius = opts->deband_opts->range;
    pars->deband_params.threshold = opts->deband_opts->threshold / 16.384;
    pars->deband_params.grain = opts->deband_opts->grain / 8.192;

    pars->params.sigmoid_params = opts->sigmoid_upscaling ? &pars->sigmoid_params : NULL;
    pars->sigmoid_params.center = opts->sigmoid_center;
    pars->sigmoid_params.slope = opts->sigmoid_slope;

    pars->params.peak_detect_params = opts->tone_map.compute_peak >= 0 ? &pars->peak_detect_params : NULL;
    pars->peak_detect_params.smoothing_period = opts->tone_map.decay_rate;
    pars->peak_detect_params.scene_threshold_low = opts->tone_map.scene_threshold_low;
    pars->peak_detect_params.scene_threshold_high = opts->tone_map.scene_threshold_high;
    pars->peak_detect_params.percentile = opts->tone_map.peak_percentile;
    pars->peak_detect_params.allow_delayed = p->next_opts->delayed_peak;

    const struct pl_tone_map_function * const tone_map_funs[] = {
        [TONE_MAPPING_AUTO]     = &pl_tone_map_auto,
        [TONE_MAPPING_CLIP]     = &pl_tone_map_clip,
        [TONE_MAPPING_MOBIUS]   = &pl_tone_map_mobius,
        [TONE_MAPPING_REINHARD] = &pl_tone_map_reinhard,
        [TONE_MAPPING_HABLE]    = &pl_tone_map_hable,
        [TONE_MAPPING_GAMMA]    = &pl_tone_map_gamma,
        [TONE_MAPPING_LINEAR]   = &pl_tone_map_linear,
        [TONE_MAPPING_SPLINE]   = &pl_tone_map_spline,
        [TONE_MAPPING_BT_2390]  = &pl_tone_map_bt2390,
        [TONE_MAPPING_BT_2446A] = &pl_tone_map_bt2446a,
        [TONE_MAPPING_ST2094_40] = &pl_tone_map_st2094_40,
        [TONE_MAPPING_ST2094_10] = &pl_tone_map_st2094_10,
    };

    const struct pl_gamut_map_function * const gamut_modes[] = {
        [GAMUT_AUTO]            = pl_color_map_default_params.gamut_mapping,
        [GAMUT_CLIP]            = &pl_gamut_map_clip,
        [GAMUT_PERCEPTUAL]      = &pl_gamut_map_perceptual,
        [GAMUT_RELATIVE]        = &pl_gamut_map_relative,
        [GAMUT_SATURATION]      = &pl_gamut_map_saturation,
        [GAMUT_ABSOLUTE]        = &pl_gamut_map_absolute,
        [GAMUT_DESATURATE]      = &pl_gamut_map_desaturate,
        [GAMUT_DARKEN]          = &pl_gamut_map_darken,
        [GAMUT_WARN]            = &pl_gamut_map_highlight,
        [GAMUT_LINEAR]          = &pl_gamut_map_linear,
    };

    pars->color_map_params.tone_mapping_function = tone_map_funs[opts->tone_map.curve];
AV_NOWARN_DEPRECATED(
    pars->color_map_params.tone_mapping_param = opts->tone_map.curve_param;
    if (isnan(pars->color_map_params.tone_mapping_param)) // vo_gpu compatibility
        pars->color_map_params.tone_mapping_param = 0.0;
)
    pars->color_map_params.inverse_tone_mapping = opts->tone_map.inverse;
    pars->color_map_params.contrast_recovery = opts->tone_map.contrast_recovery;
    pars->color_map_params.visualize_lut = opts->tone_map.visualize;
    pars->color_map_params.contrast_smoothness = opts->tone_map.contrast_smoothness;
    pars->color_map_params.gamut_mapping = gamut_modes[opts->tone_map.gamut_mode];

    pars->params.dither_params = NULL;
    pars->params.error_diffusion = NULL;

    switch (opts->dither_algo) {
    case DITHER_ERROR_DIFFUSION:
        pars->params.error_diffusion = pl_find_error_diffusion_kernel(opts->error_diffusion);
        if (!pars->params.error_diffusion) {
            MP_WARN(p, "Could not find error diffusion kernel '%s', falling "
                    "back to fruit.\n", opts->error_diffusion);
        }
        MP_FALLTHROUGH;
    case DITHER_ORDERED:
    case DITHER_FRUIT:
        pars->params.dither_params = &pars->dither_params;
        pars->dither_params.method = opts->dither_algo == DITHER_ORDERED
                                ? PL_DITHER_ORDERED_FIXED
                                : PL_DITHER_BLUE_NOISE;
        pars->dither_params.lut_size = opts->dither_size;
        pars->dither_params.temporal = opts->temporal_dither;
        break;
    }

    if (opts->dither_depth < 0) {
        pars->params.dither_params = NULL;
        pars->params.error_diffusion = NULL;
    }

    update_icc_opts(p, opts->icc_opts);

    pars->params.num_hooks = 0;
    const struct pl_hook *hook;
    for (int i = 0; opts->user_shaders && opts->user_shaders[i]; i++) {
        if ((hook = load_hook(p, opts->user_shaders[i]))) {
            MP_TARRAY_APPEND(p, p->hooks, pars->params.num_hooks, hook);
            update_hook_opts(p, opts->user_shader_opts, opts->user_shaders[i], hook);
        }
    }

    pars->params.hooks = p->hooks;

    MP_DBG(p, "Render options updated, resetting render state.\n");
    p->want_reset = true;
}

const struct vo_driver video_out_gpu_next = {
    .description = "Video output based on libplacebo",
    .name = "gpu-next",
    .caps = VO_CAP_ROTATE90 |
            VO_CAP_FILM_GRAIN |
            VO_CAP_VFLIP |
            0x0,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_image_ts = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
