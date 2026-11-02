#ifndef MPV_YUV_FRAME_TRACE_H
#define MPV_YUV_FRAME_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

struct ra_yuv_frame_trace {
    uint64_t frame_id;
    double source_pts, current_pts, duration;
    bool still, redraw, repeat;
};

// Pure diagnostic trigger. Callers update previous after every successful swap,
// not only after emitting, so continuous playback never becomes a periodic log.
static inline bool mp_yuv_frame_trace_should_emit(
    const struct ra_yuv_frame_trace *frame,
    const struct ra_yuv_frame_trace *previous,
    bool seen, bool route_changed, bool generation_changed)
{
    return !seen || route_changed || generation_changed ||
        (frame->still && (!previous->still || frame->frame_id != previous->frame_id ||
                          frame->source_pts != previous->source_pts)) ||
        fabs(frame->source_pts - previous->source_pts) >
            fmax(0.25, 3.0 * fabs(frame->duration));
}

#endif
