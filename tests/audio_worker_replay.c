/* Differential replay of measured audio loops using local context snapshots.
 * All unexpected host/channel effects fail: this fixture does not fake DMA. */
#include "spu_interp.h"
#include "spu_audio_worker.h"
#include <assert.h>
#include <time.h>

int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe;
int g_spu_smc_watch, g_spu_ls_dbg, ms_audio_worker_test_mode;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
static spu_lifted_fn functions[SPU_LS_SIZE / 4];
static unsigned budget;
static spu_context input, reference, candidate;
static uint64_t expected_fpcr;
static void assert_host_mode(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    assert(value == expected_fpcr);
}

void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr) { abort(); }
u128 spu_rdch(spu_context* ctx, uint32_t ch) { fprintf(stderr,"Unexpected rdch %u at %X\n",ch,ctx->pc); abort(); }
void spu_wrch(spu_context* ctx, uint32_t ch, u128 value) { fprintf(stderr,"Unexpected wrch %u at %X\n",ch,ctx->pc); abort(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { abort(); }
void spu_stop(spu_context* ctx, unsigned code) { abort(); }
void spu_halt(spu_context* ctx) { abort(); }
void spu_register_function(unsigned pc, spu_lifted_fn fn) {
    assert(pc < SPU_LS_SIZE && !(pc & 3)); functions[pc / 4] = fn;
}
void spu_indirect_branch(spu_context* ctx) {
    assert_host_mode();
    assert(ctx->pc < SPU_LS_SIZE && !(ctx->pc & 3));
    assert(functions[ctx->pc / 4]); g_spu_trampoline_fn = functions[ctx->pc / 4];
}
void spu_indirect_branch_mt(spu_context* ctx) { spu_indirect_branch(ctx); }
static void drain(spu_context* ctx) {
    while (g_spu_trampoline_fn) {
        assert(budget--);
        spu_lifted_fn fn = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL; fn(ctx);
    }
}
void spu_drain_call(spu_context* ctx, unsigned pc) { assert_host_mode(); drain(ctx); assert(ctx->pc == pc); }
void spu_img_restore(spu_context* ctx, int image) { assert_host_mode(); assert(ctx->image_id == image); }
extern void spu_1136862334bb6208f794040f3a7f26e4991cd6b00eac6229331432566f1535d1_spu_recomp_register(void);

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void test_arithmetic(void) {
    const uint32_t edges[] = {0, 0x80000000, 1, 0x7fffff, 0x800000,
        0x3f000000, 0x3f7fffff, 0x3f800000, 0x3f800001, 0xbf800000,
        0x7f7fffff, 0xff7fffff, 0x7f800000, 0x7fffffff, 0xff800000, 0xffffffff};
    uint32_t random = 0x98d85238;
    for (unsigned i = 0; i < 200000; ++i) {
        u128 a, b, c;
        u128* values[] = {&a, &b, &c};
        for (unsigned v = 0; v < 3; ++v) for (unsigned lane = 0; lane < 4; ++lane) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            values[v]->_u32[lane] = i % 3 ? random : edges[random % 16];
        }
        u128 want[] = {spu_fa_ref(a,b),spu_fs_ref(a,b),spu_fm_ref(a,b),spu_fma_ref(a,b,c),spu_fms_ref(a,b,c)};
        uint64_t old = ms_rtz_enter();
        u128 got[] = {ms_rtz_fa(a,b,old),ms_rtz_fs(a,b,old),ms_rtz_fm(a,b,old),ms_rtz_fma(a,b,c,old),ms_audio_rtz_fms(a,b,c,old)};
        ms_rtz_leave(old);
        if (memcmp(want,got,sizeof want)) { fprintf(stderr,"Arithmetic mismatch at %u\n",i); abort(); }
    }
    puts("PASS: 4 million arithmetic lane comparisons, including extended floats and boundaries");
}
static double execute(spu_context* ctx, int merged) {
    *ctx = input; ctx->ls = ctx->ls_store;
    ms_audio_worker_test_mode = merged; budget = 1000000;
    g_spu_trampoline_fn = NULL;
    double start = now();
    uint64_t fpcr_before, fpcr_after;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr_before));
    expected_fpcr = fpcr_before;
    functions[ctx->pc / 4](ctx); drain(ctx);
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr_after));
    assert(fpcr_before == fpcr_after);
    return now() - start;
}
int main(int argc, char** argv) {
    assert(argc > 1);
    test_arithmetic();
    spu_1136862334bb6208f794040f3a7f26e4991cd6b00eac6229331432566f1535d1_spu_recomp_register();
    for (int file_index = 1; file_index < argc; ++file_index) {
        FILE* file = fopen(argv[file_index], "rb");
        assert(file && fread(&input, sizeof input, 1, file) == 1);
        assert(fgetc(file) == EOF); fclose(file);
        assert(input.image_id == 286484578 && !input.policy_mode);
        assert(input.pc < SPU_LS_SIZE && functions[input.pc / 4]);
        double old_time = 0, new_time = 0, scoped_time = 0;
        for (unsigned repeat = 0; repeat < 100; ++repeat) {
            old_time += execute(&reference, 0);
            new_time += execute(&candidate, 1);
            assert(reference.pc == candidate.pc && reference.status == candidate.status);
            if (memcmp(reference.gpr, candidate.gpr, sizeof reference.gpr) ||
                memcmp(reference.ls_store, candidate.ls_store, SPU_LS_SIZE)) {
                fprintf(stderr, "State mismatch: %s\n", argv[file_index]); return 1;
            }
            scoped_time += execute(&candidate, 2);
            assert(reference.pc == candidate.pc && reference.status == candidate.status);
            if (memcmp(reference.gpr, candidate.gpr, sizeof reference.gpr) ||
                memcmp(reference.ls_store, candidate.ls_store, SPU_LS_SIZE)) {
                fprintf(stderr, "Scoped state mismatch: %s\n", argv[file_index]); return 1;
            }
        }
        printf("PASS %s: all registers/LS/FPCR, original %.4f ms merged %.4f ms scoped %.4f ms\n",
               argv[file_index], old_time * 10, new_time * 10, scoped_time * 10);
    }
}
