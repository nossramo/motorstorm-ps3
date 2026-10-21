#pragma once
/* Exact SPU float helpers for a host thread whose FPCR is already
 * round-toward-zero. Ordinary singles use one NEON instruction; rare
 * extended/overflow lanes use the integer SPU implementation. These are
 * not IEEE round-to-nearest approximations.
 *
 * Bind the rounding mode only around lifted/interpreted SPU execution
 * (see ms_spu_exec_enter). Do not leave the thread in this mode across
 * unrelated host floating-point work. */
#include "spu_helpers.h"
#if defined(__aarch64__) && defined(__ARM_NEON)

#ifdef __cplusplus
extern "C" {
#endif
void ms_spu_exec_enter(void);
void ms_spu_exec_leave(void);
#ifdef __cplusplus
}
#endif

static __attribute__((noinline, cold)) uint32x4_t ms_thread_fix(
    uint32x4_t result, uint32x4_t valid, u128 a, u128 b, u128 c, unsigned op)
{
    uint32_t r[4], ok[4];
    vst1q_u32(r, result);
    vst1q_u32(ok, valid);
    for (unsigned i = 0; i < 4; i++) if (!ok[i])
        r[i] = op == 0 ? spu__fa_lane(a._u32[i], b._u32[i]) :
               op == 1 ? spu__fm_lane(a._u32[i], b._u32[i]) :
               op == 2 ? spu__fma_lane(a._u32[i], b._u32[i], c._u32[i], 0, 0) :
               op == 3 ? spu__fma_lane(a._u32[i], b._u32[i], c._u32[i], 0, 0x80000000u) :
                         spu__fma_lane(a._u32[i], b._u32[i], c._u32[i], 0x80000000u, 0);
    return vld1q_u32(r);
}

static inline __attribute__((always_inline)) uint32x4_t ms_thread_normalize(uint32x4_t x)
{
    return vandq_u32(x, vcgeq_u32(vshlq_n_u32(x, 1), vdupq_n_u32(0x01000000u)));
}

#define MS_THREAD_BINARY(NAME, INSTRUCTION, OP) \
static inline __attribute__((always_inline)) u128 NAME(u128 a, u128 b) { \
    uint32x4_t x = vld1q_u32(a._u32), y = vld1q_u32(b._u32), r; \
    uint32x4_t valid = vcltq_u32(vmaxq_u32(vshlq_n_u32(x, 1), vshlq_n_u32(y, 1)), \
                                vdupq_n_u32(0xff000000u)); \
    x = ms_thread_normalize(x); y = ms_thread_normalize(y); \
    __asm__ volatile(INSTRUCTION " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(x), "w"(y)); \
    valid = vandq_u32(valid, vcltq_u32(vshlq_n_u32(r, 1), vdupq_n_u32(0xfefffffeu))); \
    r = ms_thread_normalize(r); \
    if (!spu__nv_all4(valid)) r = ms_thread_fix(r, valid, a, b, a, OP); \
    return spu__nv_u128(r); \
}
MS_THREAD_BINARY(ms_thread_fa, "fadd", 0)
MS_THREAD_BINARY(ms_thread_fm, "fmul", 1)
#undef MS_THREAD_BINARY

static inline __attribute__((always_inline)) u128 ms_thread_fs(u128 a, u128 b)
{
    return ms_thread_fa(a, spu__nv_u128(veorq_u32(vld1q_u32(b._u32), vdupq_n_u32(0x80000000u))));
}

#define MS_THREAD_MADD(NAME, NP, NC, OP) \
static inline __attribute__((always_inline)) u128 NAME(u128 a, u128 b, u128 c) { \
    uint32x4_t x = vld1q_u32(a._u32), y = vld1q_u32(b._u32), r = vld1q_u32(c._u32); \
    uint32x4_t mag = vmaxq_u32(vmaxq_u32(vshlq_n_u32(x, 1), vshlq_n_u32(y, 1)), vshlq_n_u32(r, 1)); \
    uint32x4_t valid = vcltq_u32(mag, vdupq_n_u32(0xff000000u)); \
    x = veorq_u32(ms_thread_normalize(x), vdupq_n_u32(NP)); \
    y = ms_thread_normalize(y); \
    r = veorq_u32(ms_thread_normalize(r), vdupq_n_u32(NC)); \
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(r) : "w"(x), "w"(y)); \
    valid = vandq_u32(valid, vcltq_u32(vshlq_n_u32(r, 1), vdupq_n_u32(0xfefffffeu))); \
    r = ms_thread_normalize(r); \
    if (!spu__nv_all4(valid)) r = ms_thread_fix(r, valid, a, b, c, OP); \
    return spu__nv_u128(r); \
}
MS_THREAD_MADD(ms_thread_fma, 0, 0, 2)
MS_THREAD_MADD(ms_thread_fms, 0, 0x80000000u, 3)
MS_THREAD_MADD(ms_thread_fnms, 0x80000000u, 0, 4)
#undef MS_THREAD_MADD

#if defined(MOTORSTORM_SPU_THREAD_RTZ) && !defined(MOTORSTORM_SPU_THREAD_RTZ_NO_MACROS)
#define spu_fa ms_thread_fa
#define spu_fs ms_thread_fs
#define spu_fm ms_thread_fm
#define spu_fma ms_thread_fma
#define spu_fms ms_thread_fms
#define spu_fnms ms_thread_fnms
#endif
#endif
