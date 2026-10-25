/* NEON local-store accessors against the scalar byte-swap path. */
#include "spu_context.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int g_spu_ls_watch_n;
static unsigned watch_reads, watch_writes;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr;
    if (is_write) ++watch_writes; else ++watch_reads;
}

static void scalar_load(const uint8_t* p, u128* v)
{
    for (int i = 0; i < 4; i++)
        v->_u32[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                     ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
}
static void scalar_store(uint8_t* p, u128 v)
{
    for (int i = 0; i < 4; i++) {
        uint32_t w = v._u32[i];
        p[i*4] = (uint8_t)(w >> 24); p[i*4+1] = (uint8_t)(w >> 16);
        p[i*4+2] = (uint8_t)(w >> 8); p[i*4+3] = (uint8_t)w;
    }
}

int main(void)
{
#if !defined(SPU_LS_NEON)
    fprintf(stderr, "compile with -DSPU_LS_NEON\n");
    return 2;
#else
    spu_context* ctx = calloc(1, sizeof *ctx);
    uint8_t* mem = aligned_alloc(16, SPU_LS_SIZE + 32);
    if (!ctx || !mem) return 2;
    ctx->ls = mem + 1; /* deliberately unaligned */
    g_spu_ls_probe = 0;
    g_spu_smc_watch = 0;
    g_spu_ls_watch_n = 0;
    /* Exercise the old policy-mode branches without printing capped reports. */
    g_wws_read_probe = 48;
    g_wws_code_probe = 6;
    uint64_t s = 0x123456789abcdef0ull;
    for (uint32_t n = 0; n < 20000; n++) {
        ctx->image_id = n % 3 == 0 ? 2 : 17;
        ctx->policy_mode = n % 2;
        s = s * 6364136223846793005ull + 1;
        uint32_t lsa = (uint32_t)(s & SPU_LS_MASK);
        u128 value;
        for (int i = 0; i < 4; i++) value._u32[i] = (uint32_t)(s >> (i * 8));
        uint8_t expect[16], got[16];
        scalar_store(expect, value);
        spu_ls_write128(ctx, lsa, value);
        if (memcmp(ctx->ls + (lsa & ~15u), expect, 16)) {
            fprintf(stderr, "FAIL store lsa=%x\n", lsa);
            return 1;
        }
        u128 loaded = spu_ls_read128(ctx, lsa), ref;
        scalar_load(ctx->ls + (lsa & ~15u), &ref);
        if (memcmp(&loaded, &ref, 16)) {
            fprintf(stderr, "FAIL load lsa=%x\n", lsa);
            return 1;
        }
        memcpy(got, ctx->ls + (lsa & ~15u), 16);
        (void)got;
    }
    /* Wrapping and the low page, including address 0. */
    for (uint32_t lsa = 0; lsa < 64; lsa += 16) {
        u128 value;
        value._u32[0] = 0x11223344; value._u32[1] = 0xaabbccdd;
        value._u32[2] = 0x01020304; value._u32[3] = 0xff00ff00;
        spu_ls_write128(ctx, lsa + SPU_LS_SIZE, value);
        u128 loaded = spu_ls_read128(ctx, lsa);
        if (memcmp(&loaded, &value, 16)) {
            fprintf(stderr, "FAIL wrap lsa=%x\n", lsa);
            return 1;
        }
    }
    /* Armed observers must still work in diagnostic builds, and must not
     * run in normal iOS mode even when their run-time globals are nonzero. */
    ctx->image_id = 2;
    ctx->policy_mode = 1;
    g_spu_ls_watch_n = 1;
    g_wws_read_probe = g_wws_code_probe = 0;
    u128 value = {{0}};
    value._u32[0] = 0x11223344;
    spu_ls_write128(ctx, 0x132f, value);
    u128 loaded = spu_ls_read128(ctx, 0x1320);
    (void)spu_ls_read128(ctx, 0xdf1);
    if (memcmp(&loaded, &value, sizeof value) ||
        watch_reads != (SPU_LS_DIAGNOSTICS ? 2u : 0u) ||
        watch_writes != (SPU_LS_DIAGNOSTICS ? 1u : 0u) ||
        g_wws_read_probe != (SPU_LS_DIAGNOSTICS ? 1 : 0) ||
        g_wws_code_probe != (SPU_LS_DIAGNOSTICS ? 1 : 0)) {
        fprintf(stderr, "FAIL diagnostic gates or stored value\n");
        return 1;
    }
    free(mem);
    free(ctx);
    printf("PASS 20000 unaligned LS loads/stores, wrap, policy and armed probes (diagnostics=%d)\n",
           SPU_LS_DIAGNOSTICS);
    return 0;
#endif
}
