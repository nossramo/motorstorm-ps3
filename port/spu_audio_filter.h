#pragma once
/* Native recurrence for the verified MotorStorm 0x32770 audio overlay.
 * The generator retains the last eight-sample guest iteration: it restores
 * every scratch GPR exactly, so callers observe the same full SPU state.
 * Coefficients, history and all four lanes use the existing exact SPU float
 * helpers. No reassociation, approximate floats or new rounding policy.
 */
#include "spu_helpers.h"
#include <stdatomic.h>
#include <stdlib.h>

static inline int ms_audio_filter_enabled(void)
{
    /* iOS installs its launch environment after C constructors. */
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value < 0) {
        const char* setting = getenv("MOTORSTORM_AUDIO_FILTER");
        value = setting && setting[0] == '1' && setting[1] == '\0';
        atomic_store_explicit(&cached, value, memory_order_relaxed);
    }
    return value;
}

static __attribute__((noinline)) unsigned ms_audio_filter_prefix(spu_context* ctx)
{
#if SPU_LS_DIAGNOSTICS
    /* Direct stores would bypass per-access watch/SMC hooks. */
    (void)ctx;
    return 0;
#else
    unsigned count = ctx->gpr[37]._u32[0];
    unsigned input = ctx->gpr[30]._u32[0], output = ctx->gpr[29]._u32[0];
    if (ctx->image_id != 286484578 || ctx->policy_mode ||
        ctx->gpr[39]._u32[0] != 0 || count < 16 || count > 1024 || (count & 7) ||
        ((input | output) & 15) || input > SPU_LS_SIZE - count * 4 ||
        output > SPU_LS_SIZE - count * 4 ||
        (input < output + count * 4 && output < input + count * 4)) return 0;

    const u128 a0 = ctx->gpr[35], a1 = ctx->gpr[34], a2 = ctx->gpr[33];
    const u128 b1 = ctx->gpr[32], b2 = ctx->gpr[31];
    u128 x1 = ctx->gpr[14], x2 = ctx->gpr[15];
    u128 y1 = ctx->gpr[36], y2 = ctx->gpr[38];
    const unsigned prefix = count - 8;
    for (unsigned i = 0; i < prefix; i += 4) {
        u128 quad = spu_ls_read128(ctx, input + i * 4);
        for (unsigned lane = 0; lane < 4; ++lane) {
            u128 x = quad;
            u128 t = spu_fm(a0, x);
            t = spu_fma(x1, a1, t);
            t = spu_fma(x2, a2, t);
            t = spu_fma(y1, b1, t);
            u128 y = spu_fma(y2, b2, t);
            uint32_t be = SPU_BSWAP32(y._u32[0]);
            memcpy(ctx->ls + output + (i + lane) * 4, &be, sizeof be);
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            /* Equivalent to ROTQBY by 4, without a dynamic byte permute. */
            quad._u32[0] = x._u32[1]; quad._u32[1] = x._u32[2];
            quad._u32[2] = x._u32[3]; quad._u32[3] = x._u32[0];
        }
    }
    ctx->gpr[14] = x1; ctx->gpr[15] = x2;
    ctx->gpr[36] = y1; ctx->gpr[38] = y2;
    /* AI increments all four lanes, even though only lane zero addresses LS. */
    ctx->gpr[29] = spu_ai(ctx->gpr[29], prefix * 4);
    ctx->gpr[30] = spu_ai(ctx->gpr[30], prefix * 4);
    ctx->gpr[39] = spu_ai(ctx->gpr[39], prefix);
    static atomic_int reported;
    if (!atomic_load_explicit(&reported, memory_order_relaxed) &&
        !atomic_exchange_explicit(&reported, 1, memory_order_relaxed))
        fprintf(stderr, "[audio-filter] native recurrence active: samples=%u prefix=%u\n", count, prefix);
    return prefix;
#endif
}
