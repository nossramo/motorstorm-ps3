#pragma once
#include "spu_cull_bounds.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

/* Only used inside signature-checked arithmetic-only basic-block segments.
 * Never carry FPCR across a load/store, branch, runtime or guest call. Keep
 * the extended-range fallback; these are not fast-math approximations. */
/* Rare lanes use the integer SPU implementation directly. Changing FPCR to
 * call the widened-double fallback for all four lanes was slower on captured
 * collision/refinement inputs. Integer fallback is independent of FPCR and
 * leaves the other three NEON lanes intact. */
static __attribute__((noinline,cold)) uint32x4_t ms_job_fix(
    uint32x4_t result, uint32x4_t valid, u128 a, u128 b, u128 c, unsigned op)
{
    uint32_t r[4], ok[4];vst1q_u32(r,result);vst1q_u32(ok,valid);
    for(unsigned i=0;i<4;i++) if(!ok[i])
        r[i] = op==0 ? spu__fa_lane(a._u32[i],b._u32[i]) :
               op==1 ? spu__fm_lane(a._u32[i],b._u32[i]) :
                       spu__fma_lane(a._u32[i],b._u32[i],c._u32[i],0,0);
    return vld1q_u32(r);
}
static inline __attribute__((always_inline)) uint32x4_t ms_job_normalize(uint32x4_t x)
{
    return vandq_u32(x,vcgeq_u32(vshlq_n_u32(x,1),vdupq_n_u32(0x01000000u)));
}
#define MS_JOB_BINARY(NAME, INSTRUCTION, OP) \
static inline __attribute__((always_inline)) u128 NAME(u128 a,u128 b,uint64_t old) { \
    (void)old; \
    uint32x4_t x=vld1q_u32(a._u32),y=vld1q_u32(b._u32),r; \
    uint32x4_t valid=vcltq_u32(vmaxq_u32(vshlq_n_u32(x,1),vshlq_n_u32(y,1)),vdupq_n_u32(0xff000000u)); \
    x=ms_job_normalize(x);y=ms_job_normalize(y); \
    __asm__ volatile(INSTRUCTION " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(x),"w"(y)); \
    valid=vandq_u32(valid,vcltq_u32(vshlq_n_u32(r,1),vdupq_n_u32(0xfefffffeu))); \
    r=ms_job_normalize(r); \
    if(!spu__nv_all4(valid))r=ms_job_fix(r,valid,a,b,a,OP); \
    return spu__nv_u128(r); \
}
MS_JOB_BINARY(ms_job_fa,"fadd",0)
MS_JOB_BINARY(ms_job_fm,"fmul",1)
#undef MS_JOB_BINARY
static inline u128 ms_job_fs(u128 a,u128 b,uint64_t old)
{
    return ms_job_fa(a,spu__nv_u128(veorq_u32(vld1q_u32(b._u32),vdupq_n_u32(0x80000000u))),old);
}
static inline __attribute__((always_inline)) u128 ms_job_fma(u128 a,u128 b,u128 c,uint64_t old)
{
    (void)old;
    uint32x4_t x=vld1q_u32(a._u32),y=vld1q_u32(b._u32),r=vld1q_u32(c._u32);
    uint32x4_t mag=vmaxq_u32(vmaxq_u32(vshlq_n_u32(x,1),vshlq_n_u32(y,1)),vshlq_n_u32(r,1));
    uint32x4_t valid=vcltq_u32(mag,vdupq_n_u32(0xff000000u));
    x=ms_job_normalize(x);y=ms_job_normalize(y);r=ms_job_normalize(r);
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(r) : "w"(x),"w"(y));
    valid=vandq_u32(valid,vcltq_u32(vshlq_n_u32(r,1),vdupq_n_u32(0xfefffffeu)));
    r=ms_job_normalize(r);
    if(!spu__nv_all4(valid))r=ms_job_fix(r,valid,a,b,c,2);
    return spu__nv_u128(r);
}
static inline u128 ms_job_fnms(u128 a, u128 b, u128 c, uint64_t old)
{
    return ms_job_fma(spu__nv_u128(veorq_u32(vld1q_u32(a._u32),
                         vdupq_n_u32(0x80000000u))), b, c, old);
}
/* Same 33-bit fixed-point interpolation as spu_fi, in two NEON u64 pairs.
 * No reciprocal approximation, floating point or table change. */
static inline __attribute__((always_inline)) uint32x2_t ms_job_fi_pair(uint32x2_t a,uint32x2_t b)
{
    uint32x2_t exponent=vand_u32(vshr_n_u32(b,23),vdup_n_u32(255));
    uint32x2_t base=vorr_u32(vand_u32(vshr_n_u32(b,10),vdup_n_u32(0x1fff)),vdup_n_u32(0x2000));
    uint64x2_t magnitude=vsubq_u64(vshlq_n_u64(vmovl_u32(base),19),
        vmull_u32(vand_u32(b,vdup_n_u32(0x3ff)),vand_u32(a,vdup_n_u32(0x7ffff))));
    uint64x2_t shift=vandq_u64(vcltq_u64(magnitude,vdupq_n_u64(1ull<<32)),vdupq_n_u64(1));
    magnitude=vshlq_u64(magnitude,vreinterpretq_s64_u64(shift));
    uint32x2_t adjusted=vsub_u32(exponent,vmovn_u64(shift));
    uint32x2_t result=vorr_u32(vand_u32(b,vdup_n_u32(0x80000000)),
        vorr_u32(vshl_n_u32(adjusted,23),vand_u32(vmovn_u64(vshrq_n_u64(magnitude,9)),vdup_n_u32(0x7fffff))));
    return vand_u32(result,vand_u32(vcgt_u32(exponent,vdup_n_u32(0)),vcgt_u32(adjusted,vdup_n_u32(0))));
}
static inline __attribute__((always_inline)) u128 ms_job_fi(u128 a,u128 b)
{
    uint32x4_t x=vld1q_u32(a._u32),y=vld1q_u32(b._u32);
    return spu__nv_u128(vcombine_u32(ms_job_fi_pair(vget_low_u32(x),vget_low_u32(y)),
                                    ms_job_fi_pair(vget_high_u32(x),vget_high_u32(y))));
}
static inline unsigned ms_job_scope_settings(void)
{
    static atomic_uint enabled = 0xffffffffu;
    unsigned value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value == 0xffffffffu) {
        const char* setting = getenv("MOTORSTORM_JOB_SCOPES");
        value = setting ? (unsigned)strtoul(setting, NULL, 0) & 0xffu : 0;
        if (getenv("MOTORSTORM_JOB_SCOPE_CAPTURE")) value |= 0x80000000u;
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value;
}

/* Opt-in diagnostic only. Snapshot the register inputs to pure segments,
 * never host pointers or game memory. First four and every 1024th entry,
 * bounded to 32 snapshots per segment across all worker threads. */
static void ms_job_scope_capture(const spu_context* ctx, unsigned index)
{
    static atomic_uint calls[8], saved[8];
    unsigned call = atomic_fetch_add_explicit(&calls[index], 1, memory_order_relaxed);
    if (atomic_load_explicit(&saved[index], memory_order_relaxed) >= 32) return;
    if (call >= 4 && (call & 1023u)) return;
    unsigned slot = atomic_fetch_add_explicit(&saved[index], 1, memory_order_relaxed);
    if (slot >= 32) return;
    const char* dir = getenv("MOTORSTORM_JOB_SCOPE_CAPTURE");
    if (!dir) return;
    char path[4096];
    snprintf(path, sizeof(path), "%s/segment-%u-%02u.bin", dir, index, slot);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(ctx->gpr, sizeof(ctx->gpr), 1, f); fclose(f); }
}
