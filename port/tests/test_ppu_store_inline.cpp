#include "ppu_store_inline.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

uint8_t* vm_base;
int vm_inline_ok = 1;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base;
extern "C" unsigned char g_spu_coh_bitmap[1u << 22];
unsigned char g_spu_coh_bitmap[1u << 22];

static unsigned slow_calls;

static void slow_store(uint64_t a, uint64_t v, unsigned n)
{
    ++slow_calls;
    for (unsigned i = 0; i < n; ++i)
        vm_base[(uint32_t)a + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

#undef vm_write8
#undef vm_write16
#undef vm_write32
#undef vm_write64
extern "C" {
void vm_write8(uint64_t a, uint8_t v) { slow_store(a, v, 1); }
void vm_write16(uint64_t a, uint16_t v) { slow_store(a, v, 2); }
void vm_write32(uint64_t a, uint32_t v) { slow_store(a, v, 4); }
void vm_write64(uint64_t a, uint64_t v) { slow_store(a, v, 8); }
}
#define vm_write8(a, v)  vm_write8_inl((a), (v))
#define vm_write16(a, v) vm_write16_inl((a), (v))
#define vm_write32(a, v) vm_write32_inl((a), (v))
#define vm_write64(a, v) vm_write64_inl((a), (v))

static void store(unsigned n, uint64_t a, uint64_t v)
{
    switch (n) {
    case 1: vm_write8(a, (uint8_t)v); break;
    case 2: vm_write16(a, (uint16_t)v); break;
    case 4: vm_write32(a, (uint32_t)v); break;
    default: vm_write64(a, v); break;
    }
}

static void reserve(uint32_t ea) { g_spu_coh_bitmap[(ea >> 7) >> 3] |= (unsigned char)(1u << ((ea >> 7) & 7)); }

static void check(unsigned n, uint64_t a, bool expect_slow)
{
    const uint32_t ea = (uint32_t)a;
    const uint64_t v = 0x8877665544332211ull ^ ((uint64_t)ea * 0x9E3779B97F4A7C15ull);
    memset(vm_base + ea - 8, 0xA5, n + 16);
    const unsigned before = slow_calls;
    store(n, a, v);
    if ((slow_calls != before) != expect_slow) {
        fprintf(stderr, "store%u 0x%08x: slow=%d expected %d\n", n * 8, ea, slow_calls != before, expect_slow);
        abort();
    }
    for (unsigned i = 0; i < 8; ++i) assert(vm_base[ea - 8 + i] == 0xA5);
    for (unsigned i = 0; i < n; ++i) assert(vm_base[ea + i] == (uint8_t)(v >> (8 * (n - 1 - i))));
    for (unsigned i = 0; i < 8; ++i) assert(vm_base[ea + n + i] == 0xA5);
}

/* `off` is true when a diagnostic disables every fast store. */
static void check_all(bool off, uint32_t vm_size)
{
    static const unsigned widths[] = {1, 2, 4, 8};
    const uint32_t end = vm_size && vm_size < 0xE0000000u ? vm_size : 0xE0000000u;
    for (unsigned n : widths) {
        for (uint32_t o = 0; o < 8; ++o) {
            check(n, 0x10000u + o, off);
            check(n, 0x40000000u + o, off);
            check(n, (0x1234567ull << 32) | (0x40001000u + o), off);
            check(n, 0xFFF0u + o, true);
            check(n, 0xE0000000u + o, true);
            check(n, 0xE0040010u + o, true);
            check(n, 0xFFFFFFC0u + o, true);
        }
        // Last bytes below the end of the mapping (or the raw-SPU window).
        check(n, end - n, off);
        check(n, end - n + 1, true);
        check(n, end, true);

        // Reserved lines, on the first or the last line a store touches.
        memset(g_spu_coh_bitmap, 0, sizeof g_spu_coh_bitmap);
        reserve(0x40002000u);
        reserve(0x40003080u);
        check(n, 0x40002000u, true);
        check(n, 0x4000207Fu - (n - 1), true);
        check(n, 0x40002080u, off);
        check(n, 0x40001FF8u, off);
        if (n > 1) {
            check(n, 0x40001FFFu, true);              // last line reserved
            check(n, 0x4000307Fu, true);              // crosses into reserved line
            check(n, 0x400030FFu, true);              // first reserved, last clear
            check(n, 0x40002FFFu, off);               // both touched lines clear
        }
        check(n, 0x40003100u, off);
        memset(g_spu_coh_bitmap, 0, sizeof g_spu_coh_bitmap);
    }
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "plain";
    bool off = false;
    uint32_t vm_size = 0;
    if (!strcmp(mode, "size")) vm_size = 0x50000000u;
    else if (!strcmp(mode, "tiny")) { vm_size = 0x10000u; off = true; }
    else if (strcmp(mode, "plain")) { setenv(mode, "1", 1); off = true; }
    void* mem = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(mem != MAP_FAILED);
    vm_base = (uint8_t*)mem;
    ppu_vm_size = vm_size;
    if (!strcmp(mode, "tiny")) {
        check(4, 0x8000u, true);
        check(4, 0x40000000u, true);
    } else {
        check_all(off, vm_size);
    }
    // Configuration is read once, on the first store of the unit.
    if (off && strcmp(mode, "tiny")) {
        unsetenv(mode);
        check(4, 0x40000000u, true);
    }
    printf("PPU inline stores (%s): PASS\n", mode);
    return 0;
}
