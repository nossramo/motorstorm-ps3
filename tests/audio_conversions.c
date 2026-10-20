#define MS_AUDIO_CONVERSIONS_NO_REPLACE
#include "spu_audio_conversions.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

#if !defined(__aarch64__) || !defined(__ARM_NEON)
#error This differential test requires ARM64 NEON
#endif
static uint32_t state = 0x12345678;
static uint32_t random_word(void) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state;
}
static void equal(u128 actual, u128 reference, const char* op, u128 input, int scale) {
    if (!memcmp(&actual, &reference, sizeof actual)) return;
    for (unsigned i=0; i<4; ++i)
        fprintf(stderr, "%s scale=%d input=%08x actual=%08x expected=%08x\n",
                op, scale, input._u32[i], actual._u32[i], reference._u32[i]);
    abort();
}
static void check(u128 x, int i8) {
    equal(ms_audio_cflts(x,i8),spu_cflts(x,i8),"cflts",x,i8);
    equal(ms_audio_cfltu(x,i8),spu_cfltu(x,i8),"cfltu",x,i8);
    equal(ms_audio_csflt(x,i8),spu_csflt(x,i8),"csflt",x,i8);
    equal(ms_audio_cuflt(x,i8),spu_cuflt(x,i8),"cuflt",x,i8);
}
static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static volatile uint32_t sink;
static void bench(int native) {
    u128 x={{0}}, result={{0}}; const double start=now();
    for (uint32_t i=0; i<2000000; ++i) {
        for (unsigned lane=0; lane<4; ++lane) x._u32[lane]=i*2654435761u+lane*1013904223u;
        u128 a = native ? ms_audio_csflt(x,124) : spu_csflt(x,124);
        u128 b = native ? ms_audio_cflts(a,142) : spu_cflts(a,142);
        for (unsigned lane=0; lane<4; ++lane) result._u32[lane]^=b._u32[lane];
    }
    sink=result._u32[0];
    printf("%s conversions: %.3f ms, checksum %08x\n",native?"NEON":"scalar",(now()-start)*1000,sink);
}
int main(void) {
    /* Every exponent, both signs, representative mantissas, every scale. */
    const uint32_t mantissas[]={0,1,0x3fffff,0x400000,0x7ffffe,0x7fffff};
    for(int scale=0; scale<256; ++scale) {
        for(unsigned exponent=0; exponent<256; ++exponent)
            for(unsigned m=0; m<sizeof mantissas/sizeof mantissas[0]; ++m) {
                const uint32_t bits=(exponent<<23)|mantissas[m];
                u128 x={{0}}; x._u32[0]=bits; x._u32[1]=bits|0x80000000;
                x._u32[2]=bits+1; x._u32[3]=~bits; check(x,scale);
            }
        /* Integer powers, signed limits and truncation boundaries. */
        for(unsigned bit=0; bit<32; ++bit) {
            u128 x={{0}}; uint32_t p=1u<<bit;
            x._u32[0]=p; x._u32[1]=p-1; x._u32[2]=p+1; x._u32[3]=0u-p; check(x,scale);
        }
        for(unsigned n=0; n<16384; ++n) {
            u128 x; for(unsigned lane=0; lane<4; ++lane) x._u32[lane]=random_word(); check(x,scale);
        }
    }
    puts("PASS: four exact conversions, all 256 scales, >18 million input lanes each");
    bench(0); bench(1);
}
