// Guest threads: every OSThread runs on its own host thread with its own PPCContext.
#pragma once
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "ppc.h"

namespace threads {
// OSThread field offsets (Cemu coreinit_Thread.h)
constexpr uint32_t kOSThreadSize = 0x6A0;
constexpr uint32_t T_AFFINITY = 0x304, T_MAGIC = 0x320, T_STATE = 0x324, T_ATTR = 0x325, T_ID = 0x326,
                   T_SUSPEND = 0x328, T_EFFPRIO = 0x32C, T_BASEPRIO = 0x330, T_EXITVAL = 0x334,
                   T_STACKBASE = 0x394, T_STACKEND = 0x398, T_ENTRY = 0x39C, T_SPECIFIC = 0x57C, T_TYPE = 0x5BC,
                   T_NAME = 0x5C0, T_USERSP = 0x5C8;

struct GuestThread {
    uint32_t os = 0;  // guest OSThread*
    PPCContext ctx{};
    std::thread host;
    std::string name;
    int suspend = 1;
    bool started = false, exited = false;
    bool is_main = false;
    pthread_t host_tid{};  // for watchdog stack dumps
    uint32_t exit_value = 0;
    uint32_t entry = 0;
    int core = 1;
    std::condition_variable cv;  // join / resume waiters
    // what the thread is blocked on, for dump()
    std::atomic<const char*> wait_kind{nullptr};
    uint32_t wait_obj = 0, wait_lr = 0;
};

// The kernel lock guards all scheduler and synchronization-object state.
extern std::mutex g_klock;

GuestThread* current();
const char* current_name();
GuestThread* get(uint32_t os);
// Wraps a host thread that is not a guest thread (alarm/audio/GPU callbacks) so it can run guest code.
GuestThread* adopt_host_thread(const char* name, uint32_t stack_size, int core);
void init_context(PPCContext* c, uint32_t stack_top, int core);
// Run the RPX entry on a new host thread as the default thread of core 1.
void start_main(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size);
// Host thread entry point id for logs.
uint32_t sda_base();
// Log every live guest thread and what it is blocked on; with stacks, running threads also log their host
// call stack (symbolize with tools/symbolize.sh: #line = guest address).
void dump(bool stacks = false);
// Log one host call stack of a non-guest thread (e.g. Cemu's GPU thread) as "<label> host frames".
void sample_host(pthread_t th, const char* label);

// Main-thread blocked time per wait kind (frame pacing analysis); report() returns and resets it.
void note_main_wait(const char* kind, uint64_t ns);
std::string main_wait_report();

// Marks the current guest thread as blocked for the scope of a wait (shown by dump(); the main
// thread's waits are also timed for main_wait_report()).
struct WaitMark {
    GuestThread* t;
    const char* kind;
    std::chrono::steady_clock::time_point t0;
    WaitMark(const char* k, uint32_t obj) : t(current()), kind(k) {
        if (!t) return;
        t->wait_obj = obj;
        t->wait_lr = t->ctx.lr;
        t->wait_kind = k;
        if (t->is_main) t0 = std::chrono::steady_clock::now();
    }
    ~WaitMark() {
        if (!t) return;
        t->wait_kind = nullptr;
        if (t->is_main)
            note_main_wait(kind, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
};
}  // namespace threads
