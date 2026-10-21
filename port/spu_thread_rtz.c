#include "spu_thread_rtz.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include "spu_float_scope.h"

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
