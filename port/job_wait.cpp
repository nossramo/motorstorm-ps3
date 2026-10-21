// Optional host wait for MotorStorm's six-lane WWS job completion barrier.
// The original PPU loop still makes the final decision and sets guest registers.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/passive_waits.h"

extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint16_t vm_read16(uint64_t);
extern "C" void (*g_spu_host_line_commit_hook)(uint32_t);

namespace {
constexpr unsigned slots = 16;
std::mutex wait_mutex;
std::condition_variable changed;
std::atomic<uint64_t> watched[slots]{}; // 1 + line address; zero means vacant.
bool enabled;

void line_committed(uint32_t line) {
    const uint64_t key = uint64_t(line & ~127u) + 1;
    for (auto& address : watched) {
        if (address.load(std::memory_order_acquire) != key) continue;
        // Taking the predicate mutex closes the check-to-sleep wakeup race.
        std::lock_guard lock(wait_mutex);
        changed.notify_all();
        return;
    }
}

bool pending(uint32_t object, uint32_t base, uint32_t target) {
    // Pointer replacement requires the original loop to re-read the object.
    if (vm_read32(uint64_t(object) + 4) != base) return false;
    uint32_t minimum = vm_read16(uint64_t(base) + 0x12);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (unsigned lane = 0; lane < 6; ++lane) {
        const uint32_t value = vm_read16(uint64_t(base) + 0x14 + 2 * lane);
        if (value < minimum) minimum = value;
    }
    return target > minimum;
}
}

extern "C" void motorstorm_job_wait_init() {
    const char* mode = std::getenv("MOTORSTORM_JOB_EVENT_WAIT");
    enabled = mode && *mode == '1';
    if (enabled) g_spu_host_line_commit_hook = line_committed;
}

extern "C" void motorstorm_job_wait_pause(uint32_t object, uint32_t base,
                                           uint32_t target) {
    if (!enabled || base > UINT32_MAX - 0x20u || object > UINT32_MAX - 8u) return;
    const uint64_t trace_start = PS3_FT_ON() ? ps3_ft_now() : 0;
    const uint64_t passive_start = ps3_metric_begin();
    std::unique_lock lock(wait_mutex);
    unsigned first = slots, second = slots;
    for (unsigned i = 0; i < slots; ++i) {
        if (watched[i].load(std::memory_order_relaxed)) continue;
        if (first == slots) first = i;
        else { second = i; break; }
    }
    if (second == slots) {
        ps3_wait_record(PW_JOB_PAUSE, passive_start, 3);
        return; // Capacity exhaustion retains the original spin.
    }
    watched[first].store(uint64_t((base + 0x12) & ~127u) + 1, std::memory_order_release);
    watched[second].store(uint64_t((base + 0x1f) & ~127u) + 1, std::memory_order_release);
    // Only suspend after rechecking all seven counters with the watch armed.
    // A bounded fallback also covers ordinary DMA/PPU stores (no atomic hook),
    // spurious notifications and unsupported writers, without bypassing work.
    unsigned reason = 0;
    if (pending(object, base, target))
        reason = changed.wait_for(lock, std::chrono::microseconds(250)) ==
                 std::cv_status::timeout ? 2 : 1;
    watched[first].store(0, std::memory_order_release);
    watched[second].store(0, std::memory_order_release);
    ps3_wait_record(PW_JOB_PAUSE, passive_start, reason);
    // Port-local kind 24: who=base, a=0 complete / 1 notified / 2 fallback,
    // b=target. It is nested within the existing kind-20 CPU/wall interval.
    if (trace_start) ps3_ft_span(24, base, reason, target, trace_start, ps3_ft_now());
}
