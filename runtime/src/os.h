// Cafe OS process-level state shared by the HLE libraries.
#pragma once
#include <stdint.h>

#include <string>

extern std::string g_game_root, g_user_root;
extern bool g_headless;
extern int g_run_seconds;
extern int g_target_fps;
#include <atomic>
extern std::atomic<uint64_t> g_game_frames;  // GX2SwapScanBuffers calls (watchdog)
extern uint64_t g_trace_from, g_trace_to;

namespace os {
void init();                              // heaps, FS, devices, per-library init
uint32_t build_argv(const char* rpx);     // guest argv array
void host_main_loop();                    // window/event pump on the host main thread
void request_exit(int code);
// default heaps (coreinit MEM)
uint32_t default_heap_alloc(uint32_t size, int32_t align);
void default_heap_free(uint32_t ptr);
uint32_t mem2_bound_start();
void init_heaps();
void init_time();
}  // namespace os
