// BotWRecomp: glue between this runtime and the vendored Cemu Latte GPU (Cemu environment).
// Implements runtime/src/latte/latte.h on top of Cemu's command processor:
//  - GX2 submissions (latte::submit) become ring words: INDIRECT_BUFFER + EVENT_WRITE_EOP(timestamp),
//    the same pair Cemu's TCL queues; Cemu's CP drains them through TCL::TCLGPUReadRBWord.
//  - retirement, flips and vsync are reported back through TCL/GX2 notification hooks.
#include <TargetConditionals.h>
#if TARGET_OS_IOS  // iOS port (ios/): no AppKit; CoreGraphics/ImageIO are all this file needs
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <condition_variable>
#include <deque>

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "WindowSystem.h"
#include "config/CemuConfig.h"
#include "../src/latte/latte.h"

void Latte_Start();
namespace mem { uint32_t sys_alloc(uint32_t size, uint32_t align); }

namespace {
std::mutex g_rb_lock;
std::deque<uint32> g_rb;
std::condition_variable g_rb_cv;  // the idle GPU thread waits here (TCLGPUWaitForWork)
bool g_rb_waiting = false;        // under g_rb_lock
std::atomic<uint64> g_last_submitted{0}, g_retired{0};
std::mutex g_retire_lock;
std::condition_variable g_retire_cv;
uint32 g_retire_slot = 0;
std::function<void(latte::Event)> g_sink;
std::mutex g_sync_lock;
std::condition_variable g_flip_cv, g_vsync_cv;
uint64 g_vsync_count = 0;
std::atomic<uint64> g_last_flip_time{0}, g_last_vsync_time{0};
std::atomic<uint64> g_frames{0};
int g_capture_every = 0;
std::atomic<uint64> g_capture_pending{0};
std::string g_capture_dir;

void write_png(const std::vector<uint8>& rgb, int w, int h, const std::string& path) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(nullptr, rgb.data(), rgb.size());
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate(w, h, 8, 24, w * 3, cs, kCGImageAlphaNone, prov, nullptr, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), path.size(), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGDataProviderRelease(prov);
    CFRelease(data);
    CGColorSpaceRelease(cs);
}
}  // namespace
namespace window { void on_frame(uint64_t frame); }
void log_write(const char* fmt, ...);

void TCL::TCLGPUWaitForWork(int max_us) {
    std::unique_lock<std::mutex> l(g_rb_lock);
    if (!g_rb.empty()) return;
    g_rb_waiting = true;
    g_rb_cv.wait_for(l, std::chrono::microseconds(max_us), [] { return !g_rb.empty(); });
    g_rb_waiting = false;
}
bool TCL::TCLGPUReadRBWord(uint32& w) {
    std::lock_guard<std::mutex> l(g_rb_lock);
    if (g_rb.empty()) return false;
    w = g_rb.front();
    g_rb.pop_front();
    return true;
}
void TCL::TCLGPUNotifyNewRetirementTimestamp() {
    uint64 ts = memory_readU64(g_retire_slot);
    {
        std::lock_guard<std::mutex> l(g_retire_lock);
        if (ts > g_retired) g_retired = ts;
    }
    g_retire_cv.notify_all();
}

namespace GX2 {
void __GX2NotifyNewRetirementTimestamp(uint64) {}
void __GX2NotifyEvent(GX2CallbackEventType e) {
    if (e == GX2CallbackEventType::VSYNC) {
        g_last_vsync_time = coreinit::OSGetSystemTime();
        std::lock_guard<std::mutex> l(g_sync_lock);
        g_vsync_count++;
        g_vsync_cv.notify_all();
    } else if (e == GX2CallbackEventType::FLIP) {
        g_last_flip_time = coreinit::OSGetSystemTime();
        uint64 f = ++g_frames;
        window::on_frame(f);
        if (g_capture_every && f % g_capture_every == 0) g_capture_pending = f;
        if (f % 300 == 0) log_write("frame %llu", (unsigned long long)f);
        std::lock_guard<std::mutex> l(g_sync_lock);
        g_flip_cv.notify_all();
    }
    if (g_sink) g_sink((latte::Event)(uint32)e);
}
}  // namespace GX2

// --capture: read the TV image back synchronously (GPU thread) and save it as PNG.
void botw_capture_hook(LatteTextureView* view, bool isPadView) {
    uint64 f = g_capture_pending.exchange(0);
    if (!f || isPadView || !g_renderer) {
        if (f && isPadView) g_capture_pending = f;
        return;
    }
    auto* mr = (MetalRenderer*)g_renderer.get();
    MTL::Texture* tex = static_cast<LatteTextureMtl*>(view->baseTexture)->GetTexture();
    uint32 w = (uint32)tex->width(), h = (uint32)tex->height();
    MTL::PixelFormat fmt = tex->pixelFormat();
    uint32 bpp = (fmt == MTL::PixelFormatRGBA16Float) ? 8 : 4;
    MTL::Buffer* buf = mr->GetDevice()->newBuffer(w * h * bpp, MTL::ResourceStorageModeShared);
    auto* enc = mr->GetBlitCommandEncoder();
    enc->copyFromTexture(tex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1), buf, 0, w * bpp, 0);
    mr->Flush(true);
    std::vector<uint8> rgb(w * h * 3);
    const uint8* src = (const uint8*)buf->contents();
    // the screen output gamma-encodes when the scan-out buffer is sRGB; do the same so captures match
    const bool encode = !isPadView && LatteGPUState.tvBufferUsesSRGB && fmt != MTL::PixelFormatBGRA8Unorm_sRGB;
    static uint8 s_srgb[1024];
    static bool s_srgb_init = [] {
        for (int k = 0; k < 1024; k++) {
            float v = k / 1023.0f;
            v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
            s_srgb[k] = (uint8)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
        }
        return true;
    }();
    (void)s_srgb_init;
    for (uint32 i = 0; i < w * h; i++) {
        uint8 r, g, b;
        if (fmt == MTL::PixelFormatBGRA8Unorm || fmt == MTL::PixelFormatBGRA8Unorm_sRGB) {
            b = src[i * 4]; g = src[i * 4 + 1]; r = src[i * 4 + 2];
        } else if (fmt == MTL::PixelFormatRGB10A2Unorm) {
            uint32 v = ((const uint32*)src)[i];
            r = (uint8)(((v >> 0) & 1023) >> 2); g = (uint8)(((v >> 10) & 1023) >> 2); b = (uint8)(((v >> 20) & 1023) >> 2);
        } else if (bpp == 8) {
            const __fp16* hp = (const __fp16*)(src + i * 8);
            auto c = [](float x) { return (uint8)std::clamp(x * 255.0f + 0.5f, 0.0f, 255.0f); };
            r = c(hp[0]); g = c(hp[1]); b = c(hp[2]);
        } else {
            r = src[i * 4]; g = src[i * 4 + 1]; b = src[i * 4 + 2];
        }
        if (encode) {
            if (fmt == MTL::PixelFormatRGB10A2Unorm) {  // keep 10-bit precision
                uint32 v = ((const uint32*)src)[i];
                r = s_srgb[v & 1023]; g = s_srgb[(v >> 10) & 1023]; b = s_srgb[(v >> 20) & 1023];
            } else {
                r = s_srgb[r * 1023 / 255]; g = s_srgb[g * 1023 / 255]; b = s_srgb[b * 1023 / 255];
            }
        }
        rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
    }
    buf->release();
    std::string path = fmt::format("{}/frame_{:06}.png", g_capture_dir, f);
    write_png(rgb, (int)w, (int)h, path);
    log_write("capture %s (%ux%u, format %u)", path.c_str(), w, h, (uint32)fmt);
}

namespace latte {
// Main thread, before the game starts: Cemu's renderer adds a Metal child view to `nsview`.
void create_renderer(void* nsview, int w, int h) {
    WindowSystem::GetWindowInfo().window_main.surface = nsview;
    WindowSystem::GetWindowInfo().window_main.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
    g_renderer = std::make_unique<MetalRenderer>();
    ((MetalRenderer*)g_renderer.get())->InitializeLayer({w, h}, true);
}

void resize_window(int w, int h) {
    if (g_renderer) ((MetalRenderer*)g_renderer.get())->ResizeLayer({w, h}, true);
}

void set_display_mode(int mode) { GetConfig().fullscreen_scaling = mode; }
void set_output_filter(int f) {  // upscaling uses the choice; downscaling stays bilinear unless nearest
    GetConfig().upscale_filter = f;
    GetConfig().downscale_filter = f == kNearestNeighborFilter ? kNearestNeighborFilter : kLinearFilter;
}

void set_scanout_srgb(bool tv, bool srgb) {
    if (tv) LatteGPUState.tvBufferUsesSRGB = srgb;
    else LatteGPUState.drcBufferUsesSRGB = srgb;
}

void set_capture(int every, const char* dir) {
    g_capture_every = every;
    g_capture_dir = dir;
    fs::create_directories(dir);
}

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    LatteGPUState.sharedAreaAddr = mem::sys_alloc(sizeof(gx2GPUSharedArea_t), 0x20);
    LatteGPUState.sharedArea = (gx2GPUSharedArea_t*)memory_getPointerFromVirtualOffset(LatteGPUState.sharedAreaAddr);
    LatteGPUState.sharedArea->flipRequestCountBE = 0;
    LatteGPUState.sharedArea->flipExecuteCountBE = 0;
    LatteGPUState.sharedArea->swapInterval = 1;
    g_retire_slot = mem::sys_alloc(16, 16);
    Latte_Start();  // returns once the GPU thread finished its init
    LatteGPUState.gx2InitCalled++;
}

uint64_t submit(uint32_t addr, uint32_t ndw) {
    uint64 ts = ++g_last_submitted;
    uint32 words[] = {pm4HeaderType3(IT_INDIRECT_BUFFER_PRIV, 3), addr, 0, ndw,
                      pm4HeaderType3(IT_EVENT_WRITE_EOP, 5), 0x504, g_retire_slot, 0x40000000, (uint32)ts, (uint32)(ts >> 32)};
    bool wake;
    {
        std::lock_guard<std::mutex> l(g_rb_lock);
        g_rb.insert(g_rb.end(), std::begin(words), std::end(words));
        wake = g_rb_waiting;
    }
    if (wake) g_rb_cv.notify_one();
    return ts;
}
uint64_t last_submitted() { return g_last_submitted; }
uint64_t retired() { return g_retired; }
void wait_retired(uint64_t ts) {
    std::unique_lock<std::mutex> l(g_retire_lock);
    g_retire_cv.wait(l, [&] { return g_retired >= ts; });
}

uint32_t flip_requests() { return _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE); }
uint32_t flips_executed() { return _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE); }
void add_flip_request() {
    LatteGPUState.sharedArea->flipRequestCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE) + 1);
}
void wait_for_flip() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    if (flip_requests() == flips_executed()) return;
    g_flip_cv.wait_for(l, std::chrono::milliseconds(100));
}
void wait_for_vsync() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    uint64 v = g_vsync_count;
    g_vsync_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return g_vsync_count != v; });
}
uint64_t last_flip_time() { return g_last_flip_time; }
uint64_t last_vsync_time() { return g_last_vsync_time; }
uint32_t swap_interval() { return LatteGPUState.sharedArea ? LatteGPUState.sharedArea->swapInterval : 1; }
void set_swap_interval(uint32_t v) {
    if (LatteGPUState.sharedArea) LatteGPUState.sharedArea->swapInterval = v;
}
void set_event_sink(std::function<void(Event)> sink) { g_sink = std::move(sink); }
uint32_t g_regs[kRegCount];  // unused with the Cemu CP (kept for the interface)
}  // namespace latte
