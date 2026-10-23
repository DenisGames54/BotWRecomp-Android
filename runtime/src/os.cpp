#include "os.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "hle.h"
#include "host/window.h"
#include "thread.h"

std::atomic<uint64_t> g_game_frames{0};

namespace os {
namespace {
std::atomic<int> g_exit_code{-1};
}

void init() {
    init_heaps();
    init_time();
}

uint32_t build_argv(const char* rpx) {
    uint32_t argv = mem::sys_alloc(16, 4);
    mem::w32(argv, mem::sys_strdup(rpx));
    mem::w32(argv + 4, 0);
    return argv;
}

#ifdef BOTW_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
#define BOTW_PGO_FLUSH() __llvm_profile_write_file()
#else
#define BOTW_PGO_FLUSH() ((void)0)
#endif
void request_exit(int code) {
    g_exit_code = code;
    // give the log a moment, then leave without running host static destructors
    fflush(stderr);
    BOTW_PGO_FLUSH();
    _exit(code);
}

void host_main_loop() {
    window::run();
    if (g_run_seconds) threads::dump();  // test runs: where every thread was at the end
    fflush(stderr);
    BOTW_PGO_FLUSH();
    _exit(0);
}
}  // namespace os
