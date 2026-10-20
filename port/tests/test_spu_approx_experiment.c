/* This tests the experiment's stated IEEE rounding contract, not SPU identity. */
#include "../spu_float_approx_experiment.h"
#include <assert.h>
#include <stdio.h>
static uint32_t seed = 0x781256ab;
static uint32_t next(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static uint32_t bits(float f) { uint32_t u; memcpy(&u,&f,4); return u; }
static float fp(uint32_t u) { float f; memcpy(&f,&u,4); return f; }
int main(void) {
    for (int k=0; k<100000; ++k) {
        u128 a,b,c,r;
        for(int i=0;i<4;++i) {
            a._u32[i]=(next()&0x807fffff)|((110+next()%35)<<23);
            b._u32[i]=(next()&0x807fffff)|((110+next()%35)<<23);
            c._u32[i]=(next()&0x807fffff)|((110+next()%35)<<23);
        }
        for(int op=0;op<6;++op) {
            if(op==0)r=spu_fa(a,b);else if(op==1)r=spu_fs(a,b);else if(op==2)r=spu_fm(a,b);
            else if(op==3)r=spu_fma(a,b,c);else if(op==4)r=spu_fms(a,b,c);else r=spu_fnms(a,b,c);
            for(int i=0;i<4;++i) {
                float x=fp(a._u32[i]),y=fp(b._u32[i]),z=fp(c._u32[i]);
                float f=op==0?x+y:op==1?x-y:op==2?x*y:op==3?fmaf(x,y,z):op==4?fmaf(x,y,-z):fmaf(-x,y,z);
                uint32_t want=bits(f); if(!(want&0x7f800000))want=0;
                assert(r._u32[i]==want);
            }
        }
    }
    u128 a=spu_zero(), b=spu_zero(), c=spu_zero();
    for(int i=0;i<4;++i){a._u32[i]=0x7fffffffu;b._u32[i]=0x3f800000;c._u32[i]=0;}
    u128 got=spu_fma(a,b,c), ref=spu_fma_ref(a,b,c);
    assert(!memcmp(&got,&ref,16));
    for(int i=0;i<4;++i){a._u32[i]=0x3f800000;b._u32[i]=0x3f800000;c._u32[i]=0xa1800000;}
    got=spu_fma(a,b,c);ref=spu_fma_ref(a,b,c);
    assert(got._u32[0]==0x3f800000 && ref._u32[0]==0x3f7fffff);
    puts("PASS: 2.4M ordinary lanes match IEEE; extended input falls back; rounding difference is explicit");
}
