#include "spu_thread_rtz.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include "spu_float_scope.h"

/* Launch-time A/B switch. The original integer fallback stays in the binary.
 * This is read before SPU execution, never on the arithmetic hot path. */
int ms_spu_wide_rtz = 1;
__attribute__((constructor)) static void ms_spu_wide_configure(void)
{
    const char* value = getenv("MOTORSTORM_SPU_WIDE_RTZ");
    ms_spu_wide_rtz = !value || strcmp(value, "0");
}

/* Map the SPU encoding directly into binary64. Unlike an IEEE float cast,
 * this preserves exponent 255 as finite; exponent zero is always +0.
 * Every SPU single is exactly representable in binary64. */
static inline float64x2_t ms_spu_widen(uint32x2_t input)
{
    const uint64x2_t x = vmovl_u32(input);
    const uint64x2_t mag = vandq_u64(x, vdupq_n_u64(0x7fffffffu));
    const uint64x2_t sign = vshlq_n_u64(vandq_u64(x, vdupq_n_u64(0x80000000u)), 32);
    uint64x2_t bits = vorrq_u64(sign, vaddq_u64(vshlq_n_u64(mag, 29),
                                              vdupq_n_u64(896ull << 52)));
    bits = vandq_u64(bits, vcgeq_u64(mag, vdupq_n_u64(0x00800000u)));
    return vreinterpretq_f64_u64(bits);
}

/* Truncate the significand to 24 bits, saturate the SPU extended range,
 * and flush underflow/negative zero. Do not use FCVT: exponent 255 is not
 * an IEEE single. All results here are finite binary64 values. */
static inline uint32x2_t ms_spu_narrow(float64x2_t value)
{
    const uint64x2_t bits = vreinterpretq_u64_f64(value);
    const uint64x2_t mag = vandq_u64(bits, vdupq_n_u64(0x7fffffffffffffffull));
    const uint32x2_t sign = vand_u32(vmovn_u64(vshrq_n_u64(bits, 32)),
                                   vdup_n_u32(0x80000000u));
    uint32x2_t r = vmovn_u64(vshrq_n_u64(vsubq_u64(mag, vdupq_n_u64(896ull << 52)), 29));
    r = vbsl_u32(vmovn_u64(vcgeq_u64(mag, vdupq_n_u64(1152ull << 52))),
                 vdup_n_u32(0x7fffffffu), r);
    r = vorr_u32(r, sign);
    return vand_u32(r, vmovn_u64(vcgeq_u64(mag, vdupq_n_u64(897ull << 52))));
}

/* With FPCR=RTZ, rounding first to 53 bits and then truncating to 24 bits
 * gives the same directed rounding as a single exact-to-24-bit operation.
 * FMLA keeps the product unrounded until addition. SPU operands/products
 * fit binary64's exponent range, including cancellation and exponent 255.
 * Inline asm prevents C's default-rounding assumptions from changing this.
 * The integer reference remains the independent differential-test oracle. */
uint32x4_t ms_spu_wide_fix_vector(uint32x4_t result, uint32x4_t valid,
                                uint32x4_t av, uint32x4_t bv, uint32x4_t cv, unsigned op)
{
    if (op == 3) cv = veorq_u32(cv, vdupq_n_u32(0x80000000u));
    if (op == 4) av = veorq_u32(av, vdupq_n_u32(0x80000000u));
    float64x2_t al = ms_spu_widen(vget_low_u32(av)), ah = ms_spu_widen(vget_high_u32(av));
    float64x2_t bl = ms_spu_widen(vget_low_u32(bv)), bh = ms_spu_widen(vget_high_u32(bv));
    float64x2_t rl, rh;
    if (op == 0) {
        __asm__ volatile("fadd %0.2d, %1.2d, %2.2d" : "=w"(rl) : "w"(al), "w"(bl));
        __asm__ volatile("fadd %0.2d, %1.2d, %2.2d" : "=w"(rh) : "w"(ah), "w"(bh));
    } else if (op == 1) {
        __asm__ volatile("fmul %0.2d, %1.2d, %2.2d" : "=w"(rl) : "w"(al), "w"(bl));
        __asm__ volatile("fmul %0.2d, %1.2d, %2.2d" : "=w"(rh) : "w"(ah), "w"(bh));
    } else {
        rl = ms_spu_widen(vget_low_u32(cv)); rh = ms_spu_widen(vget_high_u32(cv));
        __asm__ volatile("fmla %0.2d, %1.2d, %2.2d" : "+w"(rl) : "w"(al), "w"(bl));
        __asm__ volatile("fmla %0.2d, %1.2d, %2.2d" : "+w"(rh) : "w"(ah), "w"(bh));
    }
    return vbslq_u32(valid, result, vcombine_u32(ms_spu_narrow(rl), ms_spu_narrow(rh)));
}

static _Thread_local int ms_spu_rtz_depth;
static _Thread_local uint64_t ms_spu_rtz_saved;

void ms_spu_exec_enter(void)
{
    if (ms_spu_rtz_depth++) return;
    ms_spu_rtz_saved = ms_rtz_enter();
}

void ms_spu_exec_leave(void)
{
    if (--ms_spu_rtz_depth) return;
    ms_rtz_leave(ms_spu_rtz_saved);
}
#endif
