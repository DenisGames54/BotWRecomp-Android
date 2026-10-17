// Native GPU backend: the latte.h contract (submission queue, retirement timestamps, flips, vsync,
// GX2 events) without Cemu. A GPU thread decodes the PM4 stream GX2 writes (pm4_decoder.cpp) and
// hands typed work to the Metal renderer (renderer.h).
#if defined(BOTW_NATIVE_GPU)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "../host/window.h"
#include "../log.h"
#include "../mem.h"
#include "gpu_native.h"
#include "pm4_decoder.h"
#include "renderer.h"

namespace gpu_native::backend {
namespace {
struct Submission {
    uint32_t addr, ndw;
    uint64_t ts;
};
std::mutex g_q_lock;
std::condition_variable g_q_cv;
std::deque<Submission> g_queue;
std::atomic<uint64_t> g_last_submitted{0}, g_retired{0};
std::mutex g_retire_lock;
std::condition_variable g_retire_cv;

std::mutex g_sync_lock;
std::condition_variable g_flip_cv, g_vsync_cv;
uint64_t g_vsync_count = 0;
std::atomic<uint32_t> g_flip_requests{0}, g_flips_executed{0}, g_flip_counter{0};
std::atomic<uint32_t> g_swap_interval{1};
std::atomic<uint64_t> g_last_flip_time{0}, g_last_vsync_time{0};
std::function<void(uint32_t)> g_sink;
bool g_started = false;
uint64_t g_frames = 0;

void raise(uint32_t e) {
    if (g_sink) g_sink(e);
}

void retire(uint64_t ts) {
    {
        std::lock_guard<std::mutex> l(g_retire_lock);
        if (ts > g_retired) g_retired = ts;
    }
    g_retire_cv.notify_all();
}

void gpu_thread() {
    Decoder dec;
    for (;;) {
        Submission s;
        {
            std::unique_lock<std::mutex> l(g_q_lock);
            if (g_queue.empty()) renderer::idle();  // flush recorded work while waiting
            g_q_cv.wait(l, [] { return !g_queue.empty(); });
            s = g_queue.front();
            g_queue.pop_front();
        }
        dec.execute(s.addr, s.ndw);
        // retire once the GPU work recorded so far has completed
        renderer::after_completion([ts = s.ts] { retire(ts); });
    }
}

// Vsync at 59.94 Hz. Flip accounting follows Cemu's LatteTiming_signalVsync for BotW
// (title 101c9400): one flip executed per vsync interval while requests are pending.
void vsync_thread() {
    using clk = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1001000000ll / 60);
    auto next = clk::now() + period;
    uint32_t interval_counter = 0;
    for (;;) {
        std::this_thread::sleep_until(next);
        next += period;
        uint64_t now = rt_timebase();
        if (++interval_counter >= std::max<uint32_t>(1, g_swap_interval)) {
            interval_counter = 0;
            uint32_t req = g_flip_requests, exe = g_flips_executed;
            if (req >= exe || exe - req < 4) g_flips_executed++;
            g_flip_counter++;
            g_last_flip_time = now;
            uint64_t f = ++g_frames;
            window::on_frame(f);  // frame counter for --script / --trace-frames / --seconds
            if (f % 300 == 0) LOG("frame %llu", (unsigned long long)f);
            raise(3);  // FLIP
            std::lock_guard<std::mutex> l(g_sync_lock);
            g_flip_cv.notify_all();
        }
        g_last_vsync_time = now;
        static uint64_t ticks = 0;
        if (++ticks % 300 == 0) {
            size_t q;
            {
                std::lock_guard<std::mutex> l(g_q_lock);
                q = g_queue.size();
            }
            using namespace renderer;
            LOG("gpu_native: submitted %llu retired %llu queued %zu | ms in last 5 s: draw %.0f (shader %.0f, pipeline %.0f, "
                "texture %.0f, index %.0f), fence wait %.0f",
                (unsigned long long)g_last_submitted.load(), (unsigned long long)g_retired.load(), q, g_time_ms[T_DRAW],
                g_time_ms[T_SHADER], g_time_ms[T_PIPELINE], g_time_ms[T_TEXTURE], g_time_ms[T_INDEX], g_time_ms[T_WAIT_MEM]);
            for (double& t : g_time_ms) t = 0;
        }
        raise(2);  // VSYNC
        {
            std::lock_guard<std::mutex> l(g_sync_lock);
            g_vsync_count++;
        }
        g_vsync_cv.notify_all();
    }
}
}  // namespace

// called by the decoder for HLE_WAIT_FOR_FLIP
void gpu_wait_for_flip_counter_change() {
    uint32_t c = g_flip_counter;
    std::unique_lock<std::mutex> l(g_sync_lock);
    g_flip_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return g_flip_counter != c; });
}
void gpu_raise_event(uint32_t e) { raise(e); }

void create_renderer(void* nsview, int w, int h) { renderer::create(nsview, w, h); }
void resize_window(int w, int h) { renderer::resize(w, h); }
void set_scanout_srgb(bool tv, bool srgb) { renderer::set_scanout_srgb(tv, srgb); }
void set_display_mode(int) {}  // no on-screen output yet
void set_capture(int every, const char* dir) { renderer::set_capture(every, dir); }

void init() {
    if (g_started) return;
    g_started = true;
    LOG("gpu_native: native Metal backend (no Cemu Latte)");
    std::thread(gpu_thread).detach();
    std::thread(vsync_thread).detach();
}

uint64_t submit(uint32_t addr, uint32_t ndw) {
    uint64_t ts;
    {
        std::lock_guard<std::mutex> l(g_q_lock);
        ts = ++g_last_submitted;
        g_queue.push_back({addr, ndw, ts});
    }
    g_q_cv.notify_one();
    return ts;
}
uint64_t last_submitted() { return g_last_submitted; }
uint64_t retired() { return g_retired; }
void wait_retired(uint64_t ts) {
    std::unique_lock<std::mutex> l(g_retire_lock);
    g_retire_cv.wait(l, [&] { return g_retired >= ts; });
}
uint32_t flip_requests() { return g_flip_requests; }
uint32_t flips_executed() { return g_flips_executed; }
void add_flip_request() { g_flip_requests++; }
void wait_for_flip() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    if (g_flip_requests == g_flips_executed) return;
    g_flip_cv.wait_for(l, std::chrono::milliseconds(100));
}
void wait_for_vsync() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    uint64_t v = g_vsync_count;
    g_vsync_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return g_vsync_count != v; });
}
uint64_t last_flip_time() { return g_last_flip_time; }
uint64_t last_vsync_time() { return g_last_vsync_time; }
uint32_t swap_interval() { return g_swap_interval; }
void set_swap_interval(uint32_t v) { g_swap_interval = v; }
void set_event_sink(std::function<void(uint32_t)> sink) { g_sink = std::move(sink); }
}  // namespace gpu_native::backend
#endif  // BOTW_NATIVE_GPU
