#include <assert.h>
#include "video/out/yuv_atlas_read.h"
int main(void)
{
    const char *nonce="0123456789abcdef0123456789abcdef";
    bool enabled;
    assert(mp_yuv_atlas_read_config("",false,NAN,NAN,"",&enabled) && !enabled);
    assert(mp_yuv_atlas_read_config("0",false,NAN,NAN,"",&enabled) && !enabled);
    assert(mp_yuv_atlas_read_config("1",true,35.035,600.032766667,nonce,&enabled) && enabled);
    assert(!mp_yuv_atlas_read_config("1",false,35.035,600.032766667,nonce,&enabled));
    assert(!mp_yuv_atlas_read_config("1",true,35.035,35.035,nonce,&enabled));
    assert(!mp_yuv_atlas_read_config("1",true,35.035,NAN,nonce,&enabled));
    assert(!mp_yuv_atlas_read_config("1",true,35.035,600.0,"ABCDEF0123456789abcdef0123456789ab",&enabled));
    assert(!mp_yuv_atlas_read_config("yes",true,35.035,600.0,nonce,&enabled));
    assert(mp_yuv_atlas_ack_matches("0123456789abcdef0123456789abcdef:18446744073709551615",nonce,UINT64_MAX));
    assert(!mp_yuv_atlas_ack_matches("0123456789abcdef0123456789abcdef:1",nonce,2));
    assert(!mp_yuv_atlas_ack_matches("0123456789abcdef0123456789abcdef:01",nonce,1));
    assert(!mp_yuv_atlas_ack_matches("",nonce,1));
    assert(!mp_yuv_atlas_ack_matches(NULL,nonce,1));
    uint32_t a[8]={0},uv[8]={1,0,0,1,1,0,0,1},half[8]={1,0,0,0,1,0,0,0},seed[6]={0,0,1,0,0,1},mask;
    size_t bad_uv,bad_half;
    assert(mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask) && !mask);
    uv[1]=1;
    assert(mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask) && mask==1 && bad_uv==1);
    half[7]=0x3f800000;
    assert(mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask) && mask==3 && bad_half==1);
    uv[1]=0;
    assert(mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask) && mask==2);
    uv[3]=0;
    assert(!mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask));
    uv[3]=1;half[0]=2;
    assert(!mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask));
    half[0]=1;seed[5]=0;
    assert(!mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask));
    seed[5]=1;a[7]=0x7fc00000;
    assert(!mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask));
    a[7]=0;half[1]=0x3f800001;
    assert(!mp_yuv_atlas_read_data_valid(a,uv,half,seed,2,8,8,6,&bad_uv,&bad_half,&mask));
    assert(!mp_yuv_atlas_read_data_valid(a,uv,half,seed,SIZE_MAX,8,8,6,&bad_uv,&bad_half,&mask));
    return 0;
}
