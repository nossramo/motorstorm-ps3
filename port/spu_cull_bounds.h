#pragma once
#include "spu_float_scope.h"

/* Only for the signature-checked 0x8480 block:
 *
 * Each dot product is FM(input,input), two FMA(input,input,accumulator),
 * then FA(accumulator,input). Inputs accepted here are +0 or normal
 * singles with 2^-30 <= abs(x) < 2^30. Their exact products are multiples
 * of 2^-106, below 2^60 in magnitude. Each accumulated sum stays below
 * 2^62. Truncating any intermediate to 24 significand bits preserves that
 * 2^-106 lattice: cancellation cannot produce a nonzero subnormal.
 *
 * Consequently no operation can reach extended SPU range or underflow.
 * Keep hardware RTZ, each intermediate rounding, and positive SPU zero.
 * Do not use these helpers for other operation graphs or unchecked inputs.
 * FPCR is still restored at every original control-flow/callback boundary.
 */
static inline int ms_cull_bounds_inputs(const spu_context* ctx)
{
    static const unsigned inputs[] = {
        9, 15, 16, 17, 18, 19, 20, 21, 22,
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41
    };
    uint32x4_t valid = vdupq_n_u32(~0u);
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        const uint32x4_t raw = vld1q_u32(ctx->gpr[inputs[i]]._u32);
        const uint32x4_t mag = vandq_u32(raw, vdupq_n_u32(0x7fffffffu));
        const uint32x4_t range = vandq_u32(vcgeq_u32(mag, vdupq_n_u32(0x30800000u)),
                                         vcltq_u32(mag, vdupq_n_u32(0x4e800000u)));
        valid = vandq_u32(valid, vorrq_u32(range, vceqq_u32(raw, vdupq_n_u32(0))));
    }
    return vminvq_u32(valid) != 0;
}

/* SPU comparison is an integer ordering of signed magnitudes, with every
 * exponent-zero encoding equal to zero. This also covers exponent 255;
 * using an IEEE floating compare would change those inputs' semantics. */
static inline int32x4_t ms_cull_compare_key(u128 value)
{
    const uint32x4_t bits = vld1q_u32(value._u32);
    const uint32x4_t nonzero = vcgtq_u32(vandq_u32(bits, vdupq_n_u32(0x7f800000u)),
                                       vdupq_n_u32(0));
    const int32x4_t magnitude = vreinterpretq_s32_u32(
        vandq_u32(vandq_u32(bits, vdupq_n_u32(0x7fffffffu)), nonzero));
    const int32x4_t sign = vshrq_n_s32(vreinterpretq_s32_u32(bits), 31);
    return vsubq_s32(veorq_s32(magnitude, sign), sign);
}
static inline u128 ms_cull_fcgt(u128 a, u128 b)
{
    return spu__nv_u128(vcgtq_s32(ms_cull_compare_key(a), ms_cull_compare_key(b)));
}

#define MS_CULL_BOUNDED_BINARY(NAME, INSTRUCTION) \
static inline __attribute__((always_inline)) u128 NAME(u128 a, u128 b, uint64_t old) { \
    (void)old; \
    uint32x4_t x=vld1q_u32(a._u32), y=vld1q_u32(b._u32), r; \
    __asm__ volatile(INSTRUCTION " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(x),"w"(y)); \
    return spu__nv_u128(ms_rtz_normalize(r)); \
}
MS_CULL_BOUNDED_BINARY(ms_cull_bounded_fm, "fmul")
MS_CULL_BOUNDED_BINARY(ms_cull_bounded_fa, "fadd")
#undef MS_CULL_BOUNDED_BINARY
static inline __attribute__((always_inline)) u128 ms_cull_bounded_fma(
    u128 a, u128 b, u128 c, uint64_t old)
{
    (void)old;
    uint32x4_t x=vld1q_u32(a._u32), y=vld1q_u32(b._u32), r=vld1q_u32(c._u32);
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(r) : "w"(x),"w"(y));
    return spu__nv_u128(ms_rtz_normalize(r));
}
