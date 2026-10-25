#pragma once
/* Inline fast path for lifted guest stores. A plain store to ordinary guest
 * memory skips the out-of-line call; every store the runtime's vm_write*
 * treats specially still calls it unchanged: store watches, the null page and
 * null sweeps, the end of the mapping, raw-SPU problem state (0xE0000000 and
 * above) and lines an SPU has reserved, which need the coherent store and
 * reservation loss. */
#include "ppu_recomp.h"
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
/* One bit per 128-byte line, never cleared (spu_coherency.c). */
extern unsigned char g_spu_coh_bitmap[];
#ifdef __cplusplus
}
#endif

/* Read on the first store of this unit: guest stores eligible for the fast
 * path are those with 0x10000 <= ea and ea + n <= 0x10000 + ms_store_span.
 * Zero disables the fast path. The diagnostics that make vm_write* act on an
 * ordinary store are armed only by these variables (PPU_WWATCH/PPU_WVAL:
 * barrier_watch_hit; SPU_ATOM_EA: g_barrier_sync_watch; PS3_NULL_SWEEP:
 * g_null_sweep_hi), and ppu_vm_size is fixed before guest code runs. */
static uint64_t ms_store_span;
static int ms_store_ready;

static __attribute__((noinline, cold)) uint64_t ms_store_configure(void)
{
    if (!__atomic_load_n(&ms_store_ready, __ATOMIC_ACQUIRE)) {
        const int diagnostics = getenv("PPU_WWATCH") || getenv("PPU_WVAL") ||
                                getenv("SPU_ATOM_EA") || getenv("PS3_NULL_SWEEP");
        const uint32_t end = ppu_vm_size && ppu_vm_size < 0xE0000000u ? ppu_vm_size : 0xE0000000u;
        const uint64_t span = diagnostics || end <= 0x10000u ? 0 : (uint64_t)end - 0x10000u;
        __atomic_store_n(&ms_store_span, span, __ATOMIC_RELAXED);
        __atomic_store_n(&ms_store_ready, 1, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&ms_store_span, __ATOMIC_RELAXED);
}

/* Relaxed: a clear bit is a value spu_coh_is_reserved's acquire load could
 * also return, a set bit sends the store to vm_write*, which checks again,
 * and the guest store depends on this load through the branch. */
static inline __attribute__((always_inline)) int ms_store_line_reserved(uint32_t ea)
{
    const uint32_t line = ea >> 7;
    return (__atomic_load_n(&g_spu_coh_bitmap[line >> 3], __ATOMIC_RELAXED) >> (line & 7)) & 1u;
}

static inline __attribute__((always_inline)) int ms_store_fast(uint32_t ea, uint32_t n)
{
    const uint64_t offset = (uint64_t)(uint32_t)(ea - 0x10000u) + n;
    if (__builtin_expect(offset > __atomic_load_n(&ms_store_span, __ATOMIC_RELAXED), 0) &&
        (__atomic_load_n(&ms_store_ready, __ATOMIC_RELAXED) || offset > ms_store_configure()))
        return 0;
    /* A bit set before the coherence layer is armed only costs the slow path. */
    if (ms_store_line_reserved(ea)) return 0;
    if ((ea & 127u) > 128u - n && ms_store_line_reserved(ea + n - 1u)) return 0;
    return 1;
}

/* One host store per guest store: arm64 str is single-copy atomic when the
 * address is aligned, as VM_RAW_ST's aligned atomic store is. */
typedef uint16_t __attribute__((aligned(1), may_alias)) ms_store_u16;
typedef uint32_t __attribute__((aligned(1), may_alias)) ms_store_u32;
typedef uint64_t __attribute__((aligned(1), may_alias)) ms_store_u64;

static inline __attribute__((always_inline)) void vm_write8_inl(uint64_t a, uint8_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(ms_store_fast(ea, 1), 1)) { *(volatile uint8_t*)(vm_base + ea) = v; return; }
    vm_write8(a, v);
}
static inline __attribute__((always_inline)) void vm_write16_inl(uint64_t a, uint16_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(ms_store_fast(ea, 2), 1)) {
        *(volatile ms_store_u16*)(vm_base + ea) = __builtin_bswap16(v);
        return;
    }
    vm_write16(a, v);
}
static inline __attribute__((always_inline)) void vm_write32_inl(uint64_t a, uint32_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(ms_store_fast(ea, 4), 1)) {
        *(volatile ms_store_u32*)(vm_base + ea) = __builtin_bswap32(v);
        return;
    }
    vm_write32(a, v);
}
static inline __attribute__((always_inline)) void vm_write64_inl(uint64_t a, uint64_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (__builtin_expect(ms_store_fast(ea, 8), 1)) {
        *(volatile ms_store_u64*)(vm_base + ea) = __builtin_bswap64(v);
        return;
    }
    vm_write64(a, v);
}

#define vm_write8(a, v)  vm_write8_inl((a), (v))
#define vm_write16(a, v) vm_write16_inl((a), (v))
#define vm_write32(a, v) vm_write32_inl((a), (v))
#define vm_write64(a, v) vm_write64_inl((a), (v))
