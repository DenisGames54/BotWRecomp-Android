#include "thread.h"

#include <execinfo.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <unordered_map>
#include <vector>

#include "hle.h"
#include "loader.h"

#include <TargetConditionals.h>
#if TARGET_OS_IOS  // iOS port: stack size vs. the VA limit without extended VA (ios/src/vm_ios.cpp)
size_t botw_ios_stack_size(size_t want);
#endif

namespace threads {
std::mutex g_klock;
namespace {
thread_local GuestThread* t_current = nullptr;
std::unordered_map<uint32_t, GuestThread*> g_threads;  // OSThread* -> thread
uint16_t g_next_id = 1;
constexpr size_t kHostStack = 64u << 20;  // guest recursion maps onto the host stack

void run_thread(GuestThread* t) {
    t_current = t;
    t->host_tid = pthread_self();
    // The game's main thread and the light timing threads (audio, alarms, GX2 callbacks) get the highest QoS so
    // the scheduler keeps them on performance cores (a phone has two); worker threads one step below.
    const bool critical = t->is_main || t->name == "AX" || t->name == "alarms" || t->name == "GX2 callbacks";
    pthread_set_qos_class_self_np(critical ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
    pthread_setname_np(t->name.empty() ? "guest" : t->name.c_str());
    PPCContext* c = &t->ctx;
    c->lr = 0;
    rt_call(c, t->entry);
    uint32_t v = c->r[3];
    std::lock_guard<std::mutex> l(g_klock);
    t->exited = true;
    t->exit_value = v;
    mem::w32(t->os + T_EXITVAL, v);
    mem::w8(t->os + T_STATE, 8);  // moribund
    t->cv.notify_all();
    LOG("thread %s exited (%08x)", t->name.c_str(), v);
}

void launch(GuestThread* t) {
    t->started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if TARGET_OS_IOS
    pthread_attr_setstacksize(&attr, botw_ios_stack_size(kHostStack));
#else
    pthread_attr_setstacksize(&attr, kHostStack);
#endif
    pthread_t th;
    pthread_create(&th, &attr, [](void* p) -> void* { run_thread((GuestThread*)p); return nullptr; }, t);
    pthread_detach(th);
}
}  // namespace

GuestThread* current() { return t_current; }
const char* current_name() { return t_current ? t_current->name.c_str() : nullptr; }

GuestThread* get(uint32_t os) {
    auto it = g_threads.find(os);
    return it == g_threads.end() ? nullptr : it->second;
}

uint32_t sda_base() { return loader::main_module().sda_base; }

namespace {
std::mutex g_wait_stats_lock;
std::vector<std::pair<const char*, uint64_t>> g_wait_stats;  // kind -> ns
auto g_wait_stats_t0 = std::chrono::steady_clock::now();
}  // namespace
void note_main_wait(const char* kind, uint64_t ns) {
    std::lock_guard<std::mutex> l(g_wait_stats_lock);
    for (auto& [k, v] : g_wait_stats)
        if (k == kind) {
            v += ns;
            return;
        }
    g_wait_stats.push_back({kind, ns});
}
std::string main_wait_report() {
    std::lock_guard<std::mutex> l(g_wait_stats_lock);
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g_wait_stats_t0).count();
    g_wait_stats_t0 = now;
    std::string s;
    char buf[96];
    for (auto& [k, v] : g_wait_stats) {
        snprintf(buf, sizeof buf, "%s%s %.0f", s.empty() ? "" : ", ", k, v / 1e6 / secs);
        s += buf;
        v = 0;
    }
    return s + " (ms/s)";
}

namespace {
std::atomic<const char*> g_sample_label{"?"};  // name for non-guest threads (sample_host)
void on_stack_signal(int) {  // runs on the interrupted thread
    void* frames[48];
    int n = backtrace(frames, 48);
    char line[2048];
    int k = snprintf(line, sizeof line, "  %s host frames (base 0x%lx):", current_name() ? current_name() : g_sample_label.load(),
                     (unsigned long)_dyld_get_image_header(0));
    for (int i = 2; i < n && k < (int)sizeof line - 24; i++) k += snprintf(line + k, sizeof line - k, " %p", frames[i]);
    LOG("%s", line);
}
}  // namespace

void install_stack_signal() {
    static bool installed = [] {
        struct sigaction sa = {};
        sa.sa_handler = on_stack_signal;
        sigaction(SIGUSR2, &sa, nullptr);
        return true;
    }();
    (void)installed;
}

void sample_host(pthread_t th, const char* label) {
    install_stack_signal();
    g_sample_label = label;
    pthread_kill(th, SIGUSR2);
}

void dump(bool stacks) {
    std::vector<pthread_t> running;
    {
        std::lock_guard<std::mutex> l(g_klock);
        int suspended = 0;
        for (auto& [os, t] : g_threads) {
            if (t->exited) continue;
            if (!t->started) {
                suspended++;
                continue;
            }
            const char* k = t->wait_kind;
            if (k) LOG("thread %-32s %08x entry %08x core %d: %s %08x (lr %08x)", t->name.c_str(), os, t->entry, t->core, k,
                       t->wait_obj, t->wait_lr);
            else {
                LOG("thread %-32s %08x entry %08x core %d: running (lr %08x)", t->name.c_str(), os, t->entry, t->core, t->ctx.lr);
                if (t->host_tid) running.push_back(t->host_tid);
            }
        }
        LOG("%d threads created but never started", suspended);
    }
    if (!stacks) return;
    install_stack_signal();
    for (pthread_t th : running) {
        pthread_kill(th, SIGUSR2);
        usleep(50000);
    }
}

void init_context(PPCContext* c, uint32_t stack_top, int core) {
    memset(c, 0, sizeof *c);
    c->r[1] = (stack_top - 0x20) & ~0xFu;
    mem::w32(c->r[1], 0);  // back chain terminator
    c->r[2] = loader::main_module().sda2_base;
    c->r[13] = loader::main_module().sda_base;
    c->gqr[2] = 0x00040004;
    c->gqr[3] = 0x00050005;
    c->gqr[4] = 0x00060006;
    c->gqr[5] = 0x00070007;
    c->res_addr = 0xFFFFFFFFu;
    c->core = core;
}

static uint32_t new_os_thread(const char* name, uint32_t entry, uint32_t stack_top, uint32_t stack_size, int prio,
                              int core) {
    uint32_t os = mem::sys_alloc(kOSThreadSize, 8);
    mem::w32(os + T_MAGIC, 0x74487244);
    mem::w8(os + T_STATE, 2);
    mem::w16(os + T_ID, g_next_id++);
    mem::w32(os + T_EFFPRIO, prio);
    mem::w32(os + T_BASEPRIO, prio);
    mem::w32(os + T_STACKBASE, stack_top);
    mem::w32(os + T_STACKEND, stack_top - stack_size);
    mem::w32(os + T_ENTRY, entry);
    mem::w32(os + T_AFFINITY, 1u << core);
    mem::w32(os + T_NAME, mem::sys_strdup(name));
    mem::w32(os + 0x1DC, core);  // context.upir
    return os;
}

GuestThread* adopt_host_thread(const char* name, uint32_t stack_size, int core) {
    GuestThread* t = new GuestThread;
    uint32_t stack = mem::sys_alloc(stack_size, 16);
    t->name = name;
    t->core = core;
    t->started = true;
    t->suspend = 0;
    std::lock_guard<std::mutex> l(g_klock);
    t->os = new_os_thread(name, 0, stack + stack_size, stack_size, 0, core);
    init_context(&t->ctx, stack + stack_size, core);
    t->ctx.thread = t;
    g_threads[t->os] = t;
    t_current = t;
    return t;
}

void start_main(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size) {
    GuestThread* t = new GuestThread;
    uint32_t stack = mem::sys_alloc(stack_size, 16);
    t->name = "main";
    t->is_main = true;
    t->core = 1;
    t->entry = entry;
    t->suspend = 0;
    {
        std::lock_guard<std::mutex> l(g_klock);
        t->os = new_os_thread("Default Thread 1", entry, stack + stack_size, stack_size, 16, 1);
        init_context(&t->ctx, stack + stack_size, 1);
        t->ctx.thread = t;
        t->ctx.r[3] = argc;
        t->ctx.r[4] = argv;
        g_threads[t->os] = t;
    }
    launch(t);
}
}  // namespace threads

using namespace threads;

// ---- coreinit thread API ---------------------------------------------------------------------
// BOOL OSCreateThread(OSThread*, entry, argc, argv, stackTop, stackSize, priority, attr)
HLE_EXPORT(coreinit, OSCreateThread) {
    uint32_t os = ARG(0), entry = ARG(1), argc = ARG(2), argv = ARG(3), stack_top = ARG(4), stack_size = ARG(5);
    int32_t prio = (int32_t)ARG(6);
    uint32_t attr = ARG(7);
    GuestThread* me = current();
    int core = me ? me->core : 1;
    uint32_t aff = attr & 7;
    if (aff == 1) core = 0;
    else if (aff == 2) core = 1;
    else if (aff == 4) core = 2;
    GuestThread* t = new GuestThread;
    t->os = os;
    t->entry = entry;
    t->core = core;
    t->name = "thread";
    {
        std::lock_guard<std::mutex> l(g_klock);
        memset(mem::host(os), 0, kOSThreadSize);
        mem::w32(os + T_MAGIC, 0x74487244);
        mem::w8(os + T_STATE, 1);
        mem::w8(os + T_ATTR, (uint8_t)attr);
        mem::w16(os + T_ID, g_next_id++);
        mem::w32(os + T_SUSPEND, 1);
        mem::w32(os + T_EFFPRIO, prio);
        mem::w32(os + T_BASEPRIO, prio);
        mem::w32(os + T_STACKBASE, stack_top);
        mem::w32(os + T_STACKEND, stack_top - stack_size);
        mem::w32(os + T_ENTRY, entry);
        mem::w32(os + T_AFFINITY, aff ? aff : 7);
        mem::w32(os + 0x1DC, core);
        init_context(&t->ctx, stack_top, core);
        t->ctx.thread = t;
        t->ctx.r[3] = argc;
        t->ctx.r[4] = argv;
        if (GuestThread* old = get(os)) {
            if (!old->exited) LOG("warning: OSCreateThread reuses a live OSThread %08x", os);
        }
        g_threads[os] = t;
    }
    TRACE("OSCreateThread %08x entry %08x stack %08x+%x prio %d attr %x core %d", os, entry, stack_top, stack_size,
          prio, attr, core);
    RET(1);
}

HLE_EXPORT(coreinit, OSResumeThread) {
    uint32_t os = ARG(0);
    std::lock_guard<std::mutex> l(g_klock);
    GuestThread* t = get(os);
    if (!t) {
        LOG("OSResumeThread: unknown thread %08x", os);
        RET(0);
        return;
    }
    int prev = t->suspend;
    if (t->suspend > 0) t->suspend--;
    mem::w32(os + T_SUSPEND, t->suspend);
    if (t->suspend == 0 && !t->started) {
        mem::w8(os + T_STATE, 2);
        launch(t);
    }
    RET(prev);
}

HLE_EXPORT(coreinit, OSJoinThread) {
    uint32_t os = ARG(0), out = ARG(1);
    std::unique_lock<std::mutex> l(g_klock);
    GuestThread* t = get(os);
    if (!t) {
        RET(0);
        return;
    }
    threads::WaitMark w("join", os);
    t->cv.wait(l, [&] { return t->exited; });
    if (out) mem::w32(out, t->exit_value);
    RET(1);
}

HLE_EXPORT(coreinit, OSGetCurrentThread) { RET(current() ? current()->os : 0); }

HLE_EXPORT(coreinit, OSSetThreadName) {
    uint32_t os = ARG(0), name = ARG(1);
    mem::w32(os + T_NAME, name);
    std::lock_guard<std::mutex> l(g_klock);
    if (GuestThread* t = get(os)) {
        t->name = name ? mem::str(name) : "";
        if (t == current()) pthread_setname_np(t->name.c_str());
    }
}

HLE_EXPORT(coreinit, OSSetThreadPriority) {
    uint32_t os = ARG(0);
    int32_t prev = (int32_t)mem::r32(os + T_BASEPRIO);
    mem::w32(os + T_BASEPRIO, ARG(1));
    mem::w32(os + T_EFFPRIO, ARG(1));
    (void)prev;
    RET(1);
}
HLE_EXPORT(coreinit, OSGetThreadPriority) { RET(mem::r32(ARG(0) + T_BASEPRIO)); }

HLE_EXPORT(coreinit, OSSetThreadAffinity) {
    uint32_t os = ARG(0), aff = ARG(1) & 7;
    mem::w32(os + T_AFFINITY, aff);
    std::lock_guard<std::mutex> l(g_klock);
    if (GuestThread* t = get(os)) {
        if (aff == 1) t->core = 0;
        else if (aff == 2) t->core = 1;
        else if (aff == 4) t->core = 2;
        t->ctx.core = t->core;
    }
    RET(1);
}
HLE_EXPORT(coreinit, OSGetThreadAffinity) { RET(mem::r32(ARG(0) + T_AFFINITY)); }

HLE_EXPORT(coreinit, OSGetThreadSpecific) {
    uint32_t i = ARG(0);
    GuestThread* t = current();
    RET(t && i < 16 ? mem::r32(t->os + T_SPECIFIC + 4 * i) : 0);
}
HLE_EXPORT(coreinit, OSSetThreadSpecific) {
    uint32_t i = ARG(0);
    GuestThread* t = current();
    if (t && i < 16) mem::w32(t->os + T_SPECIFIC + 4 * i, ARG(1));
}

HLE_EXPORT(coreinit, OSGetCoreId) { RET(current() ? current()->core : 1); }
HLE_EXPORT(coreinit, OSGetStackPointer) { RET(c->r[1]); }
HLE_EXPORT(coreinit, OSYieldThread) { sched_yield(); }
HLE_EXPORT(coreinit, OSBlockThreadsOnExit) {}

// void OSSleepTicks(OSTime ticks)
HLE_EXPORT(coreinit, OSSleepTicks) {
    int64_t ticks = (int64_t)ARG64(0);
    if (ticks <= 0) {
        sched_yield();
        return;
    }
    uint64_t ns = (uint64_t)((__int128)ticks * 1000000000 / 62156250);
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    threads::WaitMark w("sleep", (uint32_t)(ns / 1000));
    nanosleep(&ts, nullptr);
}

HLE_EXPORT(coreinit, OSInitThreadQueue) {
    uint32_t q = ARG(0);
    mem::w32(q, 0);
    mem::w32(q + 4, 0);
    mem::w32(q + 8, 0);
    mem::w32(q + 12, 0);
}

// Cemu-compat: guest callback on the calling guest thread.
uint32_t cemu_core_callback(uint32_t fn, std::initializer_list<uint32_t> args) {
    threads::GuestThread* t = threads::current();
    if (!t) {
        LOG("cemu_core_callback(%08x) from a non-guest thread", fn);
        return 0;
    }
    return guest_call(&t->ctx, fn, args);
}
