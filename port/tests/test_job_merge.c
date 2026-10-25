/* Complete-state differential replay of the production block-fusion output. */
#include "spu_job_merge.h"
#include "spu_helpers.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

int g_spu_ls_watch_n, g_spu_hop_fast = 1;
void* volatile g_pm_flow_ctx;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
int ms_job_merge_test_enabled;
#ifdef MOTORSTORM_SPU_PIPELINE_TEST
unsigned ms_spu_pipeline_test_mask;
#endif
static unsigned remaining, fused_hops;
int ms_job_merge_test_hop(spu_context* ctx)
{
    (void)ctx;
    if (!remaining) return 0;
    --remaining;
    ++fused_hops;
    return 1;
}
void spu_ls_watch_slow(uint32_t lsa, int write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)write; (void)p; (void)pc; (void)lr; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; abort(); }

#include "job_merge.inc"

static uint64_t seed = 0x4f12569ab0ull;
static uint32_t random32(void)
{
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return (uint32_t)seed;
}
static uint64_t fpcr(void)
{ uint64_t r; __asm__ volatile("mrs %0, fpcr" : "=r"(r)); return r; }
static void set_fpcr(uint64_t value)
{ __asm__ volatile("msr fpcr, %0" :: "r"(value) : "memory"); }

static void run(spu_context* ctx, unsigned c, unsigned budget)
{
    remaining = budget;
    g_spu_trampoline_fn = NULL;
    cases[c].fn(ctx);
    while (g_spu_trampoline_fn) {
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        unsigned next = sizeof(cases)/sizeof(*cases);
        for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); i++)
            if (cases[i].fn == fn) { next = i; break; }
        if (next == sizeof(cases)/sizeof(*cases) || !ctx->policy_mode ||
            !spu_hop_release_ok(ctx) ||
            (ctx->int_enable && (ctx->event_status & ctx->event_mask)) ||
            !remaining) return;
        --remaining;
        ++ctx->steps;
        g_spu_trampoline_fn = NULL;
        fn(ctx);
    }
}

int main(void)
{
    const unsigned count = sizeof(cases)/sizeof(*cases);
    spu_context *input = calloc(1, sizeof(*input)), *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    uint8_t *initial = malloc(SPU_LS_SIZE), *memory = malloc(SPU_LS_SIZE), *expected = malloc(SPU_LS_SIZE);
    assert(input && a && b && initial && memory && expected);
    g_spu_ls_probe = g_spu_smc_watch = 0;
    const uint64_t host = fpcr();
    uint64_t checks = 0;
    for (unsigned trial = 0; trial < 24; trial++) {
        for (unsigned i = 0; i < SPU_LS_SIZE; i++)
            initial[i] = trial % 3 == 0 ? 0 : (uint8_t)random32();
        for (unsigned c = 0; c < count; c++) {
            memset(input, 0, sizeof(*input));
            input->ls = memory;
            input->pc = cases[c].pc;
            input->status = SPU_STATUS_RUNNING;
            input->policy_mode = trial % 8 != 1;
            input->image_id = 209;
            input->host_depth = trial % 2;
            input->drain_ret_pc = 0x3258;
            input->int_enable = trial % 8 == 2 || trial % 8 == 3;
            input->event_status = 1;
            input->event_mask = trial % 8 == 2 ? 1 : 2;
            g_spu_hop_fast = trial % 8 != 4;
            for (unsigned r = 0; r < 128; r++) for (unsigned l = 0; l < 4; l++) {
                uint32_t bits = random32();
                if (trial % 3 == 0) bits &= 0x3fu;
                if (trial % 3 == 1) bits = (bits & 0x807fffff) | ((120 + random32()%15) << 23);
                input->gpr[r]._u32[l] = bits;
            }
            memcpy(a, input, sizeof(*a)); memcpy(b, input, sizeof(*b));
            const uint64_t rounding = (host & ~0xc00000ull) | ((uint64_t)(trial % 4) << 22);
            const unsigned budget = trial % 3 == 0 ? 0 : trial % 3 == 1 ? 8 : 64;
            memcpy(memory, initial, SPU_LS_SIZE);
            g_pm_flow_ctx = trial % 8 == 5 ? a : NULL;
            ms_job_merge_test_enabled = 0;
#ifdef MOTORSTORM_SPU_PIPELINE_TEST
            ms_spu_pipeline_test_mask = 0;
#endif
            set_fpcr(rounding); run(a, c, budget);
            const uint64_t ref_fpcr = fpcr();
            void (*target)(spu_context*) = g_spu_trampoline_fn;
            memcpy(expected, memory, SPU_LS_SIZE);
            memcpy(memory, initial, SPU_LS_SIZE);
            g_pm_flow_ctx = trial % 8 == 5 ? b : NULL;
            ms_job_merge_test_enabled = 1;
#ifdef MOTORSTORM_SPU_PIPELINE_TEST
            ms_spu_pipeline_test_mask = 2;
#endif
            set_fpcr(rounding); run(b, c, budget);
            if (memcmp(a, b, sizeof(*a)) || memcmp(expected, memory, SPU_LS_SIZE) ||
                target != g_spu_trampoline_fn || fpcr() != ref_fpcr || fpcr() != rounding) {
                fprintf(stderr, "FAIL entry=%u pc=%x trial=%u steps=%llu/%llu pc=%x/%x\n",
                    c, cases[c].pc, trial, (unsigned long long)a->steps,
                    (unsigned long long)b->steps, (unsigned)a->pc, (unsigned)b->pc);
                return 1;
            }
            ++checks;
        }
    }
    set_fpcr(host);
    assert(fused_hops > count);
    printf("PASS %u entries, %llu complete context/LS/exit/FPCR comparisons, %u fused hops; "
           "four rounding modes, interrupt/diagnostic/non-policy fallbacks\n",
           count, (unsigned long long)checks, fused_hops);
    free(input); free(a); free(b); free(initial); free(memory); free(expected);
    return 0;
}
