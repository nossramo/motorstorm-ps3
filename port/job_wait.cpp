// Optional host wait for MotorStorm's six-lane WWS job completion barrier.
// The original PPU loop still makes the final decision and sets guest registers.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/passive_waits.h"

/* Same layout as spurs_policy.c. A strong definition there overrides this
 * empty body when the SPURS runtime is linked. */
extern "C" {
struct SpursPolicySight { uint32_t image, pc, wid, lane; };
__attribute__((weak)) int spurs_policy_sample(SpursPolicySight* out, int cap) {
    (void)out; (void)cap; return 0;
}
__attribute__((weak)) const char* motorstorm_job_chain_dir(void) { return nullptr; }
}

extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint16_t vm_read16(uint64_t);
extern "C" void (*g_spu_host_line_commit_hook)(uint32_t);

namespace {
constexpr unsigned slots = 16;
std::mutex wait_mutex;
std::condition_variable changed;
std::atomic<uint64_t> watched[slots]{}; // 1 + line address; zero means vacant.
bool enabled;
bool chain;
constexpr unsigned chain_buckets = 1024;
struct ChainBucket { uint32_t image, pc, hits; };
ChainBucket buckets[chain_buckets];
uint32_t lane_behind[6];
uint32_t chain_pauses, chain_idle, chain_matched, chain_seen;
uint64_t chain_wait_us;
std::chrono::steady_clock::time_point chain_flush{};

const char* chain_image_name(uint32_t image) {
    static const char* names[] = {
        "eA948", "e2130", "e6240", "e1540", "e12C10",
        "e12230", "e1CD90", "eAF80", "e7960", "e12E10"};
    if (image >= 200 && image < 210) return names[image - 200];
    return nullptr;
}

void chain_add(uint32_t image, uint32_t pc) {
    pc &= ~31u;
    uint32_t slot = (image * 16777619u ^ pc) & (chain_buckets - 1);
    for (unsigned n = 0; n < 16; n++) {
        ChainBucket& bucket = buckets[slot];
        if (bucket.hits == 0) { bucket.image = image; bucket.pc = pc; bucket.hits = 1; return; }
        if (bucket.image == image && bucket.pc == pc) { bucket.hits++; return; }
        slot = (slot + 1) & (chain_buckets - 1);
    }
}

void chain_flush_line() {
    const char* dir = motorstorm_job_chain_dir();
    if (!dir || !dir[0]) return;
    char path[1024];
    if (std::snprintf(path, sizeof path, "%s/job-chain.txt", dir) >= (int)sizeof path) return;
    static bool truncated = false;
    FILE* file = std::fopen(path, truncated ? "a" : "w");
    if (!file) return;
    truncated = true;
    std::fprintf(file, "CHAIN pauses=%u wait_ms=%llu matched=%u idle=%u seen=%u lanes=%u,%u,%u,%u,%u,%u",
                 chain_pauses, (unsigned long long)(chain_wait_us / 1000),
                 chain_matched, chain_idle, chain_seen,
                 lane_behind[0], lane_behind[1], lane_behind[2],
                 lane_behind[3], lane_behind[4], lane_behind[5]);
    unsigned pick[8];
    unsigned shown = 0;
    for (; shown < 8; shown++) {
        uint32_t best = 0;
        unsigned which = chain_buckets;
        for (unsigned i = 0; i < chain_buckets; i++) {
            bool used = false;
            for (unsigned earlier = 0; earlier < shown; earlier++) if (pick[earlier] == i) used = true;
            if (used || buckets[i].hits <= best) continue;
            best = buckets[i].hits;
            which = i;
        }
        if (which == chain_buckets) break;
        pick[shown] = which;
    }
    for (unsigned rank = 0; rank < shown; rank++) {
        const ChainBucket& bucket = buckets[pick[rank]];
        const char* name = chain_image_name(bucket.image);
        if (name) std::fprintf(file, " %s+0x%X:%u", name, bucket.pc, bucket.hits);
        else std::fprintf(file, " img%u+0x%X:%u", bucket.image, bucket.pc, bucket.hits);
    }
    std::fputc('\n', file);
    std::fclose(file);
    chain_flush = std::chrono::steady_clock::now();
}

unsigned chain_behind(uint32_t base, uint32_t target) {
    uint16_t minimum = 0xffff;
    uint16_t lane[6];
    for (unsigned i = 0; i < 6; i++) {
        lane[i] = vm_read16(uint64_t(base) + 0x14 + 2u * i);
        if (lane[i] < minimum) minimum = lane[i];
    }
    if (minimum >= target) return 0;
    unsigned behind = 0;
    for (unsigned i = 0; i < 6; i++) if (lane[i] == minimum) behind |= 1u << i;
    return behind;
}

void chain_record(unsigned behind, const SpursPolicySight* sight, int count, uint64_t wait_us) {
    chain_pauses++;
    chain_wait_us += wait_us;
    for (unsigned i = 0; i < 6; i++) if ((behind >> i) & 1u) lane_behind[i]++;
    int matched = 0;
    for (int i = 0; i < count; i++) {
        if (((behind >> sight[i].lane) & 1u) == 0) continue;
        chain_add(sight[i].image, sight[i].pc);
        matched++;
    }
    if (count > 0) chain_seen++;
    if (matched) chain_matched++;
    else chain_idle++;
    const auto now = std::chrono::steady_clock::now();
    if (chain_flush.time_since_epoch().count() == 0) chain_flush = now;
    if (now - chain_flush >= std::chrono::seconds(2)) chain_flush_line();
}

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

/* Strong definition in the SPURS runtime overrides this. Tests that do not
 * link that runtime keep the empty body. */
extern "C" __attribute__((weak)) void ps3_spurs_workers_boost(int) {}

extern "C" void motorstorm_job_wait_init() {
    const char* mode = std::getenv("MOTORSTORM_JOB_EVENT_WAIT");
    enabled = mode && *mode == '1';
    const char* chain_mode = std::getenv("MOTORSTORM_JOB_CHAIN");
    chain = chain_mode && chain_mode[0] == '1' && chain_mode[1] == '\0';
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
    unsigned behind = 0;
    SpursPolicySight sight[32];
    int sight_count = 0;
    uint64_t wait_us = 0;
    if (pending(object, base, target)) {
        std::chrono::steady_clock::time_point wait_start{};
        if (chain) {
            wait_start = std::chrono::steady_clock::now();
            /* Sample the lagging lanes before sleeping. After the wake their
             * counters have usually caught up and the job is already gone. */
            behind = chain_behind(base, target);
            if (behind) sight_count = spurs_policy_sample(sight, 32);
        }
        /* The waited-for SPU work can use a performance core only while this
         * thread is asleep. The boost is dropped again before guest code runs. */
        ps3_spurs_workers_boost(1);
        reason = changed.wait_for(lock, std::chrono::microseconds(250)) ==
                 std::cv_status::timeout ? 2 : 1;
        ps3_spurs_workers_boost(0);
        if (chain) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - wait_start).count();
            wait_us = elapsed > 0 ? (uint64_t)elapsed : 0;
        }
    }
    watched[first].store(0, std::memory_order_release);
    watched[second].store(0, std::memory_order_release);
    ps3_wait_record(PW_JOB_PAUSE, passive_start, reason);
    lock.unlock();
    if (chain && behind) chain_record(behind, sight, sight_count, wait_us);
    // Port-local kind 24: who=base, a=0 complete / 1 notified / 2 fallback,
    // b=target. It is nested within the existing kind-20 CPU/wall interval.
    if (trace_start) ps3_ft_span(24, base, reason, target, trace_start, ps3_ft_now());
}
