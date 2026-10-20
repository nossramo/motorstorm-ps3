#include "spu_cull_profile.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
int g_spu_ls_watch_n=0;
static uint64_t expected_fpcr;
static unsigned watches;
static uint64_t fpcr(void) { uint64_t r;__asm__ volatile("mrs %0, fpcr":"=r"(r));return r; }
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_indirect_branch(spu_context* c) { (void)c; assert(0); }
void jobmod_be896a6374ba_e12230_spu_func_0000847C(spu_context* c) { (void)c; assert(0); }
void jobmod_be896a6374ba_e12230_spu_func_00008420(spu_context* c) { (void)c; assert(0); }
void jobmod_be896a6374ba_e12230_spu_func_00008428(spu_context* c) { (void)c; assert(0); }
void spu_ls_watch_slow(uint32_t a,int b,const uint8_t*c,uint32_t d,uint32_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    assert(fpcr() == expected_fpcr); watches++;
}
#include "culling.inc"
static uint64_t seed=0x48ac1ed14;
static uint32_t random32(void) { seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return (uint32_t)seed; }
static uint32_t value(int ordinary) {
    uint32_t r=random32();
    if(ordinary==2) return (r&0x807fffff)|((97+random32()%60)<<23);
    if(ordinary)return (r&0x807fffff)|((120+random32()%15)<<23);
    switch(random32()%5) {
    case 0:return r&0x807fffff;
    case 1:return (r&0x807fffff)|0x7f800000;
    case 2:return (r&0x807fffff)|((120+random32()%15)<<23);
    default:return r;
    }
}
static void put_be(uint8_t* p, uint32_t v) { p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9; }
int main(void) {
    spu_context* a=calloc(1,sizeof(*a));spu_context* b=calloc(1,sizeof(*b));
    spu_context* c=calloc(1,sizeof(*c));assert(a&&b&&c);
    spu_context* input=calloc(1,sizeof(*input));assert(input);
    ps3_ft_rec records[4];
    g_ps3_ft.buf=records;g_ps3_ft.cap=4;g_ps3_ft.start=0;g_ps3_ft.end=UINT64_MAX;
    uint8_t* ls=calloc(1,SPU_LS_SIZE);assert(ls);a->ls=ls;b->ls=ls;c->ls=ls;
    const uint64_t host=fpcr();
    expected_fpcr=host;
    unsigned branches[2]={0,0}, eligible=0;
    for(unsigned i=0;i<300000;i++) {
        for(unsigned reg=0;reg<128;reg++)for(unsigned lane=0;lane<4;lane++)a->gpr[reg]._u32[lane]=value(i%3);
        for(unsigned k=0;k<4;k++) put_be(ls+4*k,value(i%3));
        a->gpr[14]._u32[0]=0;a->host_depth=i&1;a->pc=0x8480;
        memcpy(b,a,sizeof(*a));memcpy(c,a,sizeof(*a));
        if (i%300<3) memcpy(input,a,sizeof(*input));
        u128 prior=c->gpr[9];c->gpr[9]=spu_ls_read128(c,0);
        const int accepted=ms_cull_bounds_inputs(c);
        eligible+=accepted;c->gpr[9]=prior;
        g_spu_trampoline_fn=NULL;cull_reference(a);void(*expected)(spu_context*)=g_spu_trampoline_fn;
        g_spu_trampoline_fn=NULL;cull_scope(b);
        assert(fpcr()==host);
        if(memcmp(a,b,sizeof(*a))||g_spu_trampoline_fn!=expected){fprintf(stderr,"Mismatch iteration %u\n",i);return 1;}
        g_spu_trampoline_fn=NULL;cull_bounded(c);
        assert(fpcr()==host);
        if(memcmp(a,c,sizeof(*a))||g_spu_trampoline_fn!=expected){fprintf(stderr,"Bounded mismatch iteration %u\n",i);return 1;}
        branches[b->pc==0x847c]++;
        if (i%300<3) for (unsigned variant=0;variant<2;++variant) {
            memcpy(b,input,sizeof(*b));memset(records,0,sizeof(records));
            g_ps3_ft.on=2;g_ps3_ft.n=0;ms_cull_probe_counter=0;
            ms_cull_probe probe;ms_cull_probe_begin(&probe);
            assert(probe.active && fpcr()==host);
            g_spu_trampoline_fn=NULL;
            if(variant)ms_cull_profiled_bounds(b);else ms_cull_profiled_scope(b);
            ms_cull_probe_end(&probe,b->spu_id);
            assert(!memcmp(a,b,sizeof(*a))&&g_spu_trampoline_fn==expected&&fpcr()==host);
            assert(g_ps3_ft.n==4 && records[0].kind==1000 && records[0].b==1);
            assert(records[1].kind==1001 && records[1].a==(unsigned)accepted && records[1].b==1);
            assert(!ms_cull_current_probe.active);
            g_ps3_ft.on=3;
        }
    }
    assert(branches[0]&&branches[1]&&eligible>=200000&&eligible<300000);
    /* Nested/abandoned guest calls invalidate a sample without retaining a
     * dangling host-stack pointer or later publishing the abandoned span. */
    g_ps3_ft.on=2;g_ps3_ft.n=0;ms_cull_probe_counter=0;
    ms_cull_probe outer,inner;
    ms_cull_probe_begin(&outer);ms_cull_probe_counter=0;ms_cull_probe_begin(&inner);
    ms_cull_probe_end(&inner,0);ms_cull_probe_end(&outer,0);
    assert(g_ps3_ft.n==4 && !ms_cull_current_probe.active);
    g_ps3_ft.n=0;ms_cull_probe_counter=0;ms_cull_probe_begin(&outer);
    ms_cull_probe_counter=0;ms_cull_probe_begin(&inner); /* abandon outer */
    ms_cull_probe_end(&inner,0);
    assert(g_ps3_ft.n==4 && outer.generation!=inner.generation);
    g_ps3_ft.n=0;ms_cull_probe_counter=0;
    for(unsigned i=0;i<64;++i) { ms_cull_probe_begin(&inner);ms_cull_probe_end(&inner,0); }
    assert(g_ps3_ft.n==4);
    g_ps3_ft.n=0;ms_cull_probe_counter=0;g_ps3_ft.start=UINT64_MAX;
    ms_cull_probe_begin(&inner);assert(!inner.active && !ms_cull_current_probe.active);
    g_ps3_ft.start=0;ms_cull_probe_counter=0;
    memset(a->gpr,0,sizeof(a->gpr));
    a->gpr[9]._u32[0]=0x80000000;a->gpr[15]._u32[0]=1;
    a->gpr[16]._u32[0]=0x30000000;a->gpr[17]._u32[0]=0x4e800000;
    ms_cull_probe_begin(&inner);assert(!ms_cull_probe_inputs(a));
    ms_cull_probe_end(&inner,0);
    assert(records[2].kind==1002 && records[2].a==1 && records[2].b==1);
    assert(records[3].kind==1003 && records[3].a==1 && records[3].b==1);
    g_ps3_ft.on=3;
    for(unsigned i=0;i<1000000;i++) {
        u128 x,y;
        for(unsigned lane=0;lane<4;lane++) {
            x._u32[lane]=value(0); y._u32[lane]=value(0);
        }
        const u128 expected=spu_fcgt_ref(x,y), actual=ms_cull_fcgt(x,y);
        assert(!memcmp(&expected,&actual,sizeof(expected)));
    }

    /* Every live input/lane must reject values outside the proven range.
     * +0 is admitted; -0, subnormals and extended values take the old path. */
    const unsigned inputs[]={9,15,16,17,18,19,20,21,22,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41};
    const uint32_t edges[]={0,0x80000000,1,0x007fffff,0x00800000,0x307fffff,
        0x30800000,0xb0800000,0x4e7fffff,0xce7fffff,0x4e800000,0xce800000,0x7f800000,0xffffffff};
    for(unsigned reg=0;reg<128;reg++)for(unsigned lane=0;lane<4;lane++)a->gpr[reg]._u32[lane]=0x3f800000;
    for(unsigned j=0;j<sizeof(inputs)/sizeof(inputs[0]);j++)for(unsigned lane=0;lane<4;lane++)
        for(unsigned edge=0;edge<sizeof(edges)/sizeof(edges[0]);edge++) {
            uint32_t bits=edges[edge],mag=bits&0x7fffffff;
            a->gpr[inputs[j]]._u32[lane]=bits;
            assert(ms_cull_bounds_inputs(a)==(!bits||(mag>=0x30800000&&mag<0x4e800000)));
            a->gpr[inputs[j]]._u32[lane]=0x3f800000;
        }

    /* The old scoped path is the comparator for alternate caller FPCR modes.
     * Callbacks must run in caller mode, and the scope must restore it. */
    for(unsigned mode=0;mode<4;mode++) {
        expected_fpcr=(host&~0xc00000ull)|((uint64_t)mode<<22);
        __asm__ volatile("msr fpcr, %0"::"r"(expected_fpcr):"memory");
        for(unsigned i=0;i<10000;i++) {
            for(unsigned reg=0;reg<128;reg++)for(unsigned lane=0;lane<4;lane++)a->gpr[reg]._u32[lane]=value(2);
            for(unsigned k=0;k<4;k++)put_be(ls+4*k,value(2));
            a->gpr[14]._u32[0]=0;a->host_depth=1;a->pc=0x8480;
            memcpy(b,a,sizeof(*a));
            watches=0;g_spu_ls_watch_n=1;
            g_spu_trampoline_fn=NULL;cull_scope(a);void(*expected)(spu_context*)=g_spu_trampoline_fn;
            g_spu_trampoline_fn=NULL;cull_bounded(b);
            assert(fpcr()==expected_fpcr && watches==2);
            assert(!memcmp(a,b,sizeof(*a))&&g_spu_trampoline_fn==expected);
            g_spu_ls_watch_n=0;
        }
    }
    __asm__ volatile("msr fpcr, %0"::"r"(host):"memory");
    expected_fpcr=host;
    unsigned setup_branches[2]={0,0};
    for(unsigned i=0;i<100000;++i) {
        for(unsigned reg=0;reg<128;++reg)for(unsigned lane=0;lane<4;++lane)a->gpr[reg]._u32[lane]=value(i%3);
        for(unsigned k=0;k<16;++k)put_be(ls+4*k,value(i%3));
        for(unsigned k=0;k<16;++k)ls[0x16370+k]= i%3 ? k : random32();
        a->gpr[3]._u32[0]=0;a->gpr[5]._u32[0]=i&1;a->pc=0x8380;
        memcpy(b,a,sizeof(*a));
        watches=0;g_spu_ls_watch_n=1;
        g_spu_trampoline_fn=NULL;cull_setup_reference(a);void(*expected)(spu_context*)=g_spu_trampoline_fn;
        g_spu_trampoline_fn=NULL;cull_setup_scope(b);
        assert(fpcr()==host && watches==10);
        if(memcmp(a,b,sizeof(*a))||g_spu_trampoline_fn!=expected) {
            fprintf(stderr,"Setup mismatch iteration %u\n",i);return 1;
        }
        setup_branches[b->pc==0x8428]++;
        g_spu_ls_watch_n=0;
    }
    assert(setup_branches[0]&&setup_branches[1]);
    for(unsigned mode=0;mode<4;++mode) {
        for(unsigned i=0;i<1000;++i) {
            for(unsigned k=0;k<16;++k)put_be(ls+4*k,value(1));
            for(unsigned k=0;k<16;++k)ls[0x16370+k]=k;
            a->gpr[3]._u32[0]=0;a->gpr[5]._u32[0]=i&1;memcpy(b,a,sizeof(*a));
            g_spu_trampoline_fn=NULL;cull_setup_reference(a);void(*expected)(spu_context*)=g_spu_trampoline_fn;
            expected_fpcr=(host&~0xc00000ull)|((uint64_t)mode<<22);
            __asm__ volatile("msr fpcr, %0"::"r"(expected_fpcr):"memory");
            watches=0;g_spu_ls_watch_n=1;g_spu_trampoline_fn=NULL;cull_setup_scope(b);
            assert(watches==5 && fpcr()==expected_fpcr && !memcmp(a,b,sizeof(*a))&&g_spu_trampoline_fn==expected);
            g_spu_ls_watch_n=0;
            __asm__ volatile("msr fpcr, %0"::"r"(host):"memory");
            expected_fpcr=host;
        }
    }
    for(unsigned reg=0;reg<128;reg++)for(unsigned lane=0;lane<4;lane++)a->gpr[reg]._u32[lane]=value(1);
    a->host_depth=1;memcpy(b,a,sizeof(*a));memcpy(c,a,sizeof(*a));
    for(unsigned k=0;k<SPU_LS_SIZE/4;k++)put_be(ls+4*k,value(1));
    const unsigned n=1000000;
    double start=now();for(unsigned i=0;i<n;i++)cull_reference(a);double reference=now()-start;
    start=now();for(unsigned i=0;i<n;i++)cull_scope(b);double scoped=now()-start;
    start=now();for(unsigned i=0;i<n;i++)cull_bounded(c);double bounded=now()-start;
    for(unsigned k=0;k<16;++k)ls[0x16370+k]=k;
    start=now();for(unsigned i=0;i<n;++i){a->gpr[3]._u32[0]=0;a->gpr[5]._u32[0]=1;cull_setup_reference(a);}double setup_reference=now()-start;
    start=now();for(unsigned i=0;i<n;++i){b->gpr[3]._u32[0]=0;b->gpr[5]._u32[0]=1;cull_setup_scope(b);}double setup_scope=now()-start;
    assert(fpcr()==host);
    printf("PASS: 300000 full-context/branch comparisons (%u eligible), 4 million comparison lanes, input boundary checks, 40000 alternate FPCR/callback checks, 6000 profiled comparisons and sampling/unwind checks; reference %.2f ns, scoped %.2f ns, bounded %.2f ns (microbenchmark only)\n",
        eligible,reference*1e9/n,scoped*1e9/n,bounded*1e9/n);
    printf("PASS: 100000 setup full-context/branch comparisons with callbacks, 4000 setup alternate FPCR checks; setup reference %.2f ns, scoped %.2f ns (microbenchmark only)\n",
        setup_reference*1e9/n,setup_scope*1e9/n);
    free(ls);free(a);free(b);free(c);free(input);return 0;
}
