#include "ps3emu/guest_read_sync.h"
#include <atomic>
#include <thread>
#include <cassert>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <condition_variable>
extern "C" void motorstorm_gpu_sync_init();
extern "C" void motorstorm_gpu_sync_service();
extern "C" int motorstorm_gpu_sync_pending();
static std::thread::id render_thread;
static std::atomic<unsigned> calls;
static std::mutex wake_mutex;
static std::condition_variable wake_changed;
static bool wake_pending;
extern "C" void cellGcm_fifo_kick() {
    std::lock_guard<std::mutex> lock(wake_mutex);
    wake_pending = true;
    wake_changed.notify_one();
}
extern "C" void rsx_draw_engine_cpu_read(uint32_t address, uint32_t bytes) {
    assert(std::this_thread::get_id() == render_thread);
    assert(address == 0x12fff && bytes == 2);
    ++calls;
}
int main() {
    setenv("RSX_ENGINE_CPU_READBACK", "demand", 1);
    render_thread = std::this_thread::get_id();
    motorstorm_gpu_sync_init();
    assert(!motorstorm_gpu_sync_pending());
    ps3_gpu_read_sync(0x12fff, 2);
    assert(calls == 0);
    ps3_gpu_sync_mark(0x13000, 1, 1); // only second page of crossing load
    std::atomic<bool> done{false};
    std::thread guest([&] { ps3_gpu_read_sync(0x12fff, 2); done = true; });
    {
        std::unique_lock<std::mutex> lock(wake_mutex);
        assert(wake_changed.wait_for(lock, std::chrono::seconds(2), [] { return wake_pending; }));
    }
    assert(motorstorm_gpu_sync_pending());
    motorstorm_gpu_sync_service();
    assert(!motorstorm_gpu_sync_pending());
    guest.join();
    assert(done);
    assert(calls == 1);
    ps3_gpu_sync_mark(0x13000, 64, 0);
    ps3_gpu_read_sync(0x12fff, 2);
    assert(calls == 1);
}
