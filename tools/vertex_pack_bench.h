/* Replay-only paired CPU benchmark. Repeats the two vertex preparation paths
 * against identical immutable source bytes, alternating order. This excludes
 * the rest of RSX/GPU/game work and must never be reported as gameplay FPS. */
#include "rsx_vertex_packed.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static struct { u64 ns; u64 calls, packed, fallback; } replay_pack_times[2];
static u64 replay_pack_cpu_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (u64)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static int replay_vertex_pack(const rsx_vertex_fetch_plan* plan,
    const rsx_vertex_ref* refs, u32 count, rsx_packed_vertices* out, int gather)
{
    static int repeats = -1;
    static rsx_packed_vertices scratch[2];
    static u8* cpu;
    static size_t capacity;
    if (repeats < 0) {
        const char* e = getenv("RSX_REPLAY_VERTEX_BENCH");
        repeats = e ? atoi(e) : 0;
        if (repeats < 0 || repeats > 64) repeats = 0;
    }
    if (repeats) {
        const size_t bytes = (size_t)count * plan->layout.stride;
        if (bytes > capacity) {
            u8* p = realloc(cpu, bytes);
            if (!p) { fprintf(stderr, "[vertex bench] allocation failed\n"); exit(2); }
            cpu = p; capacity = bytes;
        }
        /* Warm both buffers and code paths, then measure paired repeats. */
        for (int repeat = -1; repeat < repeats; ++repeat) for (int order = 0; order < 2; ++order) {
            const int variant = order ^ (repeat & 1);
            const u64 begin = replay_pack_cpu_ns();
            const int packed = rsx_vertex_pack_ex(plan, refs, count, &scratch[variant], variant);
            if (!packed) for (u32 i = 0; i < count; ++i)
                if (!rsx_vertex_fetch_one(plan, &refs[i], cpu + (size_t)i * plan->layout.stride)) {
                    fprintf(stderr, "[vertex bench] CPU fetch failed\n"); exit(2);
                }
            const u64 elapsed = replay_pack_cpu_ns() - begin;
            if (repeat >= 0) {
                replay_pack_times[variant].ns += elapsed;
                replay_pack_times[variant].calls++;
                replay_pack_times[variant].packed += packed != 0;
                replay_pack_times[variant].fallback += packed == 0;
            }
        }
    }
    return rsx_vertex_pack_ex(plan, refs, count, out, gather);
}
static void replay_vertex_bench_report(void)
{
    for (unsigned v = 0; v < 2; ++v) if (replay_pack_times[v].calls)
        fprintf(stderr, "[vertex bench] gather=%u CPU_ms=%.6f calls=%llu packed=%llu fallback=%llu\n",
            v, replay_pack_times[v].ns / 1000000.,
            (unsigned long long)replay_pack_times[v].calls,
            (unsigned long long)replay_pack_times[v].packed,
            (unsigned long long)replay_pack_times[v].fallback);
}
