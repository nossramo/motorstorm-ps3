#include "ppu_counting_wait.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#undef vm_read32
alignas(16) static uint8_t memory[0x100000];
extern "C" {
uint8_t* vm_base = memory;
int vm_inline_ok = 1;
uint32_t ppu_vm_size = sizeof memory, ppu_hle_inject_base = 0;
unsigned char ps3_gpu_dirty_pages[1u << 20];
void ps3_gpu_sync_dispatch(uint32_t, uint32_t) { abort(); }
}
bool ms_counting_wait_test_enabled = true;
static std::atomic<unsigned> backoffs, parks;
static unsigned hook;
static constexpr uint32_t object = 0x10000, counter = 0x11000, other = 0x12000;
extern "C" uint32_t vm_read32(uint64_t address)
{
    assert(address + 4 <= sizeof memory && !(address & 3));
    return __builtin_bswap32(__atomic_load_n((uint32_t*)(memory + address), __ATOMIC_RELAXED));
}
extern "C" void vm_write32(uint64_t address, uint32_t value)
{
    assert(address + 4 <= sizeof memory && !(address & 3));
    __atomic_store_n((uint32_t*)(memory + address), __builtin_bswap32(value), __ATOMIC_RELAXED);
    if (address >= 0x20000) ++backoffs;
}
extern "C" int ppu_stwcx32(uint64_t address, uint32_t expected, uint32_t value)
{
    expected = __builtin_bswap32(expected);
    return __atomic_compare_exchange_n((uint32_t*)(memory + address), &expected,
        __builtin_bswap32(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
void ms_counting_wait_test_before_park(void*)
{
    ++parks;
    if (hook == 1) vm_write32(counter, 1); // Post between user check and kernel compare.
    if (hook == 2) { vm_write32(other, 1); vm_write32(object + 4, other); }
}
#include "counting_wait.inc"

static ppu_context context(unsigned index = 0)
{
    ppu_context c{};
    for (unsigned i = 0; i < 32; ++i) c.gpr[i] = (0x9e3779b9ull * (i + 1)) ^ 0x12345678abcdefull;
    c.gpr[3] = object; c.gpr[1] = 0x40000 + index * 0x100;
    c.cr = 0x12345678; c.xer = 0x87654321; c.reserve_addr = 0x99999;
    return c;
}
static void reset(uint32_t value)
{
    memset(memory, 0x57, sizeof memory);
    vm_write32(object + 4, counter); vm_write32(counter, value);
    backoffs = parks = 0; hook = 0;
}
static uint64_t cpu_ns()
{
    timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return uint64_t(t.tv_sec)*1000000000 + t.tv_nsec;
}
int main()
{
    unsigned checks = 0;
    // Fast acquisitions, signed/overflow values and no-op/batched producers.
    for (uint32_t value : {1u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        reset(value); auto a = context(), b = a;
        reference_00B9FBE4(&a); uint32_t want = vm_read32(counter);
        reset(value); func_00B9FBE4(&b);
        assert(!memcmp(&a, &b, sizeof a) && vm_read32(counter) == want && !parks); ++checks;
        for (uint32_t n : {0u, 1u, 4u, 0xffffffffu, 0x80000000u}) {
            reset(value); a = context(); a.gpr[4] = n; b = a;
            reference_00B9FC50(&a); want = vm_read32(counter);
            reset(value); func_00B9FC50(&b);
            assert(!memcmp(&a, &b, sizeof a) && vm_read32(counter) == want); ++checks;
        }
    }
    // Real producer/waiter execution; baseline and candidate observe the
    // same delayed token and must leave the exact same guest state.
    uint64_t elapsed[2] = {};
    ppu_context finished[2];
    for (unsigned mode = 0; mode < 2; ++mode) {
        reset(0); ms_counting_wait_test_enabled = mode;
        auto waiter = context();
        std::thread producer([] {
            while (!backoffs.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto post = context(1); post.gpr[4] = 1; func_00B9FC50(&post);
        });
        uint64_t start = cpu_ns(); func_00B9FBE4(&waiter); elapsed[mode] = cpu_ns() - start;
        producer.join(); finished[mode] = waiter;
        assert(vm_read32(counter) == 0 && vm_read32(waiter.gpr[1]-32) == 0);
        assert(!mode || parks > 0);
    }
    assert(!memcmp(finished, finished + 1, sizeof(ppu_context))); ++checks;
    // Wake-before-wait and pointer replacement need no producer callback.
    for (unsigned test : {1u, 2u}) {
        reset(0); hook = test; auto c = context(); func_00B9FBE4(&c);
        assert(parks > 0 && vm_read32(test == 1 ? counter : other) == 0); ++checks;
    }
    // An unknown writer cannot strand the waiter: timed recheck finds it.
    reset(0); auto c = context();
    std::thread uninstrumented([] {
        while (!parks.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        vm_write32(counter, 1);
    });
    func_00B9FBE4(&c); uninstrumented.join(); assert(vm_read32(counter) == 0); ++checks;
    // Concurrent waiters compete for exactly the posted number of tokens.
    for (unsigned trial = 0; trial < 40; ++trial) {
        reset(0); std::atomic<unsigned> done{0}; std::vector<std::thread> waiters;
        for (unsigned i = 0; i < 4; ++i) waiters.emplace_back([&, i] {
            auto c = context(i); func_00B9FBE4(&c); ++done;
        });
        while (parks < 4) std::this_thread::yield();
        auto post = context(5); post.gpr[4] = 4; func_00B9FC50(&post);
        for (auto& thread : waiters) thread.join();
        assert(done == 4 && vm_read32(counter) == 0); ++checks;
    }
    reset(0); vm_inline_ok = 0; c = context(); ms_counting_wait_park(&c); assert(!parks); vm_inline_ok = 1;
    assert(!ms_counting_wait_address(counter + 1));
    assert(!ms_counting_wait_address(sizeof memory));
    printf("PASS %u state/concurrency cases: original CAS, fast/negative counts, batched posts, wake-before-wait, pointer replacement, unknown writers and observer guards\n", checks);
    printf("20 ms idle wait: thread CPU spin %.3f ms, parked %.3f ms (host scheduling diagnostic, not game FPS)\n", elapsed[0]/1e6, elapsed[1]/1e6);
}
