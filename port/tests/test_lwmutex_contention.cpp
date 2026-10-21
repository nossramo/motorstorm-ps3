// Exercise the context-aware CRT handler used by the recompiled game, rather
// than the separate generic sysPrxForUser C implementation.
#include "../../vendor/ps3recomp/runtime/ppu/ppu_sysprx.cpp"
#include "ps3emu/error_codes.h"
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>

extern "C" {
uint8_t* vm_base;
volatile long long g_gcm_ref_pub_count = 0;
unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
unsigned ppu_thread_prof_pc(unsigned) { return 0; }
uint32_t vm_read32(uint64_t a) {
    return __builtin_bswap32(__atomic_load_n((uint32_t*)(vm_base + (uint32_t)a), __ATOMIC_RELAXED));
}
uint64_t vm_read64(uint64_t a) {
    return __builtin_bswap64(__atomic_load_n((uint64_t*)(vm_base + (uint32_t)a), __ATOMIC_RELAXED));
}
void vm_write32(uint64_t a, uint32_t v) {
    __atomic_store_n((uint32_t*)(vm_base + (uint32_t)a), __builtin_bswap32(v), __ATOMIC_RELAXED);
}
void vm_write64(uint64_t a, uint64_t v) {
    __atomic_store_n((uint64_t*)(vm_base + (uint32_t)a), __builtin_bswap64(v), __ATOMIC_RELAXED);
}
}

int main() {
    vm_base = (uint8_t*)calloc(1, 0x10000);
    constexpr uint32_t address = 0x1000;
    ppu_context owner{};
    owner.thread_id = 1; owner.gpr[3] = address;
    sys_lwmutex_create(&owner);
    owner.gpr[3] = address;
    sys_lwmutex_lock(&owner);
    assert(owner.gpr[3] == CELL_OK);

    std::atomic<bool> waiting{false}, entered{false};
    std::thread contender([&] {
        ppu_context ctx{}; ctx.thread_id = 2; ctx.gpr[3] = address;
        sys_lwmutex_trylock(&ctx);
        assert((uint32_t)ctx.gpr[3] == CELL_EBUSY);
        assert(vm_read32(address) == 1 && vm_read32(address + 12) == 1);
        ctx.gpr[3] = address; ctx.gpr[4] = 1000;
        sys_lwmutex_lock(&ctx);
        assert((uint32_t)ctx.gpr[3] == CELL_ETIMEDOUT);
        assert(vm_read32(address) == 1);
        // This is the game's busy -> blocking lock fallback. It must never
        // enter the file/decompression critical section before owner unlocks.
        waiting = true;
        ctx.gpr[3] = address; ctx.gpr[4] = 0;
        sys_lwmutex_lock(&ctx);
        assert(ctx.gpr[3] == CELL_OK);
        entered = true;
        ctx.gpr[3] = address; sys_lwmutex_unlock(&ctx);
    });
    while (!waiting) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!entered);
    owner.gpr[3] = address; sys_lwmutex_unlock(&owner);
    contender.join();
    assert(entered && vm_read32(address) == 0);
    owner.gpr[3] = address; sys_lwmutex_trylock(&owner);
    assert(owner.gpr[3] == CELL_OK);
    owner.gpr[3] = address; sys_lwmutex_unlock(&owner);
    free(vm_base);
    puts("CRT lwmutex: busy code, timeout, ownership and serialized handoff PASS");
}
