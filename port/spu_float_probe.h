#ifndef MOTORSTORM_SPU_FLOAT_PROBE_H
#define MOTORSTORM_SPU_FLOAT_PROBE_H
/* Measurement-only build (MOTORSTORM_SPU_FLOAT_PROBE): while
 * g_ms_spu_float_probe is set, SPU fa/fs/fm/fma/fms/fnms use hardware
 * single precision in round-to-nearest. That is NOT SPU rounding; it bounds
 * what an exact round-toward-zero implementation could save. Extended-range
 * operands and results keep the exact helpers. Never enabled in normal builds.
 */
#include "spu_helpers.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
extern volatile int g_ms_spu_float_probe;
static inline __attribute__((always_inline)) uint32x4_t ms_probe_flush(uint32x4_t v) {
    return vandq_u32(v, vcgtq_u32(spu__nv_bexp(v), vdupq_n_u32(0)));
}
static inline __attribute__((always_inline)) int ms_probe_extended(uint32x4_t v) {
    return vmaxvq_u32(spu__nv_bexp(v)) == 255;
}
#define MS_PROBE_BIN(NAME, OP, EXACT) \
static inline __attribute__((always_inline)) u128 NAME(u128 a, u128 b) { \
    if (__builtin_expect(!g_ms_spu_float_probe, 0)) return EXACT(a, b); \
    uint32x4_t x = vld1q_u32(a._u32), y = vld1q_u32(b._u32); \
    if (ms_probe_extended(x) || ms_probe_extended(y)) return EXACT(a, b); \
    uint32x4_t r = vreinterpretq_u32_f32(OP(vreinterpretq_f32_u32(ms_probe_flush(x)), \
                                          vreinterpretq_f32_u32(ms_probe_flush(y)))); \
    if (ms_probe_extended(r)) return EXACT(a, b); \
    return spu__nv_u128(ms_probe_flush(r)); \
}
MS_PROBE_BIN(ms_probe_fa, vaddq_f32, spu_fa)
MS_PROBE_BIN(ms_probe_fs, vsubq_f32, spu_fs)
MS_PROBE_BIN(ms_probe_fm, vmulq_f32, spu_fm)
#undef MS_PROBE_BIN
static inline __attribute__((always_inline)) u128 ms_probe_madd(u128 a, u128 b, u128 c,
                                                              uint32_t np, uint32_t nc) {
    if (__builtin_expect(!g_ms_spu_float_probe, 0)) return spu__nv_fma(a, b, c, np, nc);
    uint32x4_t x = vld1q_u32(a._u32), y = vld1q_u32(b._u32), z = vld1q_u32(c._u32);
    if (ms_probe_extended(x) || ms_probe_extended(y) || ms_probe_extended(z))
        return spu__nv_fma(a, b, c, np, nc);
    x = veorq_u32(ms_probe_flush(x), vdupq_n_u32(np));
    y = ms_probe_flush(y); z = veorq_u32(ms_probe_flush(z), vdupq_n_u32(nc));
    uint32x4_t r = vreinterpretq_u32_f32(vfmaq_f32(vreinterpretq_f32_u32(z),
                      vreinterpretq_f32_u32(x), vreinterpretq_f32_u32(y)));
    if (ms_probe_extended(r)) return spu__nv_fma(a, b, c, np, nc);
    return spu__nv_u128(ms_probe_flush(r));
}
#define spu_fa ms_probe_fa
#define spu_fs ms_probe_fs
#define spu_fm ms_probe_fm
#define spu_fma(a,b,c) ms_probe_madd(a,b,c,0,0)
#define spu_fms(a,b,c) ms_probe_madd(a,b,c,0,0x80000000u)
#define spu_fnms(a,b,c) ms_probe_madd(a,b,c,0x80000000u,0)
#endif
#endif
