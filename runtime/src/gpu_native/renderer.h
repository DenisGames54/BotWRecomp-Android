// What the PM4 decoder hands to the Metal renderer: typed operations, with the decoded register
// file for draw state. Implemented by renderer_metal.mm.
#pragma once
#include <stdint.h>

#include <functional>

namespace gpu_native::renderer {
void create(void* nsview, int width, int height);
void resize(int width, int height);
void set_scanout_srgb(bool tv, bool srgb);
void set_capture(int every, const char* dir);

// GPU thread only below.
void idle();                                    // the decoder ran out of work: commit what is recorded
void after_completion(std::function<void()> fn);  // run fn once all work recorded so far has finished

struct Draw {
    uint32_t count;        // indices / vertices
    uint32_t index_addr;   // 0 for auto-index draws
    uint32_t index_type;   // VGT_DMA_INDEX_TYPE (0 u16 LE, 1 u32 LE, 4 u16 BE, 9 u32 BE)
    uint32_t instances;
    const uint32_t* regs;  // decoded register file (absolute register numbers)
};
void draw(const Draw& d);

struct Surface {  // a GX2 surface as the HLE packets describe it
    uint32_t addr = 0, format = 0, tilemode = 0, width = 0, height = 0, pitch = 0, swizzle = 0, slice = 0,
             num_slices = 1, dim = 1, aa = 0;
};
void clear(uint32_t mask, const Surface& color, const Surface& depth, const float rgba[4], float depth_value,
           uint32_t stencil_value);
void copy_surface(const Surface& src, const Surface& dst, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void scanout_copy(const Surface& color, uint32_t target);  // GX2CopyColorBufferToScanBuffer (target 1 TV, 4 DRC)
void present();                                          // GX2SwapScanBuffers reached the GPU
void surface_sync(uint32_t addr, uint32_t size);          // texture cache invalidation (SURFACE_SYNC)
void occlusion_begin(uint32_t query);
void occlusion_end(uint32_t query);

// time accounting (GPU thread), reported by the backend's liveness log
enum Timer { T_SHADER, T_PIPELINE, T_TEXTURE, T_INDEX, T_DRAW, T_WAIT_MEM, T_COUNT };
extern double g_time_ms[T_COUNT];
struct Scoped {
    Timer t;
    uint64_t t0;
    explicit Scoped(Timer x);
    ~Scoped();
};
}  // namespace gpu_native::renderer
