// BotWRecomp entry: map guest memory, load U-King.rpx, initialise Cafe OS state and run the game's
// entry point on its own host thread. The host main thread stays free for the window/event loop.
#ifdef BOTW_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "hle.h"
#include "loader.h"
#include "os.h"
#include "settings.h"
#include "thread.h"
#include "host/window.h"
#include "latte/latte.h"

extern "C" int g_trace_hle;
std::string g_game_root;   // contains code/, content/, meta/
std::string g_user_root;   // ~/Library/Application Support/BotWRecomp
bool g_headless = false;
bool g_mute = false;       // no audio output (on-screen test runs)
int g_run_seconds = 0;     // --seconds N: exit after N seconds (tests)
float g_render_scale = 0;  // --scale N: render targets N times the native 1280x720 (graphic_pack_shim.cpp); 0 = fit the display
int g_target_fps = 60;       // --fps 30: the game's own pacing (game/fps.cpp)
uint64_t g_trace_from = 0, g_trace_to = 0;  // --trace-frames A-B: HLE call trace for frames [A, B)

extern "C" PPC_NORETURN void rt_fatal_exit(void) {
    fflush(stderr);
#ifdef BOTW_PGO_GEN
    __llvm_profile_write_file();
#endif
    _exit(3);
}

bool debug_watch_fault(void* addr, void* uctx);
void debug_watch_init();

static void on_fault(int sig, siginfo_t* si, void* uctx) {
    if (debug_watch_fault(si->si_addr, uctx)) return;
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)g_mem;
    if (a >= base && a < base + (1ull << 32) + 0x10000)
        LOG("FATAL: signal %d: guest memory access at %08x", sig, (uint32_t)(a - base));
    else
        LOG("FATAL: signal %d: host access at %p", sig, (void*)a);
    threads::GuestThread* t = threads::current();
    rt_trap(t ? &t->ctx : nullptr, 0, "signal (ctx registers may be stale)");
}

#ifdef BOTW_PGO_GEN
#include <cstdio>
#include <cerrno>
extern "C" int __llvm_profile_write_file(void);
extern "C" unsigned long long __llvm_profile_get_size_for_buffer(void);
#endif
int main(int argc, char** argv) {
    // saves, caches, logs, settings; BOTW_USER_ROOT lets test runs use their own folder (tools/run.sh)
    if (const char* e = getenv("BOTW_USER_ROOT")) g_user_root = e;
    else g_user_root = std::string(getenv("HOME")) + "/Library/Application Support/BotWRecomp";
    mkdir(g_user_root.c_str(), 0755);
    log_set_file((g_user_root + "/runtime.log").c_str());
    settings::load(g_user_root + "/settings.ini");  // in-game menu settings; the flags below override them
    Settings opt = settings::get();

    int capture_every = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) g_game_root = argv[++i];
        else if (a == "--trace") g_log_level = 1;
        else if (a == "--headless") g_headless = true;
        else if (a == "--mute") g_mute = true;
        else if (a == "--capture" && i + 1 < argc) capture_every = atoi(argv[++i]);
        else if (a == "--script" && i + 1 < argc) window::set_script(argv[++i]);
        else if (a == "--trace-hle") g_trace_hle = 1;
        else if (a == "--no-gyro") opt.gyro = false;
        else if (a == "--trace-frames" && i + 1 < argc) sscanf(argv[++i], "%llu-%llu", &g_trace_from, &g_trace_to);
        else if (a == "--seconds" && i + 1 < argc) g_run_seconds = atoi(argv[++i]);
        else if (a == "--fps" && i + 1 < argc) opt.fps = atoi(argv[++i]) >= 60 ? 60 : 30;
        else if (a == "--scale" && i + 1 < argc) opt.scale = (float)atof(argv[++i]);
        else if (a == "--display" && i + 1 < argc) {  // fit (letterbox) | stretch | fill (crop to cover)
            std::string m = argv[++i];
            opt.display = m == "stretch" ? 1 : m == "fill" ? 2 : 0;
        }
    }
    settings::set(opt, false);  // this run only; the menu saves its changes
    g_render_scale = opt.scale;
    if (g_game_root.empty()) {
        if (const char* e = getenv("BOTW_GAME")) g_game_root = e;
    }
    if (g_game_root.empty()) {
        fprintf(stderr, "usage: %s --game <folder with code/ content/ meta/>\n", argv[0]);
        return 2;
    }
    struct sigaction sa = {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    signal(SIGABRT, [](int) {
        LOG("FATAL: abort()");
        _exit(6);
    });

    mem::init();
    dispatch::init();
    if (!loader::load_main(g_game_root + "/code/U-King.rpx")) return 1;
    os::init();
    window::create();
    if (g_render_scale <= 0) {  // render at the display's native resolution, so fullscreen is 1:1
        int dw, dh;
        window::display_pixel_size(&dw, &dh);
        g_render_scale = std::clamp(std::min(dw / 1280.0f, dh / 720.0f), 1.0f, 3.0f);
        LOG("render scale %.3f (%dx%d for a %dx%d display)", g_render_scale, (int)(1280 * g_render_scale),
            (int)(720 * g_render_scale), dw, dh);
    }
    debug_watch_init();
    if (capture_every) latte::set_capture(capture_every, (g_user_root + "/captures").c_str());

    uint32_t argv_g = os::build_argv("U-King.rpx");
    threads::start_main(loader::main_module().entry, 1, argv_g, loader::main_module().stack_size);
    os::host_main_loop();
#ifdef BOTW_PGO_GEN
    fprintf(stderr, "PGO: expected %llu bytes\n", __llvm_profile_get_size_for_buffer());
    int pgo_rc = __llvm_profile_write_file();
    fprintf(stderr, "PGO: write_file rc=%d errno=%d\n", pgo_rc, errno);
    fflush(stderr);
#endif  // returns when the game exits or the time limit is reached
    return 0;
}
