#pragma once
/* Exact iOS SHUFB lowering for our word-native register representation.
 * Uses ARM table lookup/extension, as in RPCS3's ARM64 lowering, but derives
 * indices for ps3recomp's byte P -> host byte P^3 layout. No FP changes.
 * Reference: RPCS3/rpcs3 PR #18056, SPULLVMRecompiler.cpp::SHUFB.
 */
#include "spu_helpers.h"

#if !defined(__aarch64__) || !defined(__ARM_NEON)
#error "The iOS SHUFB lowering requires ARM64 NEON"
#endif

static inline __attribute__((always_inline)) u128 ms_spu_shufb_neon(u128 a, u128 b, u128 c)
{
    const uint8x16_t control = vld1q_u8(c._u8);
    /* The output and control already have the same host byte positions.
     * XOR 3 converts only the selected source byte to host layout. Bits
     * 5/6 are ignored for ordinary selectors; bit 7 keeps all special
     * selectors out of the 32-byte table, so TBX preserves their constants.
     */
    const uint8x16_t index = vandq_u8(veorq_u8(control, vdupq_n_u8(3)),
                                    vdupq_n_u8(0x9f));
    const uint8x16_t constants = {0, 0, 0, 0, 0, 0, 0xff, 0x80,
                                 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8x16_t special = vqtbl1q_u8(constants, vshrq_n_u8(control, 5));
    const uint8x16x2_t sources = {{vld1q_u8(a._u8), vld1q_u8(b._u8)}};
    u128 result;
    vst1q_u8(result._u8, vqtbx2q_u8(special, sources, index));
    return result;
}

/* Include only on selected iOS SPU translation units. Keeping the original
 * helper available also gives the differential test an independent baseline.
 */
#ifndef MOTORSTORM_SHUFB_NEON_NO_MACRO
#define spu_shufb ms_spu_shufb_neon
#endif
