#include <assert.h>
#include "video/out/yuv_atlas_anchor.h"

int main(void)
{
    assert(mp_yuv_atlas_uninstrumented_source("vec4 framebuffer_value;"));
    assert(!mp_yuv_atlas_uninstrumented_source("layout(std430) buffer A {float x[];};"));
    assert(!mp_yuv_atlas_uninstrumented_source("readonly buffer\nA {float x[];};"));
    assert(!mp_yuv_atlas_uninstrumented_source("float mks_tap_count;"));
    assert(!mp_yuv_atlas_uninstrumented_source(NULL));
    bool enabled;
    assert(mp_yuv_atlas_config("", false, &enabled) && !enabled);
    assert(mp_yuv_atlas_config("0", true, &enabled) && !enabled);
    assert(mp_yuv_atlas_config("1", false, &enabled) && enabled);
    assert(!mp_yuv_atlas_config("1", true, &enabled));
    assert(!mp_yuv_atlas_config("01", false, &enabled));
    assert(!mp_yuv_atlas_config("1 ", false, &enabled));
    const uint32_t legal[] = {0,0x80000000,0x33800000,0xb3800000,
        0x387fc000,0x38800000,0x3f800000,0x477fe000,0xc77fe000};
    for (size_t i=0;i<sizeof(legal)/sizeof(legal[0]);i++)
        assert(mp_yuv_atlas_half_expanded(legal[i]));
    const uint32_t illegal[] = {1,0x33000000,0x33800001,0x38800001,
        0x47800000,0x7f800000,0xff800000,0x7fc00000,0xffc00000};
    for (size_t i=0;i<sizeof(illegal)/sizeof(illegal[0]);i++)
        assert(!mp_yuv_atlas_half_expanded(illegal[i]));
    for (uint32_t h=0;h<65536;h++) {
        unsigned e=(h>>10)&31, mant=h&1023;
        if(e==31) continue;
        uint32_t expanded=(h&0x8000u)<<16;
        if(e) expanded|=((e+112u)<<23)|(mant<<13);
        else if(mant) {
            unsigned shifts=0;
            while(!(mant&1024u)){mant<<=1;shifts++;}
            expanded|=((113u-shifts)<<23)|((mant&1023u)<<13);
        }
        assert(mp_yuv_atlas_half_expanded(expanded));
    }
    uint32_t a[8]={0},b[8]={0},c[8]={0},uv[6]={0,0,1,0,0,1},mask;
    size_t ab,ac;
    assert(mp_yuv_atlas_data_valid(a,b,c,uv,2,8,6,&ab,&ac,&mask) && !mask);
    b[7]=0x80000000;
    assert(mp_yuv_atlas_data_valid(a,b,c,uv,2,8,6,&ab,&ac,&mask) && mask==2 && ab==1 && !ac);
    c[0]=0x3f800000;
    assert(mp_yuv_atlas_data_valid(a,b,c,uv,2,8,6,&ab,&ac,&mask) && mask==3 && ab==1 && ac==1);
    b[7]=0;
    assert(mp_yuv_atlas_data_valid(a,b,c,uv,2,8,6,&ab,&ac,&mask) && mask==1);
    assert(!mp_yuv_atlas_data_valid(a,b,c,uv,0,0,0,&ab,&ac,&mask));
    assert(!mp_yuv_atlas_data_valid(a,b,c,uv,SIZE_MAX,8,6,&ab,&ac,&mask));
    assert(!mp_yuv_atlas_data_valid(a,b,c,uv,2,7,6,&ab,&ac,&mask));
    assert(!mp_yuv_atlas_data_valid(a,b,c,uv,2,8,5,&ab,&ac,&mask));
    assert(!mp_yuv_atlas_data_valid(NULL,b,c,uv,2,8,6,&ab,&ac,&mask));
    uv[5]=0;
    assert(!mp_yuv_atlas_data_valid(a,b,c,uv,2,8,6,&ab,&ac,&mask));
    uv[5]=0x80000001;
    assert(!mp_yuv_atlas_data_valid(a,b,c,uv,2,8,6,&ab,&ac,&mask));
    uv[5]=1;uv[0]=0x7fc00000;
    assert(!mp_yuv_atlas_data_valid(a,b,c,uv,2,8,6,&ab,&ac,&mask));
    return 0;
}
