#include <assert.h>
#include <string.h>
#include "video/out/yuv_compare_vo.h"
static void probe_fixture(uint32_t *base, uint32_t *probe, bool use_a)
{
    memset(base,0,104*sizeof(*base));
    memset(probe,0,40*sizeof(*probe));
    for (size_t i=0;i<4;i++) {
        size_t b=i*26,p=i*10;
        base[b]=base[b+10]=base[b+19]=base[b+25]=1;
        base[b+14]=base[b+20]=0x3f000000u;
        base[b+15]=base[b+21]=0x3e800000u;
        probe[p]=probe[p+1]=probe[p+2]=probe[p+3]=probe[p+4]=1;
        probe[p+5]=base[b+20];probe[p+6]=base[b+21];
        probe[p+8]=1;probe[p+9]=use_a?1:0;
    }
}
int main(void)
{
    bool quad = true, reverse = true;
    assert(mp_yuv_compare_quad_config("", "", false, false, &quad, &reverse) && !quad && !reverse);
    assert(mp_yuv_compare_quad_config("0", "0", false, false, &quad, &reverse) && !quad && !reverse);
    assert(mp_yuv_compare_quad_config("1", "0", true, false, &quad, &reverse) && quad && !reverse);
    assert(mp_yuv_compare_quad_config("1", "1", true, false, &quad, &reverse) && quad && reverse);
    assert(!mp_yuv_compare_quad_config("1", "", true, false, &quad, &reverse));
    assert(!mp_yuv_compare_quad_config("0", "1", true, false, &quad, &reverse));
    assert(!mp_yuv_compare_quad_config("1", "1", false, false, &quad, &reverse));
    assert(!mp_yuv_compare_quad_config("1", "1", true, true, &quad, &reverse));
    assert(!mp_yuv_compare_quad_config("1", "01", true, false, &quad, &reverse));
    assert(!mp_yuv_compare_quad_config("yes", "0", true, false, &quad, &reverse));
    uint8_t vertices[32], reversed[32];
    for (size_t i = 0; i < sizeof(vertices); i++) vertices[i] = i;
    for (size_t i = 0; i < 4; i++) memcpy(reversed + 8 * i, vertices + 8 * (3 - i), 8);
    assert(mp_yuv_compare_quad_payloads_valid(vertices, vertices, 8, 32, false));
    assert(mp_yuv_compare_quad_payloads_valid(vertices, reversed, 8, 32, true));
    assert(!mp_yuv_compare_quad_payloads_valid(vertices, reversed, 8, 32, false));
    assert(!mp_yuv_compare_quad_payloads_valid(vertices, vertices, 8, 32, true));
    assert(!mp_yuv_compare_quad_payloads_valid(vertices, reversed, 0, 0, true));
    assert(!mp_yuv_compare_quad_payloads_valid(vertices, reversed, SIZE_MAX, 32, true));
    assert(!mp_yuv_compare_quad_payloads_valid(vertices, reversed, 8, 31, true));
    assert(!mp_yuv_compare_quad_payloads_valid(NULL, reversed, 8, 32, true));
    reversed[15] ^= 1; // A padding/attribute byte cannot silently differ.
    assert(!mp_yuv_compare_quad_payloads_valid(vertices, reversed, 8, 32, true));
    uint32_t positions[8] = {0xbf800000,0xbf800000,0x3f800000,0xbf800000,
                             0xbf800000,0x3f800000,0x3f800000,0x3f800000};
    assert(mp_yuv_compare_quad_positions_valid((const uint8_t *)positions,8,32,0));
    assert(!mp_yuv_compare_quad_positions_valid((const uint8_t *)positions,8,32,4));
    positions[6]=0xbf800000; // Duplicate corner.
    assert(!mp_yuv_compare_quad_positions_valid((const uint8_t *)positions,8,32,0));
    positions[6]=0x3f800000;positions[0]=0x7fc00000;
    assert(!mp_yuv_compare_quad_positions_valid((const uint8_t *)positions,8,32,0));
    positions[0]=0xbf800000;
    uint32_t swap=positions[3];positions[3]=positions[7];positions[7]=swap;
    assert(!mp_yuv_compare_quad_positions_valid((const uint8_t *)positions,8,32,0));
    // Owner-current identity/generation failure closes the CPU frame by
    // submitting, but the executor must never swap a failed transaction.
    struct mp_yuv_compare_abort_plan plan = mp_yuv_compare_abort_plan(true,false,true);
    assert(plan.submit_frame && !plan.abort_no_gl && !plan.fatal);
    // Foreign context restored to the live owner takes the same safe close.
    plan = mp_yuv_compare_abort_plan(false,true,true);
    assert(plan.submit_frame && !plan.abort_no_gl && !plan.fatal);
    // Failed restore closes only through the no-GL abort and becomes terminal.
    plan = mp_yuv_compare_abort_plan(false,false,true);
    assert(!plan.submit_frame && plan.abort_no_gl && plan.fatal);
    for (int owner=0;owner<=1;owner++) {
        for (int restored=0;restored<=1;restored++) {
            plan=mp_yuv_compare_abort_plan(owner,restored,false);
            assert(!plan.submit_frame && !plan.abort_no_gl);
            assert(plan.fatal == (!owner && !restored));
        }
    }
    assert(mp_yuv_compare_target_matches(35.035,35.035,35.035));
    assert(!mp_yuv_compare_target_matches(35.035+0.000003,35.035,35.035));
    assert(!mp_yuv_compare_target_matches(35.035,35.035+0.000003,35.035));
    assert(!mp_yuv_compare_target_matches(NAN,35.035,35.035));
    assert(!mp_yuv_compare_target_matches(35.035,INFINITY,35.035));
    assert(!mp_yuv_compare_target_matches(35.035,35.035,NAN));
    assert(!mp_yuv_compare_target_matches(-1,-1,-1));
    uint32_t words[28] = {0};
    words[0]=words[10]=words[14]=words[24]=1;
    size_t bad=999;
    assert(mp_yuv_compare_words_valid(words,28,2,1,&bad));
    assert(!mp_yuv_compare_words_valid(words,27,2,1,&bad));
    assert(!mp_yuv_compare_words_valid(NULL,28,2,1,&bad));
    assert(!mp_yuv_compare_words_valid(words,28,SIZE_MAX,1,&bad));
    words[24]=0;
    assert(!mp_yuv_compare_words_valid(words,28,2,1,&bad) && bad==14);
    words[24]=1; words[0]=2;
    assert(!mp_yuv_compare_words_valid(words,28,2,1,&bad) && bad==0);
    words[0]=1; words[27]=0x7f800000u;
    assert(!mp_yuv_compare_words_valid(words,28,2,1,&bad) && bad==27);
    words[27]=0xffc00001u;
    assert(!mp_yuv_compare_words_valid(words,28,2,1,&bad) && bad==27);
    words[27]=0x80000000u;
    assert(mp_yuv_compare_words_valid(words,28,2,1,&bad));
    // Repeat is two complete pair chunks, not interleaved per-pixel records.
    uint32_t repeated[56] = {0};
    for (size_t i = 0; i < 56; i += 14)
        repeated[i] = repeated[i+10] = 1;
    assert(mp_yuv_compare_words_valid(repeated,56,2,2,&bad));
    assert(!mp_yuv_compare_words_valid(repeated,28,2,2,&bad));
    assert(!mp_yuv_compare_words_valid(repeated,56,2,1,&bad));
    assert(!mp_yuv_compare_words_valid(repeated,56,2,0,&bad));
    assert(!mp_yuv_compare_words_valid(repeated,56,2,3,&bad));
    assert(!mp_yuv_compare_words_valid(repeated,56,SIZE_MAX/14,2,&bad));
    repeated[38]=0; // A2 count of second pair chunk, first pixel
    assert(!mp_yuv_compare_words_valid(repeated,56,2,2,&bad) && bad==28);
    repeated[38]=1; repeated[55]=0xff800000u;
    assert(!mp_yuv_compare_words_valid(repeated,56,2,2,&bad) && bad==55);
    repeated[55]=0;
    assert(mp_yuv_compare_words_valid(repeated,56,2,2,&bad));

    bool repeat=true;
    assert(mp_yuv_compare_repeat_config("",&repeat) && !repeat);
    assert(mp_yuv_compare_repeat_config("0",&repeat) && !repeat);
    assert(mp_yuv_compare_repeat_config("1",&repeat) && repeat);
    assert(!mp_yuv_compare_repeat_config("2",&repeat));
    assert(!mp_yuv_compare_repeat_config("01",&repeat));
    assert(!mp_yuv_compare_repeat_config(" 1",&repeat));
    assert(!mp_yuv_compare_repeat_config("1 ",&repeat));
    assert(!mp_yuv_compare_repeat_config(NULL,&repeat));
    assert(!mp_yuv_compare_repeat_config("1",NULL));
    bool draw_ok[4]={true,true,true,true};
    uint64_t tokens[4]={UINT64_MAX,2,UINT64_MAX,2};
    assert(mp_yuv_compare_draws_valid(true,2,draw_ok,tokens));
    assert(mp_yuv_compare_draws_valid(false,1,draw_ok,tokens));
    assert(!mp_yuv_compare_draws_valid(false,2,draw_ok,tokens));
    assert(!mp_yuv_compare_draws_valid(true,1,draw_ok,tokens));
    for (int i=0;i<4;i++) {
        draw_ok[i]=false;
        assert(!mp_yuv_compare_draws_valid(true,2,draw_ok,tokens));
        draw_ok[i]=true;
        uint64_t saved=tokens[i]; tokens[i]=0;
        assert(!mp_yuv_compare_draws_valid(true,2,draw_ok,tokens));
        tokens[i]=saved;
    }
    tokens[2]--;
    assert(!mp_yuv_compare_draws_valid(true,2,draw_ok,tokens));
    tokens[2]++; tokens[3]++;
    assert(!mp_yuv_compare_draws_valid(true,2,draw_ok,tokens));
    assert(!mp_yuv_compare_draws_valid(true,2,NULL,tokens));
    assert(!mp_yuv_compare_draws_valid(true,2,draw_ok,NULL));
    uint64_t before[4]={7,7,7,7}, after[4]={7,7,7,7};
    assert(mp_yuv_compare_epochs_valid(2,before,after,7));
    assert(mp_yuv_compare_epochs_valid(1,before,after,7));
    assert(!mp_yuv_compare_epochs_valid(0,before,after,7));
    assert(!mp_yuv_compare_epochs_valid(3,before,after,7));
    assert(!mp_yuv_compare_epochs_valid(2,NULL,after,7));
    assert(!mp_yuv_compare_epochs_valid(2,before,NULL,7));
    assert(!mp_yuv_compare_epochs_valid(2,before,after,6));
    for (int i=0;i<4;i++) {
        before[i]++;
        assert(!mp_yuv_compare_epochs_valid(2,before,after,7));
        before[i]--; after[i]++;
        assert(!mp_yuv_compare_epochs_valid(2,before,after,7));
        after[i]--;
    }
    // Equal per-draw receipts still reject an epoch discontinuity between draws.
    before[2]=after[2]=before[3]=after[3]=8;
    assert(!mp_yuv_compare_epochs_valid(2,before,after,7));
    bool tap=true;
    assert(mp_yuv_compare_tap_config("",false,&tap) && !tap);
    assert(mp_yuv_compare_tap_config("0",false,&tap) && !tap);
    assert(!mp_yuv_compare_tap_config("1",false,&tap));
    assert(mp_yuv_compare_tap_config("1",true,&tap) && tap);
    assert(!mp_yuv_compare_tap_config("true",true,&tap));
    assert(!mp_yuv_compare_tap_config(NULL,true,&tap));
    assert(!mp_yuv_compare_tap_config("1",true,NULL));
    bool invariant=true;
    assert(mp_yuv_compare_invariant_config("",false,false,&invariant) && !invariant);
    assert(mp_yuv_compare_invariant_config("0",false,false,&invariant) && !invariant);
    for (int r=0;r<=1;r++) {
        for (int t=0;t<=1;t++)
            assert(mp_yuv_compare_invariant_config("1",r,t,&invariant) == (r && t));
    }
    assert(!mp_yuv_compare_invariant_config("2",true,true,&invariant));
    assert(!mp_yuv_compare_invariant_config("true",true,true,&invariant));
    assert(!mp_yuv_compare_invariant_config(NULL,true,true,&invariant));
    assert(!mp_yuv_compare_invariant_config("1",true,true,NULL));
    bool oracle=true,use_a=true;
    assert(mp_yuv_compare_oracle_config("","",false,false,false,&oracle,&use_a) && !oracle && !use_a);
    assert(mp_yuv_compare_oracle_config("0","0",false,false,false,&oracle,&use_a) && !oracle && !use_a);
    assert(mp_yuv_compare_oracle_config("1","0",true,true,false,&oracle,&use_a) && oracle && !use_a);
    assert(mp_yuv_compare_oracle_config("1","1",true,true,false,&oracle,&use_a) && oracle && use_a);
    assert(!mp_yuv_compare_oracle_config("1","",true,true,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("0","1",true,true,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("1","0",false,true,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("1","0",true,false,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("1","0",true,true,true,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("1","2",true,true,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("2","0",true,true,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config(NULL,"0",true,true,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("0",NULL,true,true,false,&oracle,&use_a));
    assert(!mp_yuv_compare_oracle_config("0","0",true,true,false,NULL,&use_a));
    assert(!mp_yuv_compare_oracle_config("0","0",true,true,false,&oracle,NULL));
    bool probe_mode=true;
    assert(mp_yuv_compare_probe_config("",false,&probe_mode) && !probe_mode);
    assert(mp_yuv_compare_probe_config("0",false,&probe_mode) && !probe_mode);
    assert(mp_yuv_compare_probe_config("1",true,&probe_mode) && probe_mode);
    assert(!mp_yuv_compare_probe_config("1",false,&probe_mode));
    assert(!mp_yuv_compare_probe_config("2",true,&probe_mode));
    assert(!mp_yuv_compare_probe_config(NULL,true,&probe_mode));
    assert(!mp_yuv_compare_probe_config("0",true,NULL));
    bool single=true;
    assert(mp_yuv_compare_single_config("",false,false,false,false,true,&single) && !single);
    assert(mp_yuv_compare_single_config("0",false,false,false,false,true,&single) && !single);
    assert(mp_yuv_compare_single_config("1",true,true,true,true,false,&single) && single);
    assert(!mp_yuv_compare_single_config("1",false,true,true,true,false,&single));
    assert(!mp_yuv_compare_single_config("1",true,false,true,true,false,&single));
    assert(!mp_yuv_compare_single_config("1",true,true,false,true,false,&single));
    assert(!mp_yuv_compare_single_config("1",true,true,true,false,false,&single));
    assert(!mp_yuv_compare_single_config("1",true,true,true,true,true,&single));
    assert(!mp_yuv_compare_single_config("2",true,true,true,true,false,&single));
    assert(!mp_yuv_compare_single_config(NULL,true,true,true,true,false,&single));
    assert(!mp_yuv_compare_single_config("1",true,true,true,true,false,NULL));
    bool barrier_ok[2]={true,true};
    uint64_t barrier_before[2]={9,9},barrier_after[2]={9,9};
    assert(mp_yuv_compare_barriers_valid(2,barrier_ok,barrier_before,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid(1,barrier_ok,barrier_before,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid(3,barrier_ok,barrier_before,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid(2,NULL,barrier_before,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid(2,barrier_ok,NULL,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid(2,barrier_ok,barrier_before,NULL,9));
    assert(mp_yuv_compare_barriers_valid_count(1,1,barrier_ok,barrier_before,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid_count(1,2,barrier_ok,barrier_before,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid_count(2,1,barrier_ok,barrier_before,barrier_after,9));
    assert(!mp_yuv_compare_barriers_valid_count(0,0,barrier_ok,barrier_before,barrier_after,9));
    for (int i=0;i<2;i++) {
        barrier_ok[i]=false;
        assert(!mp_yuv_compare_barriers_valid(2,barrier_ok,barrier_before,barrier_after,9));
        barrier_ok[i]=true; barrier_before[i]++;
        assert(!mp_yuv_compare_barriers_valid(2,barrier_ok,barrier_before,barrier_after,9));
        barrier_before[i]--; barrier_after[i]++;
        assert(!mp_yuv_compare_barriers_valid(2,barrier_ok,barrier_before,barrier_after,9));
        barrier_after[i]--;
    }
    const char *a0v="#version 310 es\nout vec2 _uv;\nvoid main() {\n gl_Position=vec4(0);\n}\n";
    const char *bv="#version 310 es\nout vec2 _uv;\ninvariant gl_Position;\ninvariant _uv;\nvoid main() {\n gl_Position=vec4(0);\n}\n";
    assert(mp_yuv_compare_invariant_vertex_valid(bv,a0v,"_uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(a0v,a0v,"_uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,a0v,"_u2",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,a0v,"1uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,a0v,"u v",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,a0v,"_uv",3));
    assert(!mp_yuv_compare_invariant_vertex_valid(NULL,a0v,"_uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,NULL,"_uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,a0v,NULL,4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,"void main() {void main() {","_uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(bv,"invariant _uv;\nvoid main() {","_uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(
        "invariant gl_Position;\ninvariant _uv;\nvoid main() {\n gl_Position=vec4(1);\n}\n",
        "void main() {\n gl_Position=vec4(0);\n}\n","_uv",4));
    assert(!mp_yuv_compare_invariant_vertex_valid(
        "invariant gl_Position;\ninvariant _uv;\n\nvoid main() {}", "void main() {}","_uv",4));
    uint32_t tapped[104]={0}; // two pixels in each of two complete 26word chunks
    for (size_t i=0;i<104;i+=26)
        tapped[i]=tapped[i+10]=tapped[i+19]=tapped[i+25]=1;
    assert(mp_yuv_compare_words_valid_layout(tapped,104,2,2,26,&bad));
    assert(mp_yuv_compare_oracle_uv_valid(tapped,104,2,&bad));
    assert(!mp_yuv_compare_oracle_uv_valid(NULL,104,2,&bad));
    assert(!mp_yuv_compare_oracle_uv_valid(tapped,103,2,&bad));
    assert(!mp_yuv_compare_oracle_uv_valid(tapped,104,SIZE_MAX,&bad));
    for (size_t i=0;i<104;i+=26) {
        for (size_t c=0;c<2;c++) {
            tapped[i+14+c]=0x80000000u; // signed zero still must match exactly
            assert(!mp_yuv_compare_oracle_uv_valid(tapped,104,2,&bad) && bad==i+14+c);
            tapped[i+20+c]=0x80000000u;
            assert(mp_yuv_compare_oracle_uv_valid(tapped,104,2,&bad));
            tapped[i+14+c]=tapped[i+20+c]=0;
        }
    }
    uint32_t probe_base[104],probe_data[40],mask;
    size_t bad_base=SIZE_MAX,bad_probe=SIZE_MAX;
    uint32_t android_words[14]={1,0,0,0,0,0,0,1,0,0,0,0,0,0};
    assert(mp_yuv_compare_android_words_valid(android_words,14,2,&bad));
    assert(!mp_yuv_compare_android_words_valid(NULL,14,2,&bad));
    assert(!mp_yuv_compare_android_words_valid(android_words,13,2,&bad));
    assert(!mp_yuv_compare_android_words_valid(android_words,14,SIZE_MAX,&bad));
    android_words[7]=0;
    assert(!mp_yuv_compare_android_words_valid(android_words,14,2,&bad) && bad==7);
    android_words[7]=1;android_words[13]=0x7f800000u;
    assert(!mp_yuv_compare_android_words_valid(android_words,14,2,&bad) && bad==13);
    for (int mode=0;mode<=1;mode++) {
        probe_fixture(probe_base,probe_data,mode);
        assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,
                                                mode,&mask,&bad_base,&bad_probe) && mask==0);
        assert(mp_yuv_compare_probe_words_valid_count(1,probe_base,52,probe_data,20,2,
                                                mode,&mask,&bad_base,&bad_probe) && mask==0);
        assert(!mp_yuv_compare_probe_words_valid(probe_base,52,probe_data,20,2,
                                                mode,&mask,&bad_base,&bad_probe));
        assert(!mp_yuv_compare_probe_words_valid_count(1,probe_base,104,probe_data,40,2,
                                                mode,&mask,&bad_base,&bad_probe));
        assert(!mp_yuv_compare_probe_words_valid_count(0,probe_base,52,probe_data,20,2,
                                                mode,&mask,&bad_base,&bad_probe));
        assert(!mp_yuv_compare_probe_words_valid_count(3,probe_base,52,probe_data,20,2,
                                                mode,&mask,&bad_base,&bad_probe));
    }
    assert(!mp_yuv_compare_probe_words_valid(NULL,104,probe_data,40,2,true,&mask,NULL,NULL));
    assert(!mp_yuv_compare_probe_words_valid(probe_base,104,NULL,40,2,true,&mask,NULL,NULL));
    assert(!mp_yuv_compare_probe_words_valid(probe_base,103,probe_data,40,2,true,&mask,NULL,NULL));
    assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,39,2,true,&mask,NULL,NULL));
    assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,SIZE_MAX,true,&mask,NULL,NULL));
    assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,NULL,NULL,NULL));
    for (size_t pixel=0;pixel<4;pixel++) {
        const size_t bcounts[]={0,10,25},pcounts[]={0,1,2};
        for (size_t c=0;c<3;c++) {
            probe_fixture(probe_base,probe_data,true);probe_base[pixel*26+bcounts[c]]=0;
            assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,&bad_base,&bad_probe) && bad_base==pixel*26);
            probe_fixture(probe_base,probe_data,true);probe_data[pixel*10+pcounts[c]]=2;
            assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,&bad_base,&bad_probe) && bad_probe==pixel*10);
        }
        probe_fixture(probe_base,probe_data,true);probe_base[pixel*26+19]=2;
        assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL));
        for (size_t c=1;c<26;c++) {
            if(c==10||c==19||c==25)continue;
            probe_fixture(probe_base,probe_data,true);probe_base[pixel*26+c]=0x7f800000u;
            assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,&bad_base,NULL) && bad_base==pixel*26+c);
        }
    }
    const uint32_t illegal[]={16,2,3};
    for(size_t c=0;c<3;c++) {
        probe_fixture(probe_base,probe_data,true);probe_data[7+c]=illegal[c];
        assert(!mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL));
    }
    // Complete execution may retain dependency and consistency failures only
    // as diagnostic-invalid. It never relaxes the older 26word count gate.
    probe_fixture(probe_base,probe_data,true);probe_data[3]=0;
    probe_data[7]=2;probe_data[8]=0;probe_data[9]=2;probe_base[19]=0;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==1);
    assert(mp_yuv_compare_probe_words_valid_count(1,probe_base,52,probe_data,20,2,true,&mask,NULL,NULL) && mask==1);
    assert(!mp_yuv_compare_words_valid_layout(probe_base,104,2,2,26,NULL));
    probe_fixture(probe_base,probe_data,true);probe_data[7]=2;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==(1|2|4));
    probe_fixture(probe_base,probe_data,true);probe_data[8]=0;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==(1|4|8|16));
    probe_fixture(probe_base,probe_data,true);probe_data[9]=0;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==8);
    probe_fixture(probe_base,probe_data,true);probe_base[19]=0;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==16);
    probe_fixture(probe_base,probe_data,true);probe_data[5]^=1;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==(32|64));
    probe_fixture(probe_base,probe_data,true);probe_base[14]^=1;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==64);
    probe_fixture(probe_base,probe_data,false);probe_base[14]^=1;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,false,&mask,NULL,NULL) && mask==0);
    probe_fixture(probe_base,probe_data,true);probe_data[5]=0x7fc00000u;
    probe_data[7]=8;probe_data[8]=0;probe_data[9]=2;probe_base[19]=0;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==(1|32));
    probe_fixture(probe_base,probe_data,true);probe_data[3]=probe_data[4]=probe_data[5]=probe_data[6]=0;
    probe_data[7]=1;probe_data[8]=0;probe_data[9]=2;probe_base[19]=0;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==1);
    probe_data[3]=1;
    assert(mp_yuv_compare_probe_words_valid(probe_base,104,probe_data,40,2,true,&mask,NULL,NULL) && mask==(1|2));
    assert(!mp_yuv_compare_words_valid_layout(tapped,52,2,1,26,&bad));
    assert(!mp_yuv_compare_words_valid_layout(tapped,104,2,2,25,&bad));
    assert(!mp_yuv_compare_words_valid_layout(tapped,104,SIZE_MAX,2,26,&bad));
    assert(!mp_yuv_compare_words_valid_layout(tapped,103,2,2,26,&bad));
    assert(!mp_yuv_compare_words_valid(tapped,104,2,2,&bad));
    for (size_t i=0;i<104;i+=26) {
        const size_t counts[]={0,10,19,25};
        for (size_t j=0;j<4;j++) {
            tapped[i+counts[j]]=0;
            assert(!mp_yuv_compare_words_valid_layout(tapped,104,2,2,26,&bad) && bad==i);
            tapped[i+counts[j]]=1;
        }
        for (size_t j=1;j<26;j++) {
            if (j==10 || j==19 || j==25) continue;
            tapped[i+j]=0x7f800000u;
            assert(!mp_yuv_compare_words_valid_layout(tapped,104,2,2,26,&bad) && bad==i+j);
            tapped[i+j]=0;
        }
    }
    return 0;
}
