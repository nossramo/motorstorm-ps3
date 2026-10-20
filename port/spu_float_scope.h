#pragma once
/* An opt-in, bounded arithmetic scope for the pure WWS culling block 0x8480.
 * Every hardware float op is explicit asm, so it cannot move across FPCR
 * changes. Extended-range cases restore host mode around the exact helpers.
 * Do not keep this scope active across guest calls, DMA or runtime callbacks. */
#include "spu_helpers.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
static inline __attribute__((always_inline)) uint64_t ms_rtz_enter(void) {
    uint64_t old, mode;
    __asm__ volatile("mrs %0, fpcr\n\torr %1, %0, #0xc00000\n\tmsr fpcr, %1"
                     : "=&r"(old), "=&r"(mode) :: "memory");
    return old;
}
static inline __attribute__((always_inline)) void ms_rtz_leave(uint64_t old) {
    __asm__ volatile("msr fpcr, %0" :: "r"(old) : "memory");
}
static inline __attribute__((always_inline)) uint32x4_t ms_rtz_normalize(uint32x4_t v) {
    return vandq_u32(v, vcgtq_u32(spu__nv_bexp(v), vdupq_n_u32(0)));
}
static inline __attribute__((always_inline)) int ms_rtz_extended(uint32x4_t v) {
    return vmaxvq_u32(spu__nv_bexp(v)) == 255;
}
static inline __attribute__((always_inline)) int ms_rtz_overflow(uint32x4_t v) {
    return vmaxvq_u32(vandq_u32(v, vdupq_n_u32(0x7fffffff))) >= 0x7f7fffff;
}
static __attribute__((noinline,cold)) u128 ms_rtz_fallback(u128 a,u128 b,u128 c,
                                                         unsigned op,uint64_t old) {
    ms_rtz_leave(old);
    u128 result = op == 0 ? spu_fa(a,b) : op == 1 ? spu_fm(a,b) : spu_fma(a,b,c);
    (void)ms_rtz_enter();
    return result;
}
#define MS_RTZ_BINARY(NAME, INSTRUCTION, OP) \
static inline __attribute__((always_inline)) u128 NAME(u128 a,u128 b,uint64_t old) { \
    uint32x4_t x=vld1q_u32(a._u32),y=vld1q_u32(b._u32),r; \
    if(ms_rtz_extended(x)||ms_rtz_extended(y))return ms_rtz_fallback(a,b,a,OP,old); \
    x=ms_rtz_normalize(x);y=ms_rtz_normalize(y); \
    __asm__ volatile(INSTRUCTION " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(x),"w"(y)); \
    if(ms_rtz_overflow(r))return ms_rtz_fallback(a,b,a,OP,old); \
    return spu__nv_u128(ms_rtz_normalize(r)); \
}
MS_RTZ_BINARY(ms_rtz_fa,"fadd",0)
MS_RTZ_BINARY(ms_rtz_fm,"fmul",1)
#undef MS_RTZ_BINARY
static inline __attribute__((always_inline)) u128 ms_rtz_fs(u128 a,u128 b,uint64_t old) {
    const uint32x4_t negative = veorq_u32(vld1q_u32(b._u32), vdupq_n_u32(0x80000000u));
    return ms_rtz_fa(a, spu__nv_u128(negative), old);
}
static inline __attribute__((always_inline)) u128 ms_rtz_fma(u128 a,u128 b,u128 c,uint64_t old) {
    uint32x4_t x=vld1q_u32(a._u32),y=vld1q_u32(b._u32),r=vld1q_u32(c._u32);
    if(ms_rtz_extended(x)||ms_rtz_extended(y)||ms_rtz_extended(r))return ms_rtz_fallback(a,b,c,2,old);
    x=ms_rtz_normalize(x);y=ms_rtz_normalize(y);r=ms_rtz_normalize(r);
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(r) : "w"(x),"w"(y));
    if(ms_rtz_overflow(r))return ms_rtz_fallback(a,b,c,2,old);
    return spu__nv_u128(ms_rtz_normalize(r));
}
#endif
