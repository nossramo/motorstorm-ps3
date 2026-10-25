/* Opt-in host backoff for one measured semaphore polling loop in image
 * e4633a..., image id 507917221. This is not the WWS policy image (900).
 * The original GETLLAR/PUTLLC consumer decides when it may continue. */
#ifndef MOTORSTORM_SPU_SEMAPHORE_WAIT_H
#define MOTORSTORM_SPU_SEMAPHORE_WAIT_H
#include "spu_helpers.h"
#include "spu_coherency.h"
#include "spu_lockstep.h"
#include "ps3emu/ps3_ftrace.h"

static int motorstorm_spu_semaphore_delay(spu_context* ctx)
{
    static int setting; /* 0 unknown, 1 disabled, 2 enabled */
    int mode = __atomic_load_n(&setting, __ATOMIC_RELAXED);
    if (!mode) {
        const char* option = getenv("MOTORSTORM_SPU_SEMAPHORE_WAIT");
        mode = option && option[0] == '1' && !option[1] ? 2 : 1;
        __atomic_store_n(&setting, mode, __ATOMIC_RELAXED);
    }
    if (mode != 2 || ctx->image_id != 507917221 || ctx->pc != 0xAAE8 ||
        yz_lockstep_enabled()) return 0;
#if SPU_LS_DIAGNOSTICS
    /* Disabled probes leave their lazy-init gates at -1. Only a build that
     * can actually invoke observers needs to reject unknown/active gates. */
    if (g_spu_ls_watch_n || g_spu_ls_probe || g_spu_smc_watch) return 0;
#endif
    const uint32_t sp = ctx->gpr[1]._u32[0];
    if ((sp & 15) || sp < 0x30000 || sp > 0x3FFB0 ||
        ctx->gpr[82]._u32[0] != ctx->mfc_lsa ||
        ctx->mfc_lsa >= 0x30000 || ctx->mfc_size != 128 || ctx->mfc_eah) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (ctx->gpr[6]._u32[i] || ctx->gpr[7]._u32[i] != 1000 ||
            ctx->gpr[8]._u32[i]) return 0;
    /* The aligned stack loop repeatedly zeroes the same word, with no channel
     * access. Keep its last iteration to produce every original register/LS
     * value, replacing the other 999 iterations by bounded host backoff.
     * A reservation lost before registration returns immediately; coherent
     * writers notify, and the timeout covers writers without notifications.
     * Nothing treats wakeup itself as semaphore acquisition. */
    const int reason = spu_coh_wait_reservation(ctx, 100);
    if (reason == 3) return 0;
    ctx->gpr[6] = spu_il(999);
    static SPU_THREAD_LOCAL int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[spu-semaphore] native wait active: image=%d pc=%05X ls_diagnostics=%d\n",
                ctx->image_id, (unsigned)ctx->pc, SPU_LS_DIAGNOSTICS);
    }
    return reason + 1;
}
#endif
