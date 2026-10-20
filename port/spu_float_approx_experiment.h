#ifndef MOTORSTORM_SPU_FLOAT_APPROX_EXPERIMENT_H
#define MOTORSTORM_SPU_FLOAT_APPROX_EXPERIMENT_H
/* Opt-in experiment for one measured hot workload. Like RPCS3's approximate
 * path, ordinary values use hardware single precision; their rounding is
 * deliberately NOT bit-identical to the SPU. Extended-range values keep the
 * existing exact implementation. This is not enabled in the normal build.
 */
#include "spu_helpers.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
static inline __attribute__((always_inline)) uint32x4_t ms_spu_flush(uint32x4_t v) {
    return vandq_u32(v, vcgtq_u32(spu__nv_bexp(v), vdupq_n_u32(0)));
}
static inline __attribute__((always_inline)) int ms_spu_extended(uint32x4_t v) {
    return vmaxvq_u32(spu__nv_bexp(v)) == 255;
}
#define MS_SPU_APPROX_BIN(NAME, OP, REFERENCE) \
static inline __attribute__((always_inline)) u128 NAME(u128 a, u128 b) { \
    uint32x4_t x = vld1q_u32(a._u32), y = vld1q_u32(b._u32); \
    if (ms_spu_extended(x) || ms_spu_extended(y)) return REFERENCE(a, b); \
    uint32x4_t r = vreinterpretq_u32_f32(OP(vreinterpretq_f32_u32(ms_spu_flush(x)), \
                                          vreinterpretq_f32_u32(ms_spu_flush(y)))); \
    if (ms_spu_extended(r)) return REFERENCE(a, b); \
    return spu__nv_u128(ms_spu_flush(r)); \
}
MS_SPU_APPROX_BIN(ms_spu_fa, vaddq_f32, spu_fa)
MS_SPU_APPROX_BIN(ms_spu_fs, vsubq_f32, spu_fs)
MS_SPU_APPROX_BIN(ms_spu_fm, vmulq_f32, spu_fm)
#undef MS_SPU_APPROX_BIN
static inline __attribute__((always_inline)) u128 ms_spu_madd(u128 a, u128 b, u128 c,
                                                            uint32_t np, uint32_t nc) {
    uint32x4_t x = vld1q_u32(a._u32), y = vld1q_u32(b._u32), z = vld1q_u32(c._u32);
    if (ms_spu_extended(x) || ms_spu_extended(y) || ms_spu_extended(z))
        return spu__nv_fma(a, b, c, np, nc);
    x = veorq_u32(ms_spu_flush(x), vdupq_n_u32(np));
    y = ms_spu_flush(y); z = veorq_u32(ms_spu_flush(z), vdupq_n_u32(nc));
    uint32x4_t r = vreinterpretq_u32_f32(vfmaq_f32(vreinterpretq_f32_u32(z),
                      vreinterpretq_f32_u32(x), vreinterpretq_f32_u32(y)));
    if (ms_spu_extended(r)) return spu__nv_fma(a, b, c, np, nc);
    return spu__nv_u128(ms_spu_flush(r));
}
#define spu_fa ms_spu_fa
#define spu_fs ms_spu_fs
#define spu_fm ms_spu_fm
#define spu_fma(a,b,c) ms_spu_madd(a,b,c,0,0)
#define spu_fms(a,b,c) ms_spu_madd(a,b,c,0,0x80000000u)
#define spu_fnms(a,b,c) ms_spu_madd(a,b,c,0x80000000u,0)
#endif
#endif
