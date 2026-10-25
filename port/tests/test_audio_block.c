/* Complete overlay differential test, plus an independent integer DSP oracle.
 * No private game fixture is needed for this synthetic contract test. */
#include "spu_audio_block.h"
#include "spu_float_scope.h"
#include <assert.h>
#include <time.h>

extern void ms_audio_32770_spu_func_00032770(spu_context*);
extern void ref_audio_32770_spu_func_00032770(spu_context*);
#include "audio_block_reference.inc"

SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_register_function(unsigned pc, void (*fn)(spu_context*)) {}
void spu_indirect_branch(spu_context* c) { abort(); }
void spu_indirect_branch_mt(spu_context* c) { abort(); }
void spu_halt(spu_context* c) { abort(); }
void spu_stop(spu_context* c) { abort(); }
void spu_drain_call(spu_context* c, unsigned pc) { abort(); }
void spu_img_restore(spu_context* c, int image) { abort(); }

typedef struct { unsigned kind, channel; u128 value; } event;
static event events[128];
static unsigned event_count, event_pos, dma_pos, lsa, recording;
static uint8_t dma_block[384], dma_boundary[2][SPU_LS_SIZE];
static void channel(unsigned kind, unsigned ch, u128 value)
{
    assert(event_pos < 128);
    event e = {kind, ch, value};
    if (recording) events[event_pos] = e;
    else {
        assert(event_pos < event_count);
        assert(events[event_pos].kind == e.kind && events[event_pos].channel == e.channel);
        assert(!memcmp(&events[event_pos].value, &e.value, 16));
    }
    ++event_pos;
}
u128 spu_rdch(spu_context* c, unsigned ch) {
    assert(ch == MFC_RdTagStat); u128 v = spu_pref_u32(0xffff); channel(1,ch,v); return v;
}
uint32_t spu_rchcnt(spu_context* c, unsigned ch) {
    assert(ch == MFC_WrTagUpdate); channel(2,ch,spu_pref_u32(1)); return 1;
}
void spu_wrch(spu_context* c, unsigned ch, u128 v)
{
    channel(3,ch,v);
    if (ch == MFC_LSA) lsa = v._u32[0];
    if (ch == MFC_Cmd) {
        assert(dma_pos < 2);
        if (recording) memcpy(dma_boundary[dma_pos], c->ls, SPU_LS_SIZE);
        else assert(!memcmp(dma_boundary[dma_pos], c->ls, SPU_LS_SIZE));
        assert(lsa <= SPU_LS_SIZE - sizeof dma_block);
        if (dma_pos++ == 0) {
            assert(v._u32[0] == 64); memcpy(c->ls + lsa, dma_block, sizeof dma_block);
        } else assert(v._u32[0] == 32);
    }
}

static uint32_t rng = 0xbadf00d;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; return rng ^= rng << 5; }
static const uint32_t edge[] = {0, 0x80000000, 1, 0x007fffff, 0x00800000,
    0x80800000, 0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7fffffff, 0xffffffff};
static uint32_t value(unsigned mode) {
    return mode == 1 ? edge[random32() % (sizeof edge / sizeof *edge)] :
        mode == 2 ? random32() : (0x3d000000 + (random32() & 0xffffff)) | (random32() & 0x80000000);
}
static void store(uint8_t* p, uint32_t x) { x = SPU_BSWAP32(x); memcpy(p, &x, 4); }
static uint32_t load(const uint8_t* p) { uint32_t x; memcpy(&x,p,4); return SPU_BSWAP32(x); }

static void oracle(const uint8_t* in, uint8_t* out, unsigned n,
    const ms_audio_block_coeff* k, ms_audio_block_state* s)
{
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t x = load(in + ((i & ~3u) + ((i + lane) & 3)) * 4);
            uint32_t y = spu__fm_lane(k->a0[lane], x);
            y = spu__fma_lane(s->x1[lane], k->a1[lane], y, 0, 0);
            y = spu__fma_lane(s->x2[lane], k->a2[lane], y, 0, 0);
            y = spu__fma_lane(s->y1[lane], k->b1[lane], y, 0, 0);
            y = spu__fma_lane(s->y2[lane], k->b2[lane], y, 0, 0);
            if (!lane) store(out + i * 4, y);
            s->x2[lane] = s->x1[lane]; s->x1[lane] = x;
            s->y2[lane] = s->y1[lane]; s->y1[lane] = y;
        }
    }
}

static void test_kernel(void)
{
    uint8_t input[4096], expected[4096], actual[4096];
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 6; ++mode) for (unsigned n = 4; n <= 1024; n += 4) {
        ms_audio_block_coeff k; ms_audio_block_state s, a;
        uint32_t* words = (uint32_t*)&k;
        for (unsigned i=0;i<sizeof k/4;++i) words[i]=value(mode);
        words=(uint32_t*)&s;
        for (unsigned i=0;i<sizeof s/4;++i) words[i]=value(mode);
        for (unsigned i=0;i<sizeof input;i+=4) store(input+i,value(mode));
        if(mode>=3) {
            words=(uint32_t*)&k;
            for(unsigned i=0;i<sizeof k/4;++i)
                words[i]=(mode==4?0x43800001u:0x43800000u)|(random32()&0x80000000u);
            words=(uint32_t*)&s;
            for(unsigned i=0;i<sizeof s/4;++i)
                words[i]=(mode==5?0x4f800001u:0x4f800000u)|(random32()&0x80000000u);
            for(unsigned i=0;i<sizeof input;i+=4) store(input+i,0x4f800000u|(random32()&0x80000000u));
        }
        memset(expected,0xa5,sizeof expected); memcpy(actual,expected,sizeof actual); a=s;
        oracle(input,expected,n,&k,&s);
        /* Split calls exercise persistent streaming state and raw x history. */
        unsigned split = (n / 8) * 4;
        if (split) ms_audio_block_process(input,actual,split,&k,&a);
        ms_audio_block_process(input+split*4,actual+split*4,n-split,&k,&a);
        assert(!memcmp(expected,actual,sizeof actual)); assert(!memcmp(&s,&a,sizeof s)); ++cases;
    }
    assert(ms_ab_normal_windows && ms_ab_wide_windows && ms_ab_wide_blocks);
    ms_audio_block_coeff zero_coeff={0};ms_audio_block_state zero_state,zero_copy;
    memset(&zero_state,0xff,sizeof zero_state);zero_copy=zero_state;
    ms_audio_block_process(NULL,NULL,0,&zero_coeff,&zero_state);
    assert(!memcmp(&zero_copy,&zero_state,sizeof zero_state));
    printf("PASS %u streaming blocks vs independent integer arithmetic, all four history lanes\n",cases);
}

static spu_context initial, reference, candidate;
static void prepare(unsigned n, unsigned mode, unsigned variant)
{
    memset(&initial,0,sizeof initial);initial.ls=initial.ls_store;
    initial.image_id=286484578; initial.pc=0x32770;initial.host_depth=1;
    for (unsigned r=0;r<128;++r) for (unsigned l=0;l<4;++l) initial.gpr[r]._u32[l]=value(mode);
    for (unsigned i=0;i<SPU_LS_SIZE;i+=4) store(initial.ls+i,value(mode));
    for (unsigned i=0;i<sizeof dma_block;i+=4) store(dma_block+i,value(mode));
    initial.gpr[0]=spu_pref_u32(0x1444);
    initial.gpr[3]._u32[0]=0x10000; initial.gpr[4]._u32[0]=0x18000;
    initial.gpr[5]._u32[0]=0x20000; initial.gpr[6]._u32[0]=0x21000; initial.gpr[7]._u32[0]=0x22000;
    store(initial.ls+0x20000,0x100000);
    store(initial.ls+0x22000,variant%9 == 8 ? 0xffffffff : variant%8);
    store(initial.ls+0x2202c,n);
}
static void compare_entry(void)
{
    reference=initial;reference.ls=reference.ls_store;
    candidate=initial;candidate.ls=candidate.ls_store;
    event_pos=dma_pos=0;recording=1;g_spu_trampoline_fn=NULL;
    ref_audio_32770_spu_func_00032770(&reference);
    assert(dma_pos==2);event_count=event_pos;
    void (*expected_trampoline)(spu_context*)=g_spu_trampoline_fn;
    event_pos=dma_pos=0;recording=0;g_spu_trampoline_fn=NULL;
    ms_audio_32770_spu_func_00032770(&candidate);
    assert(event_pos==event_count && dma_pos==2);
    assert(g_spu_trampoline_fn==expected_trampoline);
    if (memcmp(reference.gpr,candidate.gpr,sizeof reference.gpr)) {
        for(unsigned i=0;i<128;++i) for(unsigned j=0;j<4;++j)
            if(reference.gpr[i]._u32[j]!=candidate.gpr[i]._u32[j])
                fprintf(stderr,"r%u.%u expected=%08x actual=%08x\n",i,j,reference.gpr[i]._u32[j],candidate.gpr[i]._u32[j]);
        abort();
    }
    reference.ls=candidate.ls=initial.ls;
    assert(!memcmp(&reference,&candidate,sizeof reference));
}

static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9; }
int main(int argc,char**argv)
{
    uint64_t saved=ms_rtz_enter();test_kernel();unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned n=0;n<=1040;n+=8) {
        prepare(n,mode,n/8);compare_entry();++cases;
    }
    for(unsigned i=0;i<11;++i) {
        prepare(64,2,i);
        switch(i) {
        case 0:initial.policy_mode=1;break;
        case 1:initial.image_id++;break;
        case 2:initial.gpr[3]._u32[0]+=4;break;
        case 3:initial.gpr[4]._u32[0]+=4;break;
        case 4:initial.gpr[4]._u32[0]=initial.gpr[3]._u32[0];break;
        case 5:initial.gpr[4]._u32[0]=initial.gpr[3]._u32[0]+16;break;
        case 6:store(initial.ls+0x2202c,61);break;
        case 7:store(initial.ls+0x2202c,0xffffffff);break;
        case 8:initial.gpr[3]._u32[0]=SPU_LS_SIZE-16;break;
        case 9:initial.gpr[4]._u32[0]=SPU_LS_SIZE-16;break;
        case 10:initial.host_depth=0;break;
        }
        compare_entry();++cases;
    }
    printf("PASS %u complete filter entries: entire context/LS, both DMA boundaries and every channel operation\n",cases);
    /* A real captured loop state can be passed by the replay wrapper. */
    prepare(512,0,0);
    initial.gpr[29]=spu_il(0x18000);initial.gpr[30]=spu_il(0x10000);
    initial.gpr[37]=spu_il(512);initial.gpr[39]=spu_il(0);
    if(argc==2) {
        FILE*f=fopen(argv[1],"rb");assert(f&&fread(&initial,sizeof initial,1,f)==1&&fgetc(f)==EOF);fclose(f);initial.ls=initial.ls_store;
    } else assert(argc==1);
    for(unsigned pass=0;pass<4;++pass) {
        double elapsed[3]={0};const unsigned repeats=3000;
        for(unsigned i=0;i<repeats;++i) {
            for(unsigned kind=0;kind<3;++kind) {
                unsigned mode=(kind+i+pass)%3;
                candidate=initial;candidate.ls=candidate.ls_store;double t=now();
                if(mode==1) ms_audio_filter_prefix(&candidate);
                if(mode==2) assert(ms_audio_block_prepare(&candidate,0));
                reference_loop(&candidate);elapsed[mode]+=now()-t;
            }
        }
        printf("BENCH samples=%u original=%.3f us legacy=%.3f us block=%.3f us block_vs_legacy=%.3fx\n",
            initial.gpr[37]._u32[0],elapsed[0]*1e6/repeats,elapsed[1]*1e6/repeats,elapsed[2]*1e6/repeats,elapsed[1]/elapsed[2]);
    }
    /* Wrong rounding mode must reject the adapter without changing state. */
    ms_rtz_leave(saved);uint64_t nearest=saved&~(3ull<<22);ms_rtz_leave(nearest);
    candidate=initial;assert(!ms_audio_block_prepare(&candidate,0));assert(!memcmp(&candidate,&initial,sizeof initial));
    ms_rtz_leave(saved);
}
