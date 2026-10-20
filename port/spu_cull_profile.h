#pragma once
#include "spu_cull_bounds.h"
#include "ps3emu/ps3_ftrace.h"

/* Port-local diagnostic kinds, outside the shared runtime's enum:
 * 1000: who=SPU, a=inclusive thread CPU ns, b=8480 blocks in this sampled call.
 * 1001: who=SPU, a=eligible blocks, b=8480 blocks; same interval.
 * 1002/1003: rejected input lanes: negative zero/subnormal, small/large.
 * One out of 64 complete calls to 0x8380 is sampled per host thread. Results
 * are sampled attribution, not an exact CPU total or normal-play FPS.
 * No sampling spans a guest call boundary that the original lift omitted.
 */
typedef struct ms_cull_probe {
    uint64_t start, cpu, generation;
    int active;
} ms_cull_probe;
static SPU_THREAD_LOCAL struct {
    uint64_t generation;
    uint32_t blocks, eligible, negative_zero, subnormal, small, large;
    int active;
} ms_cull_current_probe;
static SPU_THREAD_LOCAL unsigned ms_cull_probe_counter;

static void ms_cull_probe_begin(ms_cull_probe* p)
{
    memset(p, 0, sizeof(*p));
    /* Never store a pointer to a host stack frame: an SPU runtime longjmp
     * can abandon that frame. A nested call invalidates the outer sample;
     * the next begin always clears any sample abandoned by such an unwind. */
    ++ms_cull_current_probe.generation;
    ms_cull_current_probe.active = 0;
    if (!PS3_FT_ON()) return;
    if ((ms_cull_probe_counter++ & 63u) != 0) return;
    const uint64_t now = ps3_ft_now();
    if (now < g_ps3_ft.start || now >= g_ps3_ft.end) return;
    p->start = now;
    p->active = 1;
    p->generation = ms_cull_current_probe.generation;
    ms_cull_current_probe.active = 1;
    ms_cull_current_probe.blocks = ms_cull_current_probe.eligible = 0;
    ms_cull_current_probe.negative_zero = ms_cull_current_probe.subnormal = 0;
    ms_cull_current_probe.small = ms_cull_current_probe.large = 0;
    p->cpu = ps3_ft_cpu_ns(0);
}

static int ms_cull_probe_inputs(const spu_context* ctx)
{
    const int eligible = ms_cull_bounds_inputs(ctx);
    if (ms_cull_current_probe.active) {
        ++ms_cull_current_probe.blocks;
        ms_cull_current_probe.eligible += eligible;
        if (!eligible) {
            static const unsigned inputs[] = {
                9,15,16,17,18,19,20,21,22,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41
            };
            for (unsigned r=0;r<sizeof(inputs)/sizeof(inputs[0]);++r)
                for (unsigned l=0;l<4;++l) {
                    const uint32_t raw=ctx->gpr[inputs[r]]._u32[l], mag=raw&0x7fffffff;
                    if (raw==0x80000000) ++ms_cull_current_probe.negative_zero;
                    else if (mag && !(mag&0x7f800000)) ++ms_cull_current_probe.subnormal;
                    else if (mag && mag<0x30800000) ++ms_cull_current_probe.small;
                    else if (mag>=0x4e800000) ++ms_cull_current_probe.large;
                }
        }
    }
    return eligible;
}

static void ms_cull_probe_end(ms_cull_probe* p, uint32_t spu_id)
{
    if (!p->active) return;
    if (p->generation != ms_cull_current_probe.generation) return;
    const uint64_t cpu = ps3_ft_cpu_ns(0) - p->cpu;
    const uint64_t end = ps3_ft_now();
    ms_cull_current_probe.active = 0;
    /* Reject boundary-crossing samples rather than prorating their counts. */
    if (end > g_ps3_ft.end) return;
    ps3_ft_span(1000, spu_id, (uint32_t)cpu, ms_cull_current_probe.blocks, p->start, end);
    ps3_ft_span(1001, spu_id, ms_cull_current_probe.eligible, ms_cull_current_probe.blocks, p->start, end);
    ps3_ft_span(1002, spu_id, ms_cull_current_probe.negative_zero, ms_cull_current_probe.subnormal, p->start, end);
    ps3_ft_span(1003, spu_id, ms_cull_current_probe.small, ms_cull_current_probe.large, p->start, end);
}
