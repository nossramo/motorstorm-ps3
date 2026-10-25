/* Compare the actual generated guest loop with the native prefix plus the
 * same final guest iteration. Whole context and LS must remain identical. */
#include "spu_audio_filter.h"
#include "spu_float_scope.h"
#include <assert.h>
#include <time.h>
#include "audio_filter_reference.inc"

static spu_context initial, reference, candidate;
static uint32_t rng = 0x12345678;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; return rng ^= rng << 5; }
static const uint32_t edge[] = {0, 0x80000000, 1, 0x007fffff, 0x00800000,
    0x80800000, 0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7fffffff, 0xffffffff};
static void prepare(unsigned n, unsigned mode)
{
    memset(&initial, 0, sizeof initial); initial.ls = initial.ls_store;
    initial.image_id = 286484578; initial.pc = 0x32770;
    for (unsigned i = 0; i < 128; ++i)
        for (unsigned j = 0; j < 4; ++j)
            initial.gpr[i]._u32[j] = mode == 1 ? edge[random32() % (sizeof edge / sizeof *edge)] :
                mode == 2 ? random32() : (0x3d000000 + (random32() & 0xffffff)) | (random32() & 0x80000000);
    for (unsigned i = 0; i < SPU_LS_SIZE; i += 4) {
        uint32_t v = mode == 1 ? edge[random32() % (sizeof edge / sizeof *edge)] :
            mode == 2 ? random32() : (0x3e000000 + (random32() & 0xffffff)) | (random32() & 0x80000000);
        v = SPU_BSWAP32(v); memcpy(initial.ls + i, &v, 4);
    }
    initial.gpr[29]._u32[0] = 0x10000; initial.gpr[30]._u32[0] = 0x18000;
    initial.gpr[37]._u32[0] = n; initial.gpr[39] = spu_il(0);
}
static unsigned compare(void)
{
    reference = initial; reference.ls = reference.ls_store;
    candidate = initial; candidate.ls = candidate.ls_store;
    reference_loop(&reference);
    unsigned skipped = ms_audio_filter_prefix(&candidate);
    reference_loop(&candidate);
    assert(!memcmp(reference.gpr, candidate.gpr, sizeof reference.gpr));
    assert(!memcmp(reference.ls, candidate.ls, SPU_LS_SIZE));
    /* Normalize the only pointer that intentionally differs. */
    reference.ls = candidate.ls = initial.ls;
    assert(!memcmp(&reference, &candidate, sizeof reference));
    return skipped;
}
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9;
}
int main(int argc, char** argv)
{
    uint64_t fpcr = ms_rtz_enter();
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 3; ++mode)
        for (unsigned n = 16; n <= 1024; n += 8) {
            prepare(n, mode); assert(compare() == n - 8); ++cases;
        }
    /* Guards must leave the whole state intact; then the original loop runs. */
    for (unsigned i = 0; i < 12; ++i) {
        prepare(32, 0);
        switch (i) {
        case 0: initial.image_id++; break;
        case 1: initial.policy_mode = 1; break;
        case 2: initial.gpr[39] = spu_il(8); break;
        case 3: initial.gpr[37]._u32[0] = 8; break;
        case 4: initial.gpr[37]._u32[0] = 31; break;
        case 5: initial.gpr[37]._u32[0] = 1032; break;
        case 6: initial.gpr[29]._u32[0] += 4; break;
        case 7: initial.gpr[30]._u32[0] += 4; break;
        case 8: initial.gpr[29]._u32[0] = SPU_LS_SIZE - 16; break;
        case 9: initial.gpr[30]._u32[0] = SPU_LS_SIZE - 16; break;
        case 10: initial.gpr[29]._u32[0] = initial.gpr[30]._u32[0]; break;
        case 11: initial.gpr[29]._u32[0] = initial.gpr[30]._u32[0] + 16; break;
        }
        candidate = initial;
        assert(ms_audio_filter_prefix(&candidate) == 0);
        assert(!memcmp(&candidate, &initial, sizeof initial));
        assert(compare() == 0); ++cases;
    }
    printf("PASS %u cases: all GPRs, context bytes, 256 KiB LS and fallback guards\n", cases);
    prepare(512, 0);
    if (argc == 2) {
        FILE* f = fopen(argv[1], "rb");
        assert(f && fread(&initial, sizeof initial, 1, f) == 1 && fgetc(f) == EOF);
        fclose(f); initial.ls = initial.ls_store;
        unsigned skipped = compare();
        assert(skipped > 0);
        printf("PASS captured loop: %u of %u samples use the native prefix\n",
            skipped, initial.gpr[37]._u32[0]);
    } else assert(argc == 1);
    const unsigned repeat = 5000;
    for (unsigned pass = 0; pass < 4; ++pass) {
        double elapsed[2] = {0, 0};
        for (unsigned kind = 0; kind < 2; ++kind) {
            unsigned optimized = kind ^ (pass & 1);
            for (unsigned i = 0; i < repeat; ++i) {
                candidate = initial; candidate.ls = candidate.ls_store;
                double start = now();
                if (optimized) ms_audio_filter_prefix(&candidate);
                reference_loop(&candidate);
                elapsed[optimized] += now() - start;
            }
        }
        printf("BENCH %u samples original=%.3f us native=%.3f us speedup=%.3fx\n",
            initial.gpr[37]._u32[0], elapsed[0]*1e6/repeat,
            elapsed[1]*1e6/repeat, elapsed[0]/elapsed[1]);
    }
    ms_rtz_leave(fpcr);
}
