#define MOTORSTORM_VMX_NEON_TEST
#include "ppu_vmx_neon.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <time.h>

static uint32_t rng = 0x213821u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
__attribute__((noinline)) static void reference(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ vmx_vmaddfp(d,a,b,c,v); }
__attribute__((noinline)) static void specialized(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ ms_vmx_vmaddfp(d,a,b,c,v); }
__attribute__((noinline)) static void reference_add(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ (void)c; vmx_vaddfp(d,a,b,v); }
__attribute__((noinline)) static void specialized_add(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ (void)c; ms_vmx_vaddfp(d,a,b,v); }
__attribute__((noinline)) static void reference_sub(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ (void)c; vmx_vsubfp(d,a,b,v); }
__attribute__((noinline)) static void specialized_sub(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ (void)c; ms_vmx_vsubfp(d,a,b,v); }
__attribute__((noinline)) static void reference_nmsub(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ vmx_vnmsubfp(d,a,b,c,v); }
__attribute__((noinline)) static void specialized_nmsub(void* d,const void* a,const void* b,const void* c,uint32_t v)
{ ms_vmx_vnmsubfp(d,a,b,c,v); }
typedef void (*operation)(void*,const void*,const void*,const void*,uint32_t);
static const struct { const char* name; operation ref, neon; } ops[] = {
    {"madd",reference,specialized}, {"add",reference_add,specialized_add},
    {"sub",reference_sub,specialized_sub}, {"nmsub",reference_nmsub,specialized_nmsub}
};
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

int main(void)
{
    uint8_t values[3][16], expected[16], actual[16], aliased[3][16];
    const uint32_t edges[] = {0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,
        0x00800000,0x80800000,0x3f800000,0xbf800000,0x3f800001,0x7f7fffff,
        0xff7fffff,0x7f800000,0xff800000,0x7f800001,0xff800001,0x7fc12345,0xffc12345};
    const int modes[] = {FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD};
    unsigned long long tested = 0;
    const int old = fegetround();
    for (unsigned mode=0; mode<4; ++mode) {
        assert(fesetround(modes[mode]) == 0);
        for (unsigned trial=0; trial<300000; ++trial) {
            for (unsigned v=0; v<3; ++v) for (unsigned lane=0; lane<4; ++lane) {
                uint32_t bits = next();
                if (trial < 20000) bits = edges[bits % (sizeof edges/sizeof edges[0])];
                vmx_sw(values[v], lane, bits);
            }
            for (unsigned flush=0; flush<2; ++flush) {
                uint32_t vscr = flush ? VMX_NJ : 0;
              for (unsigned op=0; op<sizeof ops/sizeof ops[0]; ++op) {
                ops[op].ref(expected,values[0],values[1],values[2],vscr);
                ops[op].neon(actual,values[0],values[1],values[2],vscr);
                if (memcmp(expected,actual,16)) { fprintf(stderr,"mismatch op=%s mode=%u trial=%u NJ=%u\n",ops[op].name,mode,trial,flush); return 1; }
                if (trial % 17 == 0) for (unsigned alias=0; alias<3; ++alias) {
                    memcpy(aliased,values,sizeof values);
                    ops[op].neon(aliased[alias],aliased[0],aliased[1],aliased[2],vscr);
                    assert(memcmp(expected,aliased[alias],16) == 0);
                }
                tested += 4;
              }
            }
        }
    }
    assert(fesetround(old) == 0);
    printf("PASS NEON VMX: %llu lanes, 4 rounding modes, NJ, exceptional values, source aliases\n",tested);
    for (unsigned i=0;i<4;++i) { vmx_sw(values[0],i,0x3f7ffffe); vmx_sw(values[1],i,0x33800000); vmx_sw(values[2],i,0x3f7ffffd); }
    const unsigned iterations=4000000;
    for (unsigned op=0;op<sizeof ops/sizeof ops[0];++op) for (unsigned mode=0;mode<2;++mode) {
        double begin=now();
        for (unsigned i=0;i<iterations;++i) (mode ? ops[op].neon : ops[op].ref)(actual,values[0],values[1],values[2],VMX_NJ);
        printf("%s %s %.2f ns/vector checksum=%08x\n", ops[op].name, mode ? "NEON" : "scalar", (now()-begin)*1e9/iterations, vmx_w(actual,0));
    }
    return 0;
}
