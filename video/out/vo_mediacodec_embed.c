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

#include <errno.h>
#include <string.h>
#include <time.h>

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "common/common.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/hwdec.h"

struct mediacodec_embed_opts {
    int render_mode;
};

#define OPT_BASE_STRUCT struct mediacodec_embed_opts
static const struct m_sub_options mediacodec_embed_conf = {
    .prefix = "mediacodec-embed",
    .opts = (const struct m_option[]) {
        {"render-mode", OPT_CHOICE(render_mode, {"boolean", 0}, {"timed", 1})},
        {0}
    },
    .size = sizeof(struct mediacodec_embed_opts),
    .defaults = &(const struct mediacodec_embed_opts){ .render_mode = 0 },
};
#undef OPT_BASE_STRUCT

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;
    struct m_config_cache *opts_cache;
    struct mediacodec_embed_opts *opts;
};

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    mp_assert(vo->opts->WinID != 0 && vo->opts->WinID != -1);
    hwctx->surface = (void *)(intptr_t)(vo->opts->WinID);

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->opts_cache = m_config_cache_alloc(vo, vo->global, &mediacodec_embed_conf);
    p->opts = p->opts_cache->opts;
    MP_VERBOSE(vo, "MediaCodec Surface render mode: %s\n",
               p->opts->render_mode ? "timed" : "boolean");
    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_embed",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->next_image)
        return;

    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
    if (m_config_cache_update(p->opts_cache))
        MP_VERBOSE(vo, "MediaCodec Surface render mode changed: %s\n",
                   p->opts->render_mode ? "timed" : "boolean");

    if (p->opts->render_mode) {
        struct timespec now;
        // Android renders at renderTimestampNs in the SystemClock
        // elapsedRealtimeNanos() time base, which includes suspend and matches
        // CLOCK_BOOTTIME, not CLOCK_MONOTONIC; a monotonic timestamp would
        // land in the past by the accumulated suspend time.
        if (clock_gettime(CLOCK_BOOTTIME, &now) == 0) {
            int64_t time_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
            int ret = av_mediacodec_render_buffer_at_time(buffer, time_ns);
            if (ret < 0)
                MP_ERR(vo, "Timed MediaCodec release failed: %d; no retry\n", ret);
        } else {
            int clock_error = errno;
            MP_WARN(vo, "CLOCK_BOOTTIME read failed (%s); using boolean release once\n",
                    strerror(clock_error));
            int ret = av_mediacodec_release_buffer(buffer, 1);
            if (ret < 0)
                MP_ERR(vo, "MediaCodec boolean release after clock failure failed: %d; no retry\n", ret);
        }
    } else {
        int ret = av_mediacodec_release_buffer(buffer, 1);
        if (ret < 0)
            MP_ERR(vo, "MediaCodec boolean release failed: %d; no retry\n", ret);
    }
    mp_image_unrefp(&p->next_image);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);

    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
}

const struct vo_driver video_out_mediacodec_embed = {
    .description = "Android (Embedded MediaCodec Surface)",
    .name = "mediacodec_embed",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .global_opts = &mediacodec_embed_conf,
};
