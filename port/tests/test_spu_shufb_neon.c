/* Check every selector in every lane, mixed selectors and operand aliases
 * against both the original NEON helper and the scalar SPU-byte reference.
 * Optional timings are host microbenchmarks, never iPhone gameplay results.
 */
#define MOTORSTORM_SHUFB_NEON_NO_MACRO
#include "spu_shufb_neon.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t rng = 0x8c491e51ac9f4703ull;
static unsigned long checked;
static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}
static u128 random128(void)
{
    u128 value;
    for (int i = 0; i < 4; ++i) value._u32[i] = random32();
    return value;
}
static void check(u128 a, u128 b, u128 c)
{
    const u128 expected = spu_shufb_ref(a, b, c);
    const u128 previous = spu_shufb(a, b, c);
    const u128 result = ms_spu_shufb_neon(a, b, c);
    if (memcmp(&result, &expected, sizeof result) ||
        memcmp(&previous, &expected, sizeof previous)) {
        fprintf(stderr, "SHUFB mismatch in case %lu\n", checked);
        for (int i = 0; i < 16; ++i)
            fprintf(stderr, "lane %d: control=%02x expected=%02x old=%02x new=%02x\n",
                    i, c._u8[i], expected._u8[i], previous._u8[i], result._u8[i]);
        exit(1);
    }
    ++checked;
}

/* Kept externally visible for inspection of the compiler's actual lowering. */
__attribute__((noinline)) u128 shufb_codegen_old(u128 a, u128 b, u128 c)
{ return spu_shufb(a, b, c); }
__attribute__((noinline)) u128 shufb_codegen_new(u128 a, u128 b, u128 c)
{ return ms_spu_shufb_neon(a, b, c); }

static u128 masks[256], seeds[4];
static volatile uint64_t sink;
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
#define BENCH(NAME, FN) \
static double NAME(unsigned count, int streams, uint64_t *checksum) { \
    u128 x0 = seeds[0], x1 = seeds[1], x2 = seeds[2], x3 = seeds[3]; \
    const u128 b = seeds[1]; \
    const double start = now_ns(); \
    for (unsigned i = 0; i < count; ++i) { \
        const u128 c = masks[i & 255]; \
        x0 = FN(x0, b, c); \
        if (streams == 4) { \
            x1 = FN(x1, b, c); x2 = FN(x2, b, c); x3 = FN(x3, b, c); \
        } \
    } \
    const double elapsed = now_ns() - start; \
    *checksum = x0._u64[0] ^ x0._u64[1] ^ x1._u64[0] ^ x2._u64[1] ^ x3._u64[0]; \
    sink = *checksum; \
    return elapsed / (count * streams); \
}
BENCH(bench_old, spu_shufb)
BENCH(bench_new, ms_spu_shufb_neon)
#undef BENCH
static int compare_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void benchmark(void)
{
    for (int i = 0; i < 256; ++i) masks[i] = random128();
    for (int i = 0; i < 4; ++i) seeds[i] = random128();
    for (int streams = 1; streams <= 4; streams += 3) {
        double old[9], candidate[9];
        for (int round = 0; round < 9; ++round) {
            uint64_t a, b;
            if (round & 1) {
                candidate[round] = bench_new(4000000, streams, &b);
                old[round] = bench_old(4000000, streams, &a);
            } else {
                old[round] = bench_old(4000000, streams, &a);
                candidate[round] = bench_new(4000000, streams, &b);
            }
            if (a != b) { fputs("benchmark checksum mismatch\n", stderr); exit(1); }
        }
        qsort(old, 9, sizeof(double), compare_double);
        qsort(candidate, 9, sizeof(double), compare_double);
        printf("HOST microbenchmark (%d independent chain%s): old %.3f, new %.3f ns/op, %.1f%% less time\n",
               streams, streams == 1 ? "" : "s", old[4], candidate[4],
               (1.0 - candidate[4] / old[4]) * 100.0);
    }
}

int main(int argc, char **argv)
{
    u128 a, b, c;
    for (int i = 0; i < 16; ++i) { a._u8[i] = (uint8_t)(i + 1); b._u8[i] = (uint8_t)(i + 33); }
    /* Distinct source bytes expose endianness and source-selection errors. */
    for (int lane = 0; lane < 16; ++lane) {
        for (int selector = 0; selector < 256; ++selector) {
            c = random128(); c._u8[lane] = (uint8_t)selector;
            check(a, b, c);
        }
    }
    for (int selector = 0; selector < 256; ++selector) {
        memset(&c, selector, sizeof c);
        check(a, b, c);
    }
    for (int n = 0; n < 1000000; ++n) {
        a = random128(); b = random128(); c = random128();
        check(a, b, c); check(a, a, c); check(c, b, c);
        check(a, c, c); check(c, c, c);
    }
    printf("PASS %lu vectors (%lu byte lanes): new SHUFB == old NEON == scalar reference\n",
           checked, checked * 16);
    if (argc == 2 && !strcmp(argv[1], "--bench")) benchmark();
    return 0;
}
