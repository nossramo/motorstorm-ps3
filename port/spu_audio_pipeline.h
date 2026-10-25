#pragma once
/* iOS native-register execution across checked audio / WWS regions.
 * The generator publishes all potentially modified registers at every
 * observable call/exit and reloads after context callbacks. No ABI liveness
 * or dead scratch-register assumption is used. */
#include "spu_context.h"
#include <stdatomic.h>
#include <stdlib.h>
#ifdef MOTORSTORM_SPU_PIPELINE_TEST
extern unsigned ms_spu_pipeline_test_mask;
static inline int ms_spu_pipeline_enabled(unsigned mask) {
#if SPU_LS_DIAGNOSTICS
    (void)mask; return 0;
#else
    return !!(ms_spu_pipeline_test_mask & mask);
#endif
}
#else
static inline int ms_spu_pipeline_enabled(unsigned mask) {
#if SPU_LS_DIAGNOSTICS
    (void)mask; return 0;
#else
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value < 0) {
        const char* env = getenv("MOTORSTORM_SPU_PIPELINE");
        value = env ? (int)(strtoul(env, NULL, 0) & 3u) : 0;
        atomic_store_explicit(&cached, value, memory_order_relaxed);
    }
    return !!((unsigned)value & mask);
#endif
}
#endif
