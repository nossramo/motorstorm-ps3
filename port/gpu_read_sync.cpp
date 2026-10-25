// CPU requests are serviced by the thread that owns Metal/the RSX engine.
#include "ps3emu/guest_read_sync.h"
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/passive_waits.h"
#include "rsx_draw_engine.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
extern "C" void cellGcm_fifo_kick(void);
namespace {
// Guest process exit can run C++ static destructors while the render and PPU
// workers are still alive. Keep their rendezvous valid until OS process
// teardown; destroying it early turns a guest exit into a host mutex abort.
// There is one pair per process, reclaimed with the process itself.
std::mutex& mutex = *new std::mutex;
std::condition_variable& changed = *new std::condition_variable;
std::thread::id owner;
struct Request { uint32_t address, bytes; bool done = false; Request* next = nullptr; };
Request* head;
Request* tail;
std::atomic<bool> pending{false};
void request(uint32_t address, uint32_t bytes) {
    if (std::this_thread::get_id() == owner) {
        rsx_draw_engine_cpu_read(address, bytes);
        return;
    }
    const uint64_t trace_start = PS3_FT_ON() ? ps3_ft_now() : 0;
    const uint64_t passive_start = ps3_metric_begin();
    Request r{address, bytes};
    std::unique_lock<std::mutex> lock(mutex);
    if (tail) tail->next = &r; else head = &r;
    tail = &r;
    pending.store(true, std::memory_order_release);
    // Publish the request before waking its render-thread owner. The event
    // retains a signal even if the owner has not started its idle wait yet.
    cellGcm_fifo_kick();
    // GPU capture instruments shader compilation and can exceed the normal
    // watchdog during a scene load. Keep this opt-in, bounded and fail closed.
    static const auto timeout = [] {
        const char* value = std::getenv("MOTORSTORM_GPU_READ_TIMEOUT_SECONDS");
        long seconds = value ? std::strtol(value, nullptr, 10) : 10;
        if (seconds < 10) seconds = 10;
        if (seconds > 120) seconds = 120;
        return std::chrono::seconds(seconds);
    }();
    if (!changed.wait_for(lock, timeout, [&] { return r.done; })) {
        std::fprintf(stderr, "GPU CPU-read service timeout at %08X\n", address);
        std::abort(); // Never continue with an unfulfilled read or dangling request.
    }
    // Port-local kind 25 measures the whole CPU dependency, including queue
    // delay; the backend readback span alone omits that often dominant wait.
    if (trace_start) ps3_ft_span(25, address, bytes, 0, trace_start, ps3_ft_now());
    ps3_wait_record(PW_GPU_READ, passive_start, 0);
}
}
extern "C" void motorstorm_gpu_sync_init() {
    const char* mode = std::getenv("RSX_ENGINE_CPU_READBACK");
    if (!mode || std::strcmp(mode, "demand")) return;
    owner = std::this_thread::get_id();
    ps3_gpu_sync_set_handler(request);
}
extern "C" void motorstorm_gpu_sync_service() {
    std::unique_lock<std::mutex> lock(mutex);
    while (head) {
        Request* r = head;
        head = r->next;
        if (!head) {
            tail = nullptr;
            pending.store(false, std::memory_order_release);
        }
        lock.unlock();
        rsx_draw_engine_cpu_read(r->address, r->bytes);
        lock.lock();
        r->done = true;
        changed.notify_all();
    }
}
extern "C" int motorstorm_gpu_sync_pending() {
    return pending.load(std::memory_order_acquire);
}
