// Native Metal GPU path (docs/native-renderer.md). Built only with -DBOTW_NATIVE_GPU=ON
// (runtime/src/gpu_native/gpu_native.cmake); every source in this directory compiles to nothing
// otherwise, so the default build (Cemu Latte) is unaffected.
//
// Selection at run time (BOTW_NATIVE_GPU builds only):
//   --renderer native   or  BOTW_RENDERER=native   native backend instead of Cemu's Latte layer
//   --gpu-stats N       or  BOTW_GPU_STATS=N       PM4 stream statistics every N frames (either backend)
//   --gpu-dump-frame N  or  BOTW_GPU_DUMP_FRAME=N  with --gpu-stats: per-draw log of stream frame N
//
// This header is shared by the switch TU compiled in Cemu's environment (latte_switch.mm.in) and
// the native sources compiled in the runtime's environment, so it only uses plain C++ types.
#pragma once
#include <stdint.h>

#include <functional>

namespace gpu_native {
bool native_selected();  // --renderer native
int stats_interval();    // --gpu-stats N (0 = off)
int64_t dump_frame_index();  // --gpu-dump-frame N: log every draw/clear/copy of stream frame N (-1 = off)

// PM4 statistics tap (pm4_stats.cpp): walks a submitted command buffer (and the display lists it
// calls) before it goes to the active backend.
void stats_observe(uint32_t addr, uint32_t ndwords);

// Native backend: the same contract as runtime/src/latte/latte.h.
namespace backend {
void create_renderer(void* nsview, int width, int height);
void resize_window(int width, int height);
void set_scanout_srgb(bool tv, bool srgb);
void set_display_mode(int mode);  // 0 keep aspect, 1 stretch, 2 fill (on-screen output: phase 2)
void init();
void set_capture(int every, const char* dir);
uint64_t submit(uint32_t addr, uint32_t ndwords);
uint64_t last_submitted();
uint64_t retired();
void wait_retired(uint64_t ts);
uint32_t flip_requests();
uint32_t flips_executed();
void add_flip_request();
void wait_for_flip();
void wait_for_vsync();
uint64_t last_flip_time();
uint64_t last_vsync_time();
uint32_t swap_interval();
void set_swap_interval(uint32_t);
void set_event_sink(std::function<void(uint32_t)> sink);  // latte::Event values
}  // namespace backend
}  // namespace gpu_native
