#pragma once
/* Exact integer-only NEON conversions for the measured Multistream worker.
 * Keep SPU extended floats, truncation, saturation and denormal flushing.
 * No host floating-point mode changes; scalar helpers remain the reference.
 */
#include "spu_helpers.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
static inline u128 ms_audio_cfl(u128 a, int i8, int is_unsigned) {
    const uint32x4_t x = vld1q_u32(a._u32);
    const uint32x4_t exponent = vandq_u32(vshrq_n_u32(x, 23), vdupq_n_u32(255));
    const uint32x4_t negative = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(x), 31));
    const uint32x4_t mantissa = vorrq_u32(vandq_u32(x, vdupq_n_u32(0x7fffff)), vdupq_n_u32(0x800000));
    const int32x4_t shift = vaddq_s32(vreinterpretq_s32_u32(exponent), vdupq_n_s32(23 - (i8 & 255)));
    /* VSHL uses the low signed byte; clamp before it, then handle saturation. */
    const int32x4_t safe_shift = vmaxq_s32(vdupq_n_s32(-32), vminq_s32(shift, vdupq_n_s32(31)));
    uint32x4_t result = vshlq_u32(mantissa, safe_shift);
    if (is_unsigned) {
        result = vbslq_u32(vcgeq_s32(shift, vdupq_n_s32(9)), vdupq_n_u32(0xffffffff), result);
        result = vbicq_u32(result, negative);
    } else {
        result = vsubq_u32(veorq_u32(result, negative), negative);
        const uint32x4_t limit = veorq_u32(vdupq_n_u32(0x7fffffff), negative);
        result = vbslq_u32(vcgeq_s32(shift, vdupq_n_s32(8)), limit, result);
    }
    return spu__nv_u128(vandq_u32(result, vcgtq_u32(exponent, vdupq_n_u32(0))));
}
static inline u128 ms_audio_cflts(u128 a, int i8) { return ms_audio_cfl(a, i8, 0); }
static inline u128 ms_audio_cfltu(u128 a, int i8) { return ms_audio_cfl(a, i8, 1); }

static inline u128 ms_audio_cxf(u128 a, int i8, int is_unsigned) {
    const uint32x4_t x = vld1q_u32(a._u32);
    const uint32x4_t negative = is_unsigned ? vdupq_n_u32(0) :
        vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(x), 31));
    const uint32x4_t magnitude = vsubq_u32(veorq_u32(x, negative), negative);
    const int32x4_t top = vsubq_s32(vdupq_n_s32(31), vreinterpretq_s32_u32(vclzq_u32(magnitude)));
    const int32x4_t exponent = vaddq_s32(top, vdupq_n_s32((i8 & 255) - 28));
    const uint32x4_t mantissa = vshlq_u32(magnitude, vsubq_s32(vdupq_n_s32(23), top));
    uint32x4_t result = vorrq_u32(vshlq_n_u32(vreinterpretq_u32_s32(exponent), 23),
                                vandq_u32(mantissa, vdupq_n_u32(0x7fffff)));
    result = vbslq_u32(vcgtq_s32(exponent, vdupq_n_s32(255)), vdupq_n_u32(0x7fffffff), result);
    result = vorrq_u32(result, vandq_u32(negative, vdupq_n_u32(0x80000000)));
    result = vandq_u32(result, vandq_u32(vcgtq_u32(magnitude, vdupq_n_u32(0)),
                                       vcgtq_s32(exponent, vdupq_n_s32(0))));
    return spu__nv_u128(result);
}
static inline u128 ms_audio_csflt(u128 a, int i8) { return ms_audio_cxf(a, i8, 0); }
static inline u128 ms_audio_cuflt(u128 a, int i8) { return ms_audio_cxf(a, i8, 1); }
#ifndef MS_AUDIO_CONVERSIONS_NO_REPLACE
#define spu_cflts ms_audio_cflts
#define spu_cfltu ms_audio_cfltu
#define spu_csflt ms_audio_csflt
#define spu_cuflt ms_audio_cuflt
#endif
#endif
