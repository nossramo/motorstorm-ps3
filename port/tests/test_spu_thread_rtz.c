/* Bit-exact check of the thread-RTZ SPU helpers against the integer reference.
 * Synthetic vectors only; not a gameplay measurement. */
#define MOTORSTORM_SPU_THREAD_RTZ_NO_MACROS
#include "spu_thread_rtz.h"
#include "spu_float_scope.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (uint32_t)seed; }
static uint64_t fpcr(void) { uint64_t r; __asm__ volatile("mrs %0, fpcr" : "=r"(r)); return r; }
static uint32_t pick(void) {
    const uint32_t r = rnd(), k = r & 15;
    if (k == 0) return 0;
    if (k == 1) return 0x80000000u;
    if (k == 2) return r & 0x807fffffu;
    if (k == 3) return (r & 0x807fffffu) | 0x7f800000u;
    if (k == 4) return (rnd() & 0x80000000u) | ((1 + rnd() % 30) << 23) | (rnd() & 0x7fffff);
    if (k == 5) return (rnd() & 0x80000000u) | ((224 + rnd() % 31) << 23) | (rnd() & 0x7fffff);
    if (k < 10) return (rnd() & 0x80000000u) | ((112 + rnd() % 32) << 23) | (rnd() & 0x7fffff);
    return rnd();
}
static void fill(u128* a, u128* b, u128* c, int cancel) {
    for (int i = 0; i < 4; i++) { a->_u32[i] = pick(); b->_u32[i] = pick(); c->_u32[i] = pick(); }
    if (cancel) for (int i = 0; i < 4; i++) c->_u32[i] = a->_u32[i] ^ (rnd() & 7);
}

#define CHECK3(OP, REF, A, B, C) do { \
    const u128 got = OP(A, B, C), exp = REF(A, B, C); \
    if (memcmp(&got, &exp, 16)) { \
        fprintf(stderr, "FAIL %s %08x %08x %08x -> %08x vs %08x\n", #OP, \
                (A)._u32[0], (B)._u32[0], (C)._u32[0], got._u32[0], exp._u32[0]); \
        return 1; \
    } \
} while (0)
#define CHECK2(OP, REF, A, B) do { \
    const u128 got = OP(A, B), exp = REF(A, B); \
    if (memcmp(&got, &exp, 16)) { \
        fprintf(stderr, "FAIL %s %08x %08x -> %08x vs %08x\n", #OP, \
                (A)._u32[0], (B)._u32[0], got._u32[0], exp._u32[0]); \
        return 1; \
    } \
} while (0)

int main(void) {
    const uint64_t host = fpcr();
    const long n = 8000000;
    ms_spu_exec_enter();
    ms_spu_exec_enter();
    if ((fpcr() & 0xc00000ull) != 0xc00000ull) {
        fprintf(stderr, "FAIL FPCR is not RTZ after enter\n");
        return 1;
    }
    for (long i = 0; i < n; i++) {
        u128 a, b, c;
        fill(&a, &b, &c, (i & 7) == 0);
        CHECK2(ms_thread_fa, spu_fa_ref, a, b);
        CHECK2(ms_thread_fs, spu_fs_ref, a, b);
        CHECK2(ms_thread_fm, spu_fm_ref, a, b);
        CHECK3(ms_thread_fma, spu_fma_ref, a, b, c);
        CHECK3(ms_thread_fms, spu_fms_ref, a, b, c);
        CHECK3(ms_thread_fnms, spu_fnms_ref, a, b, c);
    }
    ms_spu_exec_leave();
    if ((fpcr() & 0xc00000ull) != 0xc00000ull) {
        fprintf(stderr, "FAIL nested leave dropped RTZ\n");
        return 1;
    }
    ms_spu_exec_leave();
    if (fpcr() != host) {
        fprintf(stderr, "FAIL host FPCR not restored: %llx vs %llx\n",
                (unsigned long long)fpcr(), (unsigned long long)host);
        return 1;
    }
    printf("PASS %ld vectors x 6 ops, nested enter/leave, host FPCR restored\n", n);
    return 0;
}
