#pragma once
/* iOS WWS block fusion. A local edge replaces only a release-mode drain
 * iteration in a policy context. Interrupts, diagnostic hooks, channels,
 * guest calls and indirect transfers retain their original runtime path. */
#include "spu_context.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef MOTORSTORM_JOB_MERGE_TEST
extern int ms_job_merge_test_enabled;
int ms_job_merge_test_hop(spu_context* ctx);
static inline int ms_job_merge_enabled(void) { return ms_job_merge_test_enabled; }
#else
static inline int ms_job_merge_enabled(void)
{
    /* The launcher sets the environment after constructors. Cache on first
     * execution, with concurrent SPURS workers allowed to arrive together. */
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value < 0) {
        const char* setting = getenv("MOTORSTORM_JOB_MERGE");
        value = setting && setting[0] == '1' && setting[1] == '\0';
        atomic_store_explicit(&cached, value, memory_order_relaxed);
    }
    return value;
}
#endif

static inline __attribute__((always_inline)) int ms_job_merge_hop(spu_context* ctx)
{
    if (!ctx->policy_mode || !spu_hop_release_ok(ctx) ||
        (ctx->int_enable && (ctx->event_status & ctx->event_mask))) return 0;
    /* spu_hop_release only increments steps for policy contexts. No stack
     * reset can occur here. The generated regions contain no guest calls,
     * so the enclosing drain's return PC and IRQ frame remain unchanged. */
#ifdef MOTORSTORM_JOB_MERGE_TEST
    if (!ms_job_merge_test_hop(ctx)) return 0;
#endif
    ++ctx->steps;
    return 1;
}
