#pragma once
/* Native streaming filter for the verified 0x32770 overlay. The DSP kernel
 * has no SPU context, register file, DMA or instruction-helper calls. Its
 * contract is big-endian input/output samples, five four-lane coefficients,
 * and four history vectors. All lanes are preserved, including lanes not
 * written to PCM: no guest ABI/dead-register assumption is required.
 *
 * Bounded windows use native singles; the general path keeps exact binary64
 * values between operations. Each multiply/FMA is rounded/flushed/saturated
 * to SPU precision before the next operation. Coefficients are decoded once
 * per block (or once per exceptional window in the bounded path).
 * FPCR must be RTZ, as established by the iOS SPU execution boundary.
 */
#include "spu_audio_filter.h"

typedef struct { uint32_t a0[4], a1[4], a2[4], b1[4], b2[4]; } ms_audio_block_coeff;
typedef struct { uint32_t x1[4], x2[4], y1[4], y2[4]; } ms_audio_block_state;

static inline int ms_audio_block_enabled(void)
{
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value < 0) {
        const char* setting = getenv("MOTORSTORM_AUDIO_BLOCK");
        value = setting && !strcmp(setting, "1");
        atomic_store_explicit(&cached, value, memory_order_relaxed);
    }
    return value;
}

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(MOTORSTORM_SPU_THREAD_RTZ)
#ifdef MOTORSTORM_AUDIO_BLOCK_TEST
static unsigned ms_ab_normal_windows, ms_ab_wide_windows, ms_ab_wide_blocks;
#define MS_AB_COUNT(NAME) (++ms_ab_##NAME)
#else
#define MS_AB_COUNT(NAME) ((void)0)
#endif
static inline float64x2_t ms_ab_decode(uint32x2_t input)
{
    uint64x2_t x = vmovl_u32(input);
    uint64x2_t mag = vandq_u64(x, vdupq_n_u64(0x7fffffffu));
    uint64x2_t sign = vshlq_n_u64(vandq_u64(x, vdupq_n_u64(0x80000000u)), 32);
    uint64x2_t bits = vorrq_u64(sign,
        vaddq_u64(vshlq_n_u64(mag, 29), vdupq_n_u64(896ull << 52)));
    return vreinterpretq_f64_u64(vandq_u64(bits,
        vcgeq_u64(mag, vdupq_n_u64(0x00800000u))));
}

static inline float64x2_t ms_ab_round(float64x2_t input)
{
    uint64x2_t bits = vreinterpretq_u64_f64(input);
    uint64x2_t mag = vandq_u64(bits, vdupq_n_u64(0x7fffffffffffffffull));
    uint64x2_t sign = vandq_u64(bits, vdupq_n_u64(0x8000000000000000ull));
    /* Largest extended SPU single: (2 - 2^-23) * 2^128. */
    mag = vbslq_u64(vcgeq_u64(mag, vdupq_n_u64(1152ull << 52)),
        vdupq_n_u64((1151ull << 52) | (0x7fffffull << 29)), mag);
    bits = vorrq_u64(sign, vandq_u64(mag, vdupq_n_u64(~((1ull << 29) - 1))));
    return vreinterpretq_f64_u64(vandq_u64(bits,
        vcgeq_u64(mag, vdupq_n_u64(897ull << 52))));
}

/* Input has already passed through ms_ab_round or ms_ab_decode. */
static inline uint32x2_t ms_ab_encode(float64x2_t input)
{
    uint64x2_t bits = vreinterpretq_u64_f64(input);
    uint64x2_t mag = vandq_u64(bits, vdupq_n_u64(0x7fffffffffffffffull));
    uint32x2_t result = vmovn_u64(vshrq_n_u64(vsubq_u64(mag,
        vdupq_n_u64(896ull << 52)), 29));
    result = vorr_u32(result, vand_u32(vmovn_u64(vshrq_n_u64(bits, 32)),
        vdup_n_u32(0x80000000u)));
    return vand_u32(result, vmovn_u64(vcgeq_u64(mag, vdupq_n_u64(897ull << 52))));
}

static inline float64x2_t ms_ab_mul(float64x2_t a, float64x2_t b)
{
    float64x2_t r;
    __asm__ volatile("fmul %0.2d, %1.2d, %2.2d" : "=w"(r) : "w"(a), "w"(b));
    return ms_ab_round(r);
}
static inline float64x2_t ms_ab_madd(float64x2_t a, float64x2_t b, float64x2_t c)
{
    __asm__ volatile("fmla %0.2d, %1.2d, %2.2d" : "+w"(c) : "w"(a), "w"(b));
    return ms_ab_round(c);
}

/* count is a multiple of four; buffers must not overlap. Four lanes follow
 * the guest quadword rotation, not four consecutive independent PCM outputs.
 * Only lane zero is written, but the full history is returned exactly. */
static __attribute__((noinline)) void ms_audio_block_wide(
    const uint8_t* input, uint8_t* output, unsigned count,
    const ms_audio_block_coeff* coefficients, ms_audio_block_state* state)
{
    if (!count) return;
#define MS_AB_LOAD(NAME, SOURCE) \
    float64x2_t NAME##l = ms_ab_decode(vld1_u32((SOURCE))); \
    float64x2_t NAME##h = ms_ab_decode(vld1_u32((SOURCE) + 2))
    MS_AB_LOAD(a0, coefficients->a0); MS_AB_LOAD(a1, coefficients->a1);
    MS_AB_LOAD(a2, coefficients->a2); MS_AB_LOAD(b1, coefficients->b1);
    MS_AB_LOAD(b2, coefficients->b2);
    MS_AB_LOAD(x1, state->x1); MS_AB_LOAD(x2, state->x2);
    MS_AB_LOAD(y1, state->y1); MS_AB_LOAD(y2, state->y2);
#undef MS_AB_LOAD
    for (unsigned i = 0; i < count; i += 4) {
        uint32x4_t bits = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(input + i * 4)));
        float64x2_t xl = ms_ab_decode(vget_low_u32(bits));
        float64x2_t xh = ms_ab_decode(vget_high_u32(bits));
        for (unsigned j = 0; j < 4; ++j) {
            float64x2_t tl = ms_ab_mul(a0l, xl), th = ms_ab_mul(a0h, xh);
            tl = ms_ab_madd(x1l, a1l, tl); th = ms_ab_madd(x1h, a1h, th);
            tl = ms_ab_madd(x2l, a2l, tl); th = ms_ab_madd(x2h, a2h, th);
            tl = ms_ab_madd(y1l, b1l, tl); th = ms_ab_madd(y1h, b1h, th);
            tl = ms_ab_madd(y2l, b2l, tl); th = ms_ab_madd(y2h, b2h, th);
            uint32_t sample = SPU_BSWAP32(vget_lane_u32(ms_ab_encode(tl), 0));
            memcpy(output + (i + j) * 4, &sample, sizeof sample);
            x2l = x1l; x2h = x1h; x1l = xl; x1h = xh;
            y2l = y1l; y2h = y1h; y1l = tl; y1h = th;
            float64x2_t next = vextq_f64(xl, xh, 1);
            xh = vextq_f64(xh, xl, 1); xl = next;
        }
    }
    /* Raw input history can include denormals or negative zero. Unlike the
     * arithmetic, the guest copies those bits without normalization. */
    if (count) {
        uint32x4_t last = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(input + (count - 4) * 4)));
        vst1q_u32(state->x1, vextq_u32(last, last, 3));
        vst1q_u32(state->x2, vextq_u32(last, last, 2));
    }
    vst1_u32(state->y1, ms_ab_encode(y1l)); vst1_u32(state->y1 + 2, ms_ab_encode(y1h));
    vst1_u32(state->y2, ms_ab_encode(y2l)); vst1_u32(state->y2 + 2, ms_ab_encode(y2h));
}

static inline float32x4_t ms_ab_normal(uint32x4_t bits)
{
    return vreinterpretq_f32_u32(vandq_u32(bits,
        vcgeq_u32(vshlq_n_u32(bits, 1), vdupq_n_u32(0x01000000u))));
}
static inline float32x4_t ms_ab_mul_normal(float32x4_t a, float32x4_t b)
{
    float32x4_t r;
    __asm__ volatile("fmul %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b));
    return ms_ab_normal(vreinterpretq_u32_f32(r));
}
static inline float32x4_t ms_ab_madd_normal(float32x4_t a, float32x4_t b, float32x4_t c)
{
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(c) : "w"(a), "w"(b));
    return ms_ab_normal(vreinterpretq_u32_f32(c));
}
static inline uint32x4_t ms_ab_magnitude(uint32x4_t bits)
{
    return vandq_u32(bits, vdupq_n_u32(0x7fffffffu));
}

/* Checked eight-sample windows avoid per-operation overflow tests. With all
 * |coefficients| <= 2^8 and |input/history| <= 2^32, each output is at most
 * 5 * 2^8 times the preceding maximum. Eight steps stay strictly below
 * 2^(32+8*11) = 2^120, including every partial sum; IEEE overflow/extended
 * values cannot occur. Flush each intermediate as required by SPU arithmetic.
 * A failed window uses the exact wide kernel, then rechecks the next window.
 * This is a data bound, not an assumption about normal/inaudible game sounds. */
static __attribute__((noinline)) void ms_audio_block_process(
    const uint8_t* input, uint8_t* output, unsigned count,
    const ms_audio_block_coeff* k, ms_audio_block_state* s)
{
    if (!count) return;
    uint32x4_t a0b=vld1q_u32(k->a0), a1b=vld1q_u32(k->a1), a2b=vld1q_u32(k->a2);
    uint32x4_t b1b=vld1q_u32(k->b1), b2b=vld1q_u32(k->b2);
    uint32x4_t maximum=vmaxq_u32(vmaxq_u32(ms_ab_magnitude(a0b),ms_ab_magnitude(a1b)),
        vmaxq_u32(ms_ab_magnitude(a2b),vmaxq_u32(ms_ab_magnitude(b1b),ms_ab_magnitude(b2b))));
    if (vmaxvq_u32(maximum)>0x43800000u) {
        MS_AB_COUNT(wide_blocks);
        ms_audio_block_wide(input,output,count,k,s); return;
    }
    float32x4_t a0=ms_ab_normal(a0b),a1=ms_ab_normal(a1b),a2=ms_ab_normal(a2b);
    float32x4_t b1=ms_ab_normal(b1b),b2=ms_ab_normal(b2b);
    for (unsigned base=0;base<count;base+=8) {
        unsigned n=count-base<8?count-base:8;
        uint32x4_t x1b=vld1q_u32(s->x1),x2b=vld1q_u32(s->x2);
        uint32x4_t y1b=vld1q_u32(s->y1),y2b=vld1q_u32(s->y2);
        uint32x4_t q0=vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(input+base*4)));
        uint32x4_t q1=n==8?vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(input+(base+4)*4))):q0;
        maximum=vmaxq_u32(vmaxq_u32(ms_ab_magnitude(x1b),ms_ab_magnitude(x2b)),
            vmaxq_u32(vmaxq_u32(ms_ab_magnitude(y1b),ms_ab_magnitude(y2b)),
                      vmaxq_u32(ms_ab_magnitude(q0),ms_ab_magnitude(q1))));
        if (vmaxvq_u32(maximum)>0x4f800000u) {
            MS_AB_COUNT(wide_windows);
            ms_audio_block_wide(input+base*4,output+base*4,n,k,s); continue;
        }
        MS_AB_COUNT(normal_windows);
        float32x4_t x1=ms_ab_normal(x1b),x2=ms_ab_normal(x2b);
        float32x4_t y1=ms_ab_normal(y1b),y2=ms_ab_normal(y2b);
        for(unsigned i=0;i<n;i+=4) {
            float32x4_t x=ms_ab_normal(i?q1:q0);
            for(unsigned lane=0;lane<4;++lane) {
                float32x4_t y=ms_ab_mul_normal(a0,x);
                y=ms_ab_madd_normal(x1,a1,y); y=ms_ab_madd_normal(x2,a2,y);
                y=ms_ab_madd_normal(y1,b1,y); y=ms_ab_madd_normal(y2,b2,y);
                uint32_t sample=SPU_BSWAP32(vgetq_lane_u32(vreinterpretq_u32_f32(y),0));
                memcpy(output+(base+i+lane)*4,&sample,4);
                x2=x1;x1=x;y2=y1;y1=y;x=vextq_f32(x,x,1);
            }
        }
        uint32x4_t last=n==8?q1:q0;
        vst1q_u32(s->x1,vextq_u32(last,last,3));vst1q_u32(s->x2,vextq_u32(last,last,2));
        vst1q_u32(s->y1,vreinterpretq_u32_f32(y1));vst1q_u32(s->y2,vreinterpretq_u32_f32(y2));
    }
}
#undef MS_AB_COUNT
#endif

/* Compatibility adapter: the fixed final eight samples execute once through
 * the retained guest body to reconstruct every scratch register. Both filter
 * segments use the same semantic kernel. DMA and state writes stay in their
 * original order, including the state PUT between the two segments. */
static inline unsigned ms_audio_block_prepare(spu_context* ctx, unsigned second)
{
#if SPU_LS_DIAGNOSTICS || !defined(__aarch64__) || !defined(__ARM_NEON) || !defined(MOTORSTORM_SPU_THREAD_RTZ)
    (void)ctx; (void)second; return 0;
#else
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    unsigned done = second ? ctx->gpr[37]._u32[0] : ctx->gpr[39]._u32[0];
    unsigned end = ctx->gpr[second ? 43 : 37]._u32[0];
    if (ctx->image_id != 286484578 || ctx->policy_mode ||
        (fpcr & (3ull << 22)) != (3ull << 22) ||
        end > 1024 || done > end || (!second && done)) return 0;
    unsigned count = end - done;
    unsigned input = ctx->gpr[30]._u32[0], output = ctx->gpr[29]._u32[0];
    if (count < 16 || (count & 7) || ((input | output) & 15) ||
        input > SPU_LS_SIZE - count * 4 || output > SPU_LS_SIZE - count * 4 ||
        (input < output + count * 4 && output < input + count * 4)) return 0;
    ms_audio_block_coeff coefficients;
    ms_audio_block_state state;
    memcpy(coefficients.a0, ctx->gpr[35]._u32, 16);
    memcpy(coefficients.a1, ctx->gpr[34]._u32, 16);
    memcpy(coefficients.a2, ctx->gpr[33]._u32, 16);
    memcpy(coefficients.b1, ctx->gpr[32]._u32, 16);
    memcpy(coefficients.b2, ctx->gpr[31]._u32, 16);
    memcpy(state.x1, ctx->gpr[14]._u32, 16); memcpy(state.x2, ctx->gpr[15]._u32, 16);
    memcpy(state.y1, ctx->gpr[36]._u32, 16); memcpy(state.y2, ctx->gpr[38]._u32, 16);
    const unsigned prefix = count - 8;
    ms_audio_block_process(ctx->ls + input, ctx->ls + output, prefix, &coefficients, &state);
    memcpy(ctx->gpr[14]._u32, state.x1, 16); memcpy(ctx->gpr[15]._u32, state.x2, 16);
    memcpy(ctx->gpr[36]._u32, state.y1, 16); memcpy(ctx->gpr[38]._u32, state.y2, 16);
    ctx->gpr[29] = spu_ai(ctx->gpr[29], prefix * 4);
    ctx->gpr[30] = spu_ai(ctx->gpr[30], prefix * 4);
    unsigned counter = second ? 37 : 39;
    ctx->gpr[counter] = spu_ai(ctx->gpr[counter], prefix);
    static atomic_int reported;
    if (!atomic_load_explicit(&reported, memory_order_relaxed) &&
        !atomic_exchange_explicit(&reported, 1, memory_order_relaxed))
        fprintf(stderr, "[audio-block] native ARM filter active: samples=%u segment=%u\n", count, second);
    return prefix;
#endif
}
