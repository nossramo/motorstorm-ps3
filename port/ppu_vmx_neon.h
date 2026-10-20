#pragma once
/* Opt-in specializations of sampled VMX arithmetic helpers. All vectors
 * retain guest byte order, VSCR[NJ] denormal handling and the existing canonical
 * NaN result. No fast-math, approximate arithmetic or host FPCR changes. */
#include "ppu_vmx.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
static inline uint32x4_t ms_vmx_flush4(uint32x4_t bits)
{
    const uint32x4_t zero_exp = vceqq_u32(vandq_u32(bits, vdupq_n_u32(0x7f800000u)), vdupq_n_u32(0));
    return vbslq_u32(zero_exp, vandq_u32(bits, vdupq_n_u32(0x80000000u)), bits);
}
static inline uint32x4_t ms_vmx_load4(const void* p)
{
    return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8((const uint8_t*)p)));
}
static inline uint32x4_t ms_vmx_nan4(uint32x4_t bits)
{
    return vcgtq_u32(vandq_u32(bits, vdupq_n_u32(0x7fffffffu)), vdupq_n_u32(0x7f800000u));
}
static inline void ms_vmx_store4(void* p, uint32x4_t bits)
{
    vst1q_u8((uint8_t*)p, vrev32q_u8(vreinterpretq_u8_u32(bits)));
}
static inline __attribute__((always_inline)) void ms_vmx_addsubfp(
    void* destination, const void* a, const void* b, uint32_t vscr, int subtract)
{
    uint32x4_t x = ms_vmx_load4(a), y = ms_vmx_load4(b);
    if (vscr & VMX_NJ) { x = ms_vmx_flush4(x); y = ms_vmx_flush4(y); }
    const float32x4_t xf = vreinterpretq_f32_u32(x), yf = vreinterpretq_f32_u32(y);
    uint32x4_t result = vreinterpretq_u32_f32(subtract ? vsubq_f32(xf, yf) : vaddq_f32(xf, yf));
    const uint32x4_t quiet = vdupq_n_u32(0x7fc00000u);
    result = vbslq_u32(ms_vmx_nan4(result), quiet, result);
    /* Preserve the scalar helper's A-then-B NaN payload/sign precedence. */
    result = vbslq_u32(ms_vmx_nan4(y), vorrq_u32(y, quiet), result);
    result = vbslq_u32(ms_vmx_nan4(x), vorrq_u32(x, quiet), result);
    if (vscr & VMX_NJ) result = ms_vmx_flush4(result);
    ms_vmx_store4(destination, result);
}
static inline __attribute__((always_inline)) void ms_vmx_vaddfp(
    void* d, const void* a, const void* b, uint32_t vscr)
{ ms_vmx_addsubfp(d, a, b, vscr, 0); }
static inline __attribute__((always_inline)) void ms_vmx_vsubfp(
    void* d, const void* a, const void* b, uint32_t vscr)
{ ms_vmx_addsubfp(d, a, b, vscr, 1); }
static inline __attribute__((always_inline)) void ms_vmx_vmaddfp(
    void* destination, const void* a, const void* b, const void* c, uint32_t vscr)
{
    uint32x4_t x = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8((const uint8_t*)a)));
    uint32x4_t y = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8((const uint8_t*)b)));
    uint32x4_t z = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8((const uint8_t*)c)));
    if (vscr & VMX_NJ) { x = ms_vmx_flush4(x); y = ms_vmx_flush4(y); z = ms_vmx_flush4(z); }
    uint32x4_t result = vreinterpretq_u32_f32(vfmaq_f32(vreinterpretq_f32_u32(y),
        vreinterpretq_f32_u32(x), vreinterpretq_f32_u32(z)));
    const uint32x4_t nan = vcgtq_u32(vandq_u32(result, vdupq_n_u32(0x7fffffffu)), vdupq_n_u32(0x7f800000u));
    result = vbslq_u32(nan, vdupq_n_u32(0x7fc00000u), result);
    if (vscr & VMX_NJ) result = ms_vmx_flush4(result);
    vst1q_u8((uint8_t*)destination, vrev32q_u8(vreinterpretq_u8_u32(result)));
}
static inline __attribute__((always_inline)) void ms_vmx_vnmsubfp(
    void* destination, const void* a, const void* b, const void* c, uint32_t vscr)
{
    uint32x4_t x = ms_vmx_load4(a), y = ms_vmx_load4(b), z = ms_vmx_load4(c);
    if (vscr & VMX_NJ) { x = ms_vmx_flush4(x); y = ms_vmx_flush4(y); z = ms_vmx_flush4(z); }
    const uint32x4_t sign = vdupq_n_u32(0x80000000u);
    /* Exact -fma(x, z, -y); negation AFTER rounding matters in directed modes. */
    uint32x4_t result = vreinterpretq_u32_f32(vfmaq_f32(vreinterpretq_f32_u32(veorq_u32(y, sign)),
        vreinterpretq_f32_u32(x), vreinterpretq_f32_u32(z)));
    result = veorq_u32(result, sign);
    result = vbslq_u32(ms_vmx_nan4(result), vdupq_n_u32(0x7fc00000u), result);
    if (vscr & VMX_NJ) result = ms_vmx_flush4(result);
    ms_vmx_store4(destination, result);
}
#ifndef MOTORSTORM_VMX_NEON_TEST
#define vmx_vmaddfp ms_vmx_vmaddfp
#if defined(MOTORSTORM_NEON_ARITHMETIC)
#define vmx_vaddfp ms_vmx_vaddfp
#define vmx_vsubfp ms_vmx_vsubfp
#define vmx_vnmsubfp ms_vmx_vnmsubfp
#endif
#endif
#endif
