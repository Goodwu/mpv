#include <assert.h>
#include "video/out/yuv_frame_trace.h"
int main(void)
{
    struct ra_yuv_frame_trace last = {.frame_id=100, .source_pts=35.035, .duration=0.0333667};
    struct ra_yuv_frame_trace f = last;
    assert(mp_yuv_frame_trace_should_emit(&f,&last,false,false,false));
    assert(!mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,true,false));
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,false,true));
    f.still=true;
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    last=f;
    f.redraw=true; f.repeat=true;
    assert(!mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    f.frame_id++;
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    f.frame_id=last.frame_id; f.source_pts+=0.0333667;
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    f.still=false; last.still=false;
    assert(!mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    f.source_pts=last.source_pts+0.25;
    assert(!mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    f.source_pts=last.source_pts+0.251;
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    f.source_pts=600.032766667;
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    last=f; f.source_pts=35.035;
    assert(mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    // Long continuous playback updates previous each time and emits nothing.
    for (int i=0;i<3000;i++) {
        last=f; f.source_pts+=0.0333667; f.frame_id++;
        assert(!mp_yuv_frame_trace_should_emit(&f,&last,true,false,false));
    }
    return 0;
}
