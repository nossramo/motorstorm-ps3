#pragma once
/* Platform scheduling only: the checked guest wait/CAS/post bodies and their
 * memory operations remain intact. A timed compare-and-wait follows the
 * original backoff; the producer wakes waiters after publishing a token.
 * Uninstrumented writers and pointer replacement are covered by the timeout.
 * https://github.com/apple-oss-distributions/libplatform/blob/main/include/os/os_sync_wait_on_address.h
 */
#include "ppu_recomp.h"
#include <atomic>
#include <cstdlib>
#include <os/os_sync_wait_on_address.h>

static inline bool ms_counting_wait_enabled()
{
#ifdef MOTORSTORM_COUNTING_WAIT_TEST
    extern bool ms_counting_wait_test_enabled;
    return ms_counting_wait_test_enabled;
#else
    static const bool enabled = [] {
        const char* v = getenv("MOTORSTORM_COUNTING_WAIT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return enabled;
#endif
}

static inline void* ms_counting_wait_address(uint64_t address)
{
    if (!vm_base || address != (uint32_t)address || (address & 3) ||
        !vm_inl_ok((uint32_t)address, 4)) return nullptr;
    return vm_base + address;
}

static inline void ms_counting_wait_park(const ppu_context* ctx)
{
    if (!ms_counting_wait_enabled()) return;
    const uint64_t object = ctx->gpr[3];
    if (object > UINT32_MAX - 4 || !ms_counting_wait_address(object + 4)) return;
    void* address = ms_counting_wait_address(vm_read32(object + 4));
    if (!address || __atomic_load_n((uint32_t*)address, __ATOMIC_RELAXED)) return;
#ifdef MOTORSTORM_COUNTING_WAIT_TEST
    extern void ms_counting_wait_test_before_park(void*);
    ms_counting_wait_test_before_park(address);
#endif
    /* Zero has identical guest/host byte order. The kernel atomically checks
     * it again before sleeping, so a post before the wait cannot be lost.
     * Any result (wake, timeout, interrupt, error) resumes the guest loop. */
    (void)os_sync_wait_on_address_with_timeout(address, 0, 4,
        OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, 250000);
}

static inline void ms_counting_wait_wake(const ppu_context* ctx)
{
    /* Only zero-to-one can leave waiters asleep. Wake all: posts can be
     * batched and every waiter still competes using the original guest CAS. */
    if (ctx->gpr[11] || !ms_counting_wait_enabled()) return;
    void* address = ms_counting_wait_address(ctx->gpr[9]);
    if (address) (void)os_sync_wake_by_address_all(address, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}
