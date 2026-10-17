// Native Metal renderer (phase 1 slice): typed work from the PM4 decoder -> Metal.
//
//  * guest memory is one zero-copy MTLBuffer: uniform blocks and vertex attributes are read in
//    place by the shaders (byte swaps happen in the vertex fetch code)
//  * shaders are translated from the R600 programs with tools/shaders (r600.cpp, msl.cpp) and
//    compiled with newLibraryWithSource on first use, cached by program + baked register state
//    (phase 2: ahead-of-time metallib + function-constant specialization, docs/native-renderer.md)
//  * surfaces: address-keyed cache of MTLTextures; GPU-written targets are found again when sampled;
//    guest textures are detiled on the CPU with the vendored AddrLib (phase 2: compute detiling)
//  * headless: the TV scan buffer is captured to PNG (--capture N); no on-screen presentation yet
#if defined(BOTW_NATIVE_GPU)
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <math.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../tools/shaders/src/msl.h"
#include "../log.h"
#include "../mem.h"
#include "pm4.h"
#include "renderer.h"

// vendored GX2 software surface copy (runtime/src/gx2/GX2_Surface_Copy.cpp): tiled <-> linear via AddrLib
void gx2SurfaceCopySoftware(uint8_t* inputData, int32_t surfSrcHeight, int32_t srcPitch, int32_t srcDepth, uint32_t srcSlice,
                            uint32_t srcSwizzle, uint32_t srcHwTileMode, uint8_t* outputData, int32_t surfDstHeight,
                            int32_t dstPitch, int32_t dstDepth, uint32_t dstSlice, uint32_t dstSwizzle, uint32_t dstHwTileMode,
                            uint32_t copyWidth, uint32_t copyHeight, uint32_t copyBpp);

#include <mach/mach_time.h>
namespace gpu_native::renderer {
double g_time_ms[T_COUNT];
static double ticks_ms() {
    static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}
Scoped::Scoped(Timer x) : t(x), t0((uint64_t)(ticks_ms() * 1000.0)) {}
Scoped::~Scoped() { g_time_ms[t] += ticks_ms() - (double)t0 / 1000.0; }
void end_pass();
namespace {
using namespace pm4;

constexpr uint32_t kGuestBase = 0x10000;  // first 64 KB of g_mem are PROT_NONE (null guard)

// ---- formats -----------------------------------------------------------------------------------
// GX2 surface format = hw format | 0x100 int | 0x200 signed | 0x400 sRGB | 0x800 float
struct Fmt {
    MTLPixelFormat pf = MTLPixelFormatInvalid;
    uint32_t bpe = 0;    // bytes per element (block for BC)
    uint32_t block = 1;  // 4 for BC
    bool depth = false;
    int kind = 0;        // 0 float, 1 uint, 2 sint
    bool swap_rb = false;  // Metal format stores red and blue the other way round (5_6_5, 1_5_5_5)
};
Fmt fmt_of(uint32_t f) {
    uint32_t hw = f & 0x3F;
    bool i = f & 0x100, s = f & 0x200, srgb = f & 0x400;
    auto pick = [&](MTLPixelFormat un, MTLPixelFormat sn, MTLPixelFormat ui, MTLPixelFormat si, uint32_t bpe) {
        Fmt r;
        r.bpe = bpe;
        r.pf = i ? (s ? si : ui) : (s ? sn : un);
        r.kind = i ? (s ? 2 : 1) : 0;
        return r;
    };
    Fmt r;
    switch (hw) {
        case 0x01: return pick(MTLPixelFormatR8Unorm, MTLPixelFormatR8Snorm, MTLPixelFormatR8Uint, MTLPixelFormatR8Sint, 1);
        case 0x05: return pick(MTLPixelFormatR16Unorm, MTLPixelFormatR16Snorm, MTLPixelFormatR16Uint, MTLPixelFormatR16Sint, 2);
        case 0x06: r.pf = MTLPixelFormatR16Float, r.bpe = 2; return r;
        case 0x08: r.pf = MTLPixelFormatB5G6R5Unorm, r.bpe = 2, r.swap_rb = true; return r;
        case 0x0A: r.pf = MTLPixelFormatBGR5A1Unorm, r.bpe = 2, r.swap_rb = true; return r;
        case 0x07: return pick(MTLPixelFormatRG8Unorm, MTLPixelFormatRG8Snorm, MTLPixelFormatRG8Uint, MTLPixelFormatRG8Sint, 2);
        case 0x0D: return pick(MTLPixelFormatR32Uint, MTLPixelFormatR32Sint, MTLPixelFormatR32Uint, MTLPixelFormatR32Sint, 4);
        case 0x0E: r.pf = MTLPixelFormatR32Float, r.bpe = 4; return r;
        case 0x0F: return pick(MTLPixelFormatRG16Unorm, MTLPixelFormatRG16Snorm, MTLPixelFormatRG16Uint, MTLPixelFormatRG16Sint, 4);
        case 0x10: r.pf = MTLPixelFormatRG16Float, r.bpe = 4; return r;
        case 0x16: r.pf = MTLPixelFormatRG11B10Float, r.bpe = 4; return r;
        case 0x19: r.pf = i ? MTLPixelFormatRGB10A2Uint : MTLPixelFormatRGB10A2Unorm, r.bpe = 4, r.kind = i ? 1 : 0; return r;
        case 0x1A:
            r = pick(MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Snorm, MTLPixelFormatRGBA8Uint, MTLPixelFormatRGBA8Sint, 4);
            if (srgb) r.pf = MTLPixelFormatRGBA8Unorm_sRGB;
            return r;
        case 0x1D: return pick(MTLPixelFormatRG32Uint, MTLPixelFormatRG32Sint, MTLPixelFormatRG32Uint, MTLPixelFormatRG32Sint, 8);
        case 0x1E: r.pf = MTLPixelFormatRG32Float, r.bpe = 8; return r;
        case 0x1F: return pick(MTLPixelFormatRGBA16Unorm, MTLPixelFormatRGBA16Snorm, MTLPixelFormatRGBA16Uint, MTLPixelFormatRGBA16Sint, 8);
        case 0x20: r.pf = MTLPixelFormatRGBA16Float, r.bpe = 8; return r;
        case 0x22: return pick(MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Sint, MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Sint, 16);
        case 0x23: r.pf = MTLPixelFormatRGBA32Float, r.bpe = 16; return r;
        case 0x31: r.pf = srgb ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatBC1_RGBA, r.bpe = 8, r.block = 4; return r;
        case 0x32: r.pf = srgb ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatBC2_RGBA, r.bpe = 16, r.block = 4; return r;
        case 0x33: r.pf = srgb ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatBC3_RGBA, r.bpe = 16, r.block = 4; return r;
        case 0x34: r.pf = s ? MTLPixelFormatBC4_RSnorm : MTLPixelFormatBC4_RUnorm, r.bpe = 8, r.block = 4; return r;
        case 0x35: r.pf = s ? MTLPixelFormatBC5_RGSnorm : MTLPixelFormatBC5_RGUnorm, r.bpe = 16, r.block = 4; return r;
        default: return r;
    }
}
uint32_t surf_bpe(uint32_t gx2fmt) {
    if (gx2fmt & 0x1000) {
        uint32_t df = gx2fmt & 7;
        return df == 1 ? 2 : df == 6 ? 4 : 8;
    }
    return fmt_of(gx2fmt).bpe;
}
uint32_t cb_format(uint32_t info) {  // CB_COLORn_INFO -> GX2 surface format
    static const uint32_t bits[8] = {0, 0x200, 0, 0, 0x100, 0x300, 0x400, 0x800};
    return ((info >> 2) & 0x3F) | bits[(info >> 12) & 7];
}
uint32_t tex_format(const uint32_t* w) {  // texture resource words -> GX2 surface format
    uint32_t f = (w[1] >> 26) & 0x3F, nfa = (w[4] >> 8) & 3;
    if (nfa == 2) f |= 0x800;
    else if (nfa == 1) f |= 0x100;
    if ((w[4] >> 11) & 1) f |= 0x400;
    if ((w[4] & 3) == 1) f |= 0x200;
    return f;
}

// ---- state ---------------------------------------------------------------------------------------
id<MTLDevice> g_dev;
id<MTLCommandQueue> g_queue;
id<MTLBuffer> g_guest;
id<MTLCommandBuffer> g_cb;
id<MTLRenderCommandEncoder> g_enc;
std::string g_capture_dir;
int g_capture_every = 0;
uint64_t g_frame = 0;
bool g_tv_srgb = false;
bool g_ok = false;

struct Surf {
    uint32_t addr = 0, swizzle = 0, pitch = 0, width = 0, height = 0, slices = 1, gx2fmt = 0, tile = 0, dim = 1;
    id<MTLTexture> tex;
    bool gpu = false;     // last written by the GPU
    uint64_t hash = 0;    // guest data hash at upload (CPU surfaces)
    uint64_t frame = 0;   // last use
    uint64_t seq = 0;     // GPU write order (bound as a target)
};
uint64_t g_seq = 0;
std::unordered_multimap<uint32_t, Surf*> g_surfaces;  // by address (swizzle stripped)

// change detector for CPU-written textures: at most 4096 evenly spread 8-byte samples
// (phase 2 replaces this with DCFlush / GX2Invalidate / SURFACE_SYNC dirty tracking)
uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t step = std::max<size_t>(8, (n / 4096) & ~(size_t)7);
    for (size_t i = 0; i + 8 <= n; i += step) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

id<MTLTexture> make_texture(MTLPixelFormat pf, uint32_t w, uint32_t h, uint32_t slices, uint32_t dim, bool rt) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.pixelFormat = pf;
    d.width = std::max(1u, w);
    d.height = std::max(1u, h);
    d.textureType = MTLTextureType2D;
    if (dim == 5 || slices > 1) d.textureType = MTLTextureType2DArray, d.arrayLength = std::max(1u, slices);
    if (dim == 3) d.textureType = MTLTextureTypeCubeArray, d.arrayLength = std::max(1u, slices / 6);
    if (dim == 2) d.textureType = MTLTextureType3D, d.depth = std::max(1u, slices), d.arrayLength = 1;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | (rt ? MTLTextureUsageRenderTarget : 0);
    return [g_dev newTextureWithDescriptor:d];
}

// GX2 texture component selection (resource word4 DST_SEL_X..W) as a Metal swizzle view, cached
std::unordered_map<uint64_t, id<MTLTexture>> g_views;
id<MTLTexture> swizzled(id<MTLTexture> t, MTLPixelFormat pf, uint32_t word4, bool swap_rb = false) {
    uint32_t sel = (word4 >> 16) & 0xFFF;
    if (swap_rb) {  // guest X/Z live in the texture's B/R channels
        uint32_t m = 0;
        for (int c = 0; c < 4; c++) {
            uint32_t v = (sel >> (3 * c)) & 7;
            m |= (v == 0 ? 2u : v == 2 ? 0u : v) << (3 * c);
        }
        sel = m;
    }
    if (sel == 0x688 /* X Y Z W */ && pf == t.pixelFormat) return t;
    uint64_t key = (uint64_t)(uintptr_t)(__bridge void*)t ^ ((uint64_t)sel << 48) ^ ((uint64_t)pf << 32);
    auto it = g_views.find(key);
    if (it != g_views.end()) return it->second;
    auto ch = [](uint32_t v) {
        switch (v) {
            case 0: return MTLTextureSwizzleRed;
            case 1: return MTLTextureSwizzleGreen;
            case 2: return MTLTextureSwizzleBlue;
            case 3: return MTLTextureSwizzleAlpha;
            case 4: return MTLTextureSwizzleZero;
            default: return MTLTextureSwizzleOne;
        }
    };
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(ch(sel & 7), ch((sel >> 3) & 7), ch((sel >> 6) & 7), ch((sel >> 9) & 7));
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:pf textureType:t.textureType levels:NSMakeRange(0, t.mipmapLevelCount)
                                                 slices:NSMakeRange(0, t.arrayLength * (t.textureType == MTLTextureTypeCubeArray ? 6 : 1))
                                                swizzle:sw];
    return g_views[key] = v ? v : t;
}

// Metal only allows texture views within a compatibility class (and aborts otherwise): accept
// identical formats, sRGB/linear pairs and ordinary (non-packed, non-compressed, non-depth) formats
// of the same size. Anything else needs a copy pass (phase 3).
bool view_ok(MTLPixelFormat a, MTLPixelFormat b) {
    if (a == b) return true;
    auto ordinary = [](MTLPixelFormat f) {
        switch (f) {
            case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
            case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
            case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint:
            case MTLPixelFormatRG8Sint: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float:
            case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint:
            case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
            case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
            case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float:
            case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint:
            case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA32Uint:
            case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float:
                return true;
            default:
                return false;
        }
    };
    auto size = [](MTLPixelFormat f) {
        switch (f) {
            case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: return 1;
            case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
            case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint:
            case MTLPixelFormatRG8Sint: return 2;
            case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Unorm:
            case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
            case MTLPixelFormatRGBA16Float: return 8;
            case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float: return 16;
            default: return 4;
        }
    };
    return ordinary(a) && ordinary(b) && size(a) == size(b);
}

// most recently written GPU surface at addr with the same element size (exact format preferred)
Surf* find_gpu_surface(uint32_t addr, uint32_t bpe_fmt_class_bpe, uint32_t min_w = 0, uint32_t min_h = 0,
                       uint32_t exact_fmt = 0xFFFFFFFF) {
    Surf* best = nullptr;
    auto r = g_surfaces.equal_range(addr);
    for (int pass = 0; pass < 2 && !best; pass++)
        for (auto it = r.first; it != r.second; ++it) {
            Surf* c = it->second;
            if (pass == 0 && exact_fmt != 0xFFFFFFFF && c->gx2fmt != exact_fmt) continue;
            if (c->gpu && surf_bpe(c->gx2fmt) == bpe_fmt_class_bpe && c->pitch >= min_w && c->height >= min_h &&
                (!best || c->seq > best->seq))
                best = c;
        }
    return best;
}

// render target / depth buffer surface (created on first use, kept)
Surf* rt_surface(uint32_t addr, uint32_t swizzle, uint32_t gx2fmt, uint32_t pitch, uint32_t height, uint32_t tile,
                    bool depth_fmt, MTLPixelFormat depth_pf) {
    auto r = g_surfaces.equal_range(addr);
    for (auto it = r.first; it != r.second; ++it) {
        Surf* s = it->second;
        if (s->gx2fmt == gx2fmt && s->pitch == pitch && s->height == height && s->tex) {
            s->gpu = true;
            s->frame = g_frame;
            s->seq = ++g_seq;
            return s;
        }
    }
    Surf* s = new Surf;
    s->addr = addr, s->swizzle = swizzle, s->pitch = pitch, s->width = pitch, s->height = height, s->gx2fmt = gx2fmt;
    s->tile = tile;
    MTLPixelFormat pf = depth_fmt ? depth_pf : fmt_of(gx2fmt).pf;
    if (pf == MTLPixelFormatInvalid) {
        delete s;
        return nullptr;
    }
    s->tex = make_texture(pf, pitch, height, 1, 1, true);
    s->gpu = true;
    s->frame = g_frame;
    s->seq = ++g_seq;
    g_surfaces.emplace(addr, s);
    return s;
}

// guest texture (CPU-written data), detiled on upload, re-uploaded when its bytes change
Surf* guest_texture(uint32_t addr, uint32_t swizzle, uint32_t gx2fmt, uint32_t pitch, uint32_t width, uint32_t height,
                       uint32_t slices, uint32_t tile, uint32_t dim) {
    Scoped timer(T_TEXTURE);
    Fmt f = fmt_of(gx2fmt);
    if (f.pf == MTLPixelFormatInvalid) return nullptr;
    uint32_t we = (width + f.block - 1) / f.block, he = (height + f.block - 1) / f.block;
    uint32_t pe = pitch;  // already in elements for BC (pitch / 4)
    uint32_t he_al = (he + 31) & ~31u;
    size_t bytes = (size_t)pe * he_al * f.bpe * slices;
    if (!bytes || bytes > (256u << 20)) return nullptr;
    Surf* s = nullptr;
    auto r = g_surfaces.equal_range(addr);
    for (auto it = r.first; it != r.second; ++it) {
        Surf* c = it->second;
        if (!c->gpu && c->gx2fmt == gx2fmt && c->width == width && c->height == height && c->pitch == pitch &&
            c->slices == slices && c->dim == dim) {
            s = c;
            break;
        }
    }
    if (s && s->frame == g_frame) return s;  // validated already this frame
    uint64_t h = fnv(mem::host(addr), bytes);
    if (s && s->hash == h) {
        s->frame = g_frame;
        return s;
    }
    if (!s) {
        s = new Surf;
        s->addr = addr, s->swizzle = swizzle, s->pitch = pitch, s->width = width, s->height = height;
        s->slices = slices, s->gx2fmt = gx2fmt, s->tile = tile, s->dim = dim;
        s->tex = make_texture(f.pf, width, height, slices, dim, false);
        g_surfaces.emplace(addr, s);
    }
    s->hash = h;
    s->frame = g_frame;
    // detile slice by slice into a linear staging buffer, then upload
    std::vector<uint8_t> lin((size_t)we * he * f.bpe);
    for (uint32_t z = 0; z < slices; z++) {
        if (tile <= 1) {
            for (uint32_t y = 0; y < he; y++)
                memcpy(lin.data() + (size_t)y * we * f.bpe, mem::host(addr) + ((size_t)z * he_al + y) * pe * f.bpe,
                       (size_t)we * f.bpe);
        } else {
            gx2SurfaceCopySoftware(mem::host(addr), (int32_t)he, (int32_t)pe, (int32_t)slices, z, swizzle, tile, lin.data(),
                                   (int32_t)he, (int32_t)we, 1, 0, 0, 0, we, he, f.bpe * 8);
        }
        MTLRegion rg = MTLRegionMake2D(0, 0, width, height);
        id<MTLTexture> staging;
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.pf width:width height:height mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        staging = [g_dev newTextureWithDescriptor:d];
        [staging replaceRegion:rg mipmapLevel:0 withBytes:lin.data() bytesPerRow:(NSUInteger)we * f.bpe];
        // copy into the private texture on the GPU timeline (ordered with rendering)
        end_pass();
        id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
        [b copyFromTexture:staging sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(width, height, 1) toTexture:s->tex destinationSlice:(s->dim == 2 ? 0 : z)
          destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, s->dim == 2 ? z : 0)];
        [b endEncoding];
    }
    return s;
}
}  // namespace

// ---- frame / encoder management ------------------------------------------------------------------
namespace {
struct PassKey {
    id<MTLTexture> color[8];
    id<MTLTexture> depth;
    bool operator==(const PassKey& o) const {
        for (int i = 0; i < 8; i++)
            if (color[i] != o.color[i]) return false;
        return depth == o.depth;
    }
};
PassKey g_pass{};

void ensure_cb() {
    if (!g_cb) g_cb = [g_queue commandBuffer];
}
}  // namespace

void end_pass() {
    ensure_cb();
    if (g_enc) {
        [g_enc endEncoding];
        g_enc = nil;
    }
    g_pass = PassKey{};
}

namespace {
bool begin_pass(const PassKey& k) {
    if (g_enc && k == g_pass) return true;
    end_pass();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    bool any = false;
    for (int i = 0; i < 8; i++) {
        if (!k.color[i]) continue;
        rp.colorAttachments[i].texture = k.color[i];
        rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        any = true;
    }
    if (k.depth) {
        rp.depthAttachment.texture = k.depth;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        any = true;
    }
    if (!any) return false;
    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
    g_pass = k;
    return g_enc != nil;
}

// ---- shaders ---------------------------------------------------------------------------------------
struct Translation {
    std::vector<std::pair<uint32_t, uint32_t>> deps;
    id<MTLFunction> fn;
    msl::Output info;
    bool failed = false;
};
std::unordered_map<uint64_t, std::vector<Translation*>> g_shaders;  // by program (+fetch) hash
uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
int g_shader_log = 0;

Translation* shader(msl::Stage stage, uint32_t prog, uint32_t size, uint32_t fetch, uint32_t fetch_size, const uint32_t* regs) {
    if (!prog || !size || size > (1u << 20)) return nullptr;
    uint64_t key = hash_bytes(mem::host(prog), size) ^ (uint64_t)stage * 0x9E3779B97F4A7C15ull;
    if (fetch && fetch_size) key = hash_bytes(mem::host(fetch), fetch_size, key);
    auto& list = g_shaders[key];
    for (Translation* t : list) {
        bool match = true;
        for (auto& [r, v] : t->deps)
            if (regs[r] != v) {
                match = false;
                break;
            }
        if (match) return t->failed ? nullptr : t;
    }
    Scoped timer(T_SHADER);
    Translation* t = new Translation;
    msl::Input in;
    in.stage = stage;
    in.program = mem::host(prog);
    in.program_size = size;
    in.fetch = fetch_size ? mem::host(fetch) : nullptr;
    in.fetch_size = fetch_size;
    in.regs = regs;
    char name[64];
    snprintf(name, sizeof name, "%s_%016llx", stage == msl::Stage::Vertex ? "vs" : "ps", (unsigned long long)key);
    in.name = name;
    t->info = msl::emit(in);
    t->deps = t->info.deps;
    if (!t->info.ok) {
        t->failed = true;
        if (g_shader_log++ < 20) LOG("gpu_native: shader %s not translated: %s", name, t->info.error.c_str());
    } else {
        NSError* err = nil;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        if (@available(macOS 14.0, *)) opt.languageVersion = MTLLanguageVersion3_1;
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe;
        else opt.fastMathEnabled = NO;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:t->info.source.c_str()] options:opt error:&err];
        t->fn = lib ? [lib newFunctionWithName:[NSString stringWithUTF8String:name]] : nil;
        if (!t->fn) {
            t->failed = true;
            if (g_shader_log++ < 20)
                LOG("gpu_native: shader %s failed to compile: %s", name, err ? err.localizedDescription.UTF8String : "?");
        }
    }
    list.push_back(t);
    return t->failed ? nullptr : t;
}

// ---- pipeline / fixed-function state --------------------------------------------------------------
MTLBlendFactor blend_factor(uint32_t f) {
    switch (f) {
        case 0: return MTLBlendFactorZero;
        case 1: return MTLBlendFactorOne;
        case 2: return MTLBlendFactorSourceColor;
        case 3: return MTLBlendFactorOneMinusSourceColor;
        case 4: return MTLBlendFactorSourceAlpha;
        case 5: return MTLBlendFactorOneMinusSourceAlpha;
        case 6: return MTLBlendFactorDestinationAlpha;
        case 7: return MTLBlendFactorOneMinusDestinationAlpha;
        case 8: return MTLBlendFactorDestinationColor;
        case 9: return MTLBlendFactorOneMinusDestinationColor;
        case 10: return MTLBlendFactorSourceAlphaSaturated;
        case 13: return MTLBlendFactorBlendColor;
        case 14: return MTLBlendFactorOneMinusBlendColor;
        case 15: return MTLBlendFactorSource1Color;
        case 16: return MTLBlendFactorOneMinusSource1Color;
        case 17: return MTLBlendFactorSource1Alpha;
        case 18: return MTLBlendFactorOneMinusSource1Alpha;
        case 19: return MTLBlendFactorBlendAlpha;
        case 20: return MTLBlendFactorOneMinusBlendAlpha;
        default: return MTLBlendFactorOne;
    }
}
MTLBlendOperation blend_op(uint32_t f) {
    switch (f) {
        case 0: return MTLBlendOperationAdd;
        case 1: return MTLBlendOperationSubtract;
        case 2: return MTLBlendOperationMin;
        case 3: return MTLBlendOperationMax;
        case 4: return MTLBlendOperationReverseSubtract;
        default: return MTLBlendOperationAdd;
    }
}

std::unordered_map<uint64_t, id<MTLRenderPipelineState>> g_psos;
std::unordered_map<uint32_t, id<MTLDepthStencilState>> g_ds;
std::unordered_map<uint64_t, id<MTLSamplerState>> g_samplers;

id<MTLDepthStencilState> depth_state(uint32_t ctl, bool has_depth) {
    uint32_t key = has_depth ? ctl & 0x7E : 0;  // Z enable/write/func (stencil: phase 3)
    auto it = g_ds.find(key);
    if (it != g_ds.end()) return it->second;
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    if (has_depth && (ctl & 2)) {
        d.depthCompareFunction = (MTLCompareFunction)((ctl >> 4) & 7);  // same encoding as Metal
        d.depthWriteEnabled = (ctl >> 2) & 1;
    } else {
        d.depthCompareFunction = MTLCompareFunctionAlways;
        d.depthWriteEnabled = NO;
    }
    return g_ds[key] = [g_dev newDepthStencilStateWithDescriptor:d];
}

MTLSamplerAddressMode clamp_mode(uint32_t c) {
    switch (c) {
        case 0: return MTLSamplerAddressModeRepeat;
        case 1: return MTLSamplerAddressModeMirrorRepeat;
        case 2: return MTLSamplerAddressModeClampToEdge;
        case 3: case 5: case 7: return MTLSamplerAddressModeMirrorClampToEdge;
        case 4: case 6: return MTLSamplerAddressModeClampToBorderColor;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}

id<MTLSamplerState> sampler(const uint32_t* w, bool compare) {
    uint64_t key = (uint64_t)w[0] | ((uint64_t)(w[1] & 0xFFFFF) << 32) | ((uint64_t)compare << 63);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    d.sAddressMode = clamp_mode(w[0] & 7);
    d.tAddressMode = clamp_mode((w[0] >> 3) & 7);
    d.rAddressMode = clamp_mode((w[0] >> 6) & 7);
    uint32_t mag = (w[0] >> 9) & 7, minf = (w[0] >> 12) & 7, mip = (w[0] >> 17) & 3;
    d.magFilter = (mag == 1 || mag == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (minf == 1 || minf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = mip == 2 ? MTLSamplerMipFilterLinear : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    uint32_t aniso = (w[0] >> 19) & 7;
    if (aniso && (minf >= 4)) d.maxAnisotropy = std::min(16u, 1u << aniso);
    uint32_t border = (w[0] >> 22) & 3;
    d.borderColor = border == 1 ? MTLSamplerBorderColorOpaqueBlack : border == 2 ? MTLSamplerBorderColorOpaqueWhite
                                                                               : MTLSamplerBorderColorTransparentBlack;
    d.lodMinClamp = (float)(w[1] & 0x3FF) / 64.0f;
    d.lodMaxClamp = (float)((w[1] >> 10) & 0x3FF) / 64.0f;
    if (compare) d.compareFunction = (MTLCompareFunction)((w[0] >> 26) & 7);
    d.supportArgumentBuffers = NO;
    return g_samplers[key] = [g_dev newSamplerStateWithDescriptor:d];
}

// per-draw constants, must match StageInfo in the shader prelude (msl.cpp)
struct StageInfo {
    float tex_size[18][4];
    float tex_scale[18][4];
    uint32_t attrib_stride[16];
    uint32_t misc[4];
    float frag_coord_scale[4];
    float window_to_clip[4];
    float alpha_ref, point_size, pad0, pad1;
};

// index data in a per-frame ring (big-endian guest indices -> little-endian, primitive conversion)
struct Ring {
    id<MTLBuffer> buf;
    size_t off = 0, size = 64u << 20;
} g_ring;

uint64_t g_draws = 0, g_skipped = 0, g_skip_log = 0;
void skip(const char* why) {
    g_skipped++;
    if (g_skip_log++ < 30) LOG("gpu_native: draw skipped: %s", why);
}
}  // namespace

// ---- interface ------------------------------------------------------------------------------------
void create(void* nsview, int w, int h) {
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) {
        LOG("gpu_native: no Metal device");
        return;
    }
    g_queue = [g_dev newCommandQueue];
    // all of guest memory as one buffer (no copy, pages are not committed by Metal)
    g_guest = [g_dev newBufferWithBytesNoCopy:g_mem + kGuestBase length:(1ull << 32) options:MTLResourceStorageModeShared
                                  deallocator:nil];
    g_ring.buf = [g_dev newBufferWithLength:g_ring.size options:MTLResourceStorageModeShared];
    g_ok = g_guest && g_ring.buf;
    LOG("gpu_native: Metal renderer on %s, guest memory buffer %s (%dx%d view)", g_dev.name.UTF8String, g_guest ? "ok" : "FAILED",
        w, h);
}
void resize(int, int) {}
void set_scanout_srgb(bool tv, bool srgb) {
    if (tv) g_tv_srgb = srgb;
}
void set_capture(int every, const char* dir) {
    g_capture_every = every;
    g_capture_dir = std::string(dir) + "-native";
    mkdir(g_capture_dir.c_str(), 0755);
}

void idle() {
    if (!g_ok) return;
    if (g_cb) {
        end_pass();
        [g_cb commit];
        g_cb = nil;
    }
}

void after_completion(std::function<void()> fn) {
    if (!g_ok) {
        fn();
        return;
    }
    end_pass();
    id<MTLCommandBuffer> cb = g_cb;
    g_cb = nil;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) { fn(); }];
    [cb commit];
}

void draw(const Draw& d) {
    if (!g_ok) return;
    Scoped timer(T_DRAW);
    const uint32_t* r = d.regs;
    ensure_cb();
    // render targets
    PassKey pk{};
    MTLPixelFormat color_pf[8] = {};
    uint32_t mask = r[CB_TARGET_MASK];
    uint32_t color_kinds = 0;
    // attach only targets the pixel shader exports: GX2 leaves stale CB_COLORn registers (and a full
    // CB_TARGET_MASK) for targets that the current shader does not write
    mask &= r[CB_SHADER_MASK];
    for (int i = 0; i < 8; i++) {
        uint32_t m = (mask >> (i * 4)) & 0xF;
        uint32_t base = r[CB_COLOR0_BASE + i], info = r[CB_COLOR0_INFO + i], size = r[CB_COLOR0_SIZE + i];
        if (!m || !base || !((info >> 2) & 0x3F)) continue;
        uint32_t tile = (info >> 8) & 0xF, swz = 0;
        if (tile >= 4) swz = base & 0x700, base &= ~0x700u;
        uint32_t pitch = ((size & 0x3FF) + 1) * 8, height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
        Surf* s = rt_surface(base & ~0xFFu, swz, cb_format(info), pitch, height, tile, false, MTLPixelFormatInvalid);
        if (!s) return skip("unsupported color target format");
        pk.color[i] = s->tex;
        color_pf[i] = s->tex.pixelFormat;
    }
    // depth (GX2SetDepthBuffer puts the address in DB_HTILE_DATA_BASE, see GX2_RenderTarget.cpp)
    uint32_t dctl = r[DB_DEPTH_CONTROL];
    bool use_depth = false;
    MTLPixelFormat depth_pf = MTLPixelFormatInvalid;
    if (dctl & 6) {
        uint32_t dinfo = r[DB_DEPTH_INFO], dbase = r[DB_DEPTH_INFO + 1] << 8, dsize = r[DB_DEPTH_SIZE];
        uint32_t df = dinfo & 7;
        depth_pf = df == 1 ? MTLPixelFormatDepth16Unorm : df == 6 ? MTLPixelFormatDepth32Float
                 : df == 3 || df == 5 || df == 7 ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid;
        if (dbase && depth_pf != MTLPixelFormatInvalid) {
            uint32_t pitch = ((dsize & 0x3FF) + 1) * 8, height = (((dsize >> 10) & 0xFFFFF) + 1) * 64 / pitch;
            Surf* s = rt_surface(dbase & ~0x7FFu, dbase & 0x700, 0x1000 | df, pitch, height, (dinfo >> 15) & 0xF, true, depth_pf);
            if (s) pk.depth = s->tex, use_depth = true;
        }
    }
    if (!use_depth) depth_pf = MTLPixelFormatInvalid;
    // shaders
    bool gs = (r[VGT_GS_MODE] & 3) != 0;
    if (gs) return skip("geometry shader");
    Translation* vs = shader(msl::Stage::Vertex, r[SQ_PGM_START_VS] << 8, r[SQ_PGM_SIZE_VS] << 3, r[SQ_PGM_START_FS] << 8,
                             r[SQ_PGM_SIZE_FS] << 3, r);
    Translation* ps = shader(msl::Stage::Pixel, r[SQ_PGM_START_PS] << 8, r[SQ_PGM_SIZE_PS] << 3, 0, 0, r);
    if (!vs || !ps) return skip("shader unavailable");
    // pipeline
    uint64_t pkey = (uint64_t)(uintptr_t)(__bridge void*)vs->fn * 31 ^ (uint64_t)(uintptr_t)(__bridge void*)ps->fn;
    for (int i = 0; i < 8; i++) pkey = pkey * 1099511628211ull ^ ((uint64_t)color_pf[i] << 8 | ((mask >> (i * 4)) & 0xF));
    pkey = pkey * 1099511628211ull ^ (uint64_t)depth_pf;
    pkey = pkey * 1099511628211ull ^ r[CB_COLOR_CONTROL];
    for (int i = 0; i < 8; i++) pkey = pkey * 1099511628211ull ^ r[CB_BLEND0_CONTROL + i];
    id<MTLRenderPipelineState> pso;
    auto pit = g_psos.find(pkey);
    if (pit != g_psos.end()) pso = pit->second;
    else {
        Scoped timer(T_PIPELINE);
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vs->fn;
        pd.fragmentFunction = ps->fn;
        uint32_t cc = r[CB_COLOR_CONTROL];
        for (int i = 0; i < 8; i++) {
            if (color_pf[i] == MTLPixelFormatInvalid) continue;
            auto* ca = pd.colorAttachments[i];
            ca.pixelFormat = color_pf[i];
            uint32_t m = (mask >> (i * 4)) & 0xF;
            ca.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) |
                           (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
            uint32_t b = r[CB_BLEND0_CONTROL + i];
            bool blend = (cc >> (8 + i)) & 1;
            if (blend && fmt_of(r[CB_COLOR0_INFO + i] ? cb_format(r[CB_COLOR0_INFO + i]) : 0).kind == 0) {
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = blend_factor(b & 0x1F);
                ca.rgbBlendOperation = blend_op((b >> 5) & 7);
                ca.destinationRGBBlendFactor = blend_factor((b >> 8) & 0x1F);
                if ((b >> 29) & 1) {
                    ca.sourceAlphaBlendFactor = blend_factor((b >> 16) & 0x1F);
                    ca.alphaBlendOperation = blend_op((b >> 21) & 7);
                    ca.destinationAlphaBlendFactor = blend_factor((b >> 24) & 0x1F);
                } else {
                    ca.sourceAlphaBlendFactor = ca.sourceRGBBlendFactor;
                    ca.alphaBlendOperation = ca.rgbBlendOperation;
                    ca.destinationAlphaBlendFactor = ca.destinationRGBBlendFactor;
                }
            }
        }
        pd.depthAttachmentPixelFormat = depth_pf;
        if (depth_pf == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = depth_pf;
        NSError* err = nil;
        pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        g_psos[pkey] = pso;
        if (!pso && g_skip_log < 30) LOG("gpu_native: pipeline failed: %s", err ? err.localizedDescription.UTF8String : "?");
    }
    if (!pso) return skip("pipeline");
    if (!begin_pass(pk)) return skip("no render target");
    [g_enc setRenderPipelineState:pso];
    [g_enc setDepthStencilState:depth_state(dctl, use_depth)];
    // rasterizer
    uint32_t su = r[PA_SU_SC_MODE_CNTL];
    [g_enc setCullMode:(su & 3) == 3 ? MTLCullModeNone : (su & 1) ? MTLCullModeFront : (su & 2) ? MTLCullModeBack : MTLCullModeNone];
    [g_enc setFrontFacingWinding:(su & 4) ? MTLWindingClockwise : MTLWindingCounterClockwise];
    auto rf = [&](uint32_t reg) { float f; memcpy(&f, &r[reg], 4); return f; };
    float xs = rf(0xA10F), xo = rf(0xA110), ys = rf(0xA111), yo = rf(0xA112), zs = rf(0xA113), zo = rf(0xA114);
    bool halfz = (r[PA_CL_CLIP_CNTL] >> 19) & 1;
    MTLViewport vp{xo - xs, yo + ys, xs * 2.0, ys * -2.0, halfz ? zo : zo - zs, zs + zo};
    [g_enc setViewport:vp];
    uint32_t tl = r[0xA090], br = r[0xA091];
    uint32_t sx = tl & 0x7FFF, sy = (tl >> 16) & 0x7FFF, ex = br & 0x7FFF, ey = (br >> 16) & 0x7FFF;
    id<MTLTexture> any = pk.color[0] ? pk.color[0] : pk.depth;
    for (int i = 0; !any && i < 8; i++) any = pk.color[i];
    ex = std::min<uint32_t>(ex, (uint32_t)any.width), ey = std::min<uint32_t>(ey, (uint32_t)any.height);
    if (ex <= sx || ey <= sy) return skip("empty scissor");
    [g_enc setScissorRect:MTLScissorRect{sx, sy, ex - sx, ey - sy}];
    // per-stage bindings
    auto fail_bind = [&](const char* why, uint32_t v) {
        if (g_skip_log < 30) LOG("gpu_native: binding failed: %s (%x)", why, v);
        return false;
    };
    auto bind_stage = [&](Translation* t, bool vertex) -> bool {
        StageInfo si{};
        uint32_t texbase = vertex ? kResourceBase + kResVsTex : kResourceBase + kResPsTex;
        uint32_t ubbase = vertex ? kResourceBase + kResVsUbo : kResourceBase + kResPsUbo;
        uint32_t smpbase = vertex ? kSamplerBase + 18 * 3 : kSamplerBase;
        for (auto& b : t->info.bindings) {
            if (b.kind == "cfile") {
                const uint32_t* c = r + kAluConstBase + (vertex ? 0x400 : 0);
                if (vertex) [g_enc setVertexBytes:c length:4096 atIndex:b.index];
                else [g_enc setFragmentBytes:c length:4096 atIndex:b.index];
            } else if (b.kind == "ub") {
                uint32_t a = r[ubbase + b.slot * 7];
                if (a < kGuestBase) return fail_bind("uniform block address 0", b.slot);
                if (vertex) [g_enc setVertexBuffer:g_guest offset:a - kGuestBase atIndex:b.index];
                else [g_enc setFragmentBuffer:g_guest offset:a - kGuestBase atIndex:b.index];
            } else if (b.kind == "ab") {
                const uint32_t* w = r + kResourceBase + kResAttrib + b.slot * 7;
                if (w[0] < kGuestBase) return fail_bind("attribute buffer address 0", b.slot);
                si.attrib_stride[b.slot] = (w[2] >> 11) & 0xFFFF;
                [g_enc setVertexBuffer:g_guest offset:w[0] - kGuestBase atIndex:b.index];
            } else if (b.kind == "tex") {
                const uint32_t* w = r + texbase + b.slot * 7;
                uint32_t addr = w[2] << 8;
                if (!addr) return fail_bind("texture address 0", b.slot);
                uint32_t tile = (w[0] >> 3) & 0xF, swz = 0;
                if (tile >= 4) swz = addr & 0x700, addr &= ~0x700u;
                uint32_t dim = w[0] & 7, width = ((w[0] >> 19) & 0x1FFF) + 1, height = (w[1] & 0x1FFF) + 1;
                uint32_t pitch = (((w[0] >> 8) & 0x7FF) + 1) * 8;
                uint32_t gf = tex_format(w);
                Fmt f = fmt_of(gf);
                if (f.block == 4) pitch /= 4;
                uint32_t slices = 1;
                if (dim == 5 || dim == 2) slices = ((w[1] >> 13) & 0x1FFF) + 1;
                if (dim == 3) slices = 6 * (((w[1] >> 13) & 0x1FFF) + 1);
                if (dim == 0 || dim == 4) height = 1;
                id<MTLTexture> tex;
                Surf* g = find_gpu_surface(addr, f.bpe);
                if (g && (g->gx2fmt & 0x1000)) {
                    return fail_bind("depth buffer sampled as color (needs copy pass)", g->gx2fmt);
                } else if (g && f.pf != MTLPixelFormatInvalid && view_ok(g->tex.pixelFormat, f.pf)) {
                    tex = swizzled(g->tex, f.pf, w[4], f.swap_rb && f.pf == g->tex.pixelFormat);
                } else {
                    Surf* s = guest_texture(addr, swz, gf, pitch, width, height, slices, tile, dim);
                    if (!s) return fail_bind("texture format/size", gf);
                    tex = swizzled(s->tex, s->tex.pixelFormat, w[4], f.swap_rb);
                }
                if (!tex) return fail_bind("texture view", gf);
                si.tex_size[b.slot][0] = (float)width, si.tex_size[b.slot][1] = (float)height;
                si.tex_size[b.slot][2] = (float)slices, si.tex_size[b.slot][3] = 1.0f;
                si.tex_scale[b.slot][0] = (float)tex.width / (float)width;
                si.tex_scale[b.slot][1] = (float)tex.height / (float)height;
                if (vertex) [g_enc setVertexTexture:tex atIndex:b.index];
                else [g_enc setFragmentTexture:tex atIndex:b.index];
            } else if (b.kind == "smp") {
                const uint32_t* w = r + smpbase + b.slot * 3;
                id<MTLSamplerState> s = sampler(w, false);
                if (vertex) [g_enc setVertexSamplerState:s atIndex:b.index];
                else [g_enc setFragmentSamplerState:s atIndex:b.index];
            }
        }
        si.frag_coord_scale[0] = si.frag_coord_scale[1] = 1.0f;
        si.window_to_clip[0] = any ? 2.0f / (float)any.width : 1.0f;
        si.window_to_clip[1] = any ? -2.0f / (float)any.height : 1.0f;
        si.window_to_clip[2] = -1.0f, si.window_to_clip[3] = 1.0f;
        float aref;
        memcpy(&aref, &r[0xA10E], 4);  // SX_ALPHA_REF
        si.alpha_ref = aref;
        si.misc[0] = r[SQ_VTX_BASE_VTX_LOC], si.misc[1] = r[SQ_VTX_START_INST_LOC];
        if (vertex) [g_enc setVertexBytes:&si length:sizeof si atIndex:0];
        else [g_enc setFragmentBytes:&si length:sizeof si atIndex:0];
        return true;
    };
    if (!bind_stage(vs, true) || !bind_stage(ps, false)) return skip("resource binding");
    // primitive + indices
    uint32_t prim = r[VGT_PRIMITIVE_TYPE];
    MTLPrimitiveType mp;
    bool quads = false;
    switch (prim) {
        case 1: mp = MTLPrimitiveTypePoint; break;
        case 2: mp = MTLPrimitiveTypeLine; break;
        case 3: mp = MTLPrimitiveTypeLineStrip; break;
        case 4: mp = MTLPrimitiveTypeTriangle; break;
        case 6: mp = MTLPrimitiveTypeTriangleStrip; break;
        case 0x13: mp = MTLPrimitiveTypeTriangle, quads = true; break;
        default: return skip("primitive type");
    }
    uint32_t count = d.count;
    uint32_t base_vertex = r[SQ_VTX_BASE_VTX_LOC], base_instance = r[SQ_VTX_START_INST_LOC];
    if (d.index_addr || quads) {
        Scoped timer(T_INDEX);
        uint32_t n = quads ? count / 4 * 6 : count;
        if (!n) return;
        size_t bytes = (size_t)n * 4;
        if (g_ring.off + bytes > g_ring.size) g_ring.off = 0;  // frames in flight are short; ring is large
        uint32_t* out = (uint32_t*)((uint8_t*)g_ring.buf.contents + g_ring.off);
        auto idx = [&](uint32_t i) -> uint32_t {
            if (!d.index_addr) return i;
            uint32_t t = d.index_type;
            if (t == 4) return mem::r16(d.index_addr + i * 2);   // U16 big-endian
            if (t == 9) return mem::r32(d.index_addr + i * 4);   // U32 big-endian
            if (t == 0) { uint16_t v; memcpy(&v, mem::host(d.index_addr + i * 2), 2); return v; }
            uint32_t v;
            memcpy(&v, mem::host(d.index_addr + i * 4), 4);
            return v;
        };
        if (quads)
            for (uint32_t q = 0; q < count / 4; q++) {
                uint32_t a = idx(q * 4), b = idx(q * 4 + 1), c = idx(q * 4 + 2), e = idx(q * 4 + 3);
                uint32_t* o = out + q * 6;
                o[0] = a, o[1] = b, o[2] = c, o[3] = a, o[4] = c, o[5] = e;
            }
        else
            for (uint32_t i = 0; i < n; i++) out[i] = idx(i);
        [g_enc drawIndexedPrimitives:mp indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:g_ring.buf
                   indexBufferOffset:g_ring.off instanceCount:std::max(1u, d.instances) baseVertex:(NSInteger)base_vertex
                        baseInstance:base_instance];
        g_ring.off = (g_ring.off + bytes + 255) & ~(size_t)255;
    } else {
        [g_enc drawPrimitives:mp vertexStart:base_vertex vertexCount:count instanceCount:std::max(1u, d.instances)
                 baseInstance:base_instance];
    }
    g_draws++;
}

void clear(uint32_t mask, const Surface& c, const Surface& z, const float rgba[4], float depth_value, uint32_t stencil) {
    if (!g_ok) return;
    ensure_cb();
    end_pass();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    bool any = false;
    if ((mask & 1) && c.addr) {
        uint32_t addr = c.addr, swz = 0;
        if (c.tilemode >= 4) swz = addr & 0x700, addr &= ~0x700u;
        // clears name the surface by GX2 format and pitch/height
        auto* surf = rt_surface(addr & ~0xFFu, swz, c.format, c.pitch, c.height, c.tilemode, false, MTLPixelFormatInvalid);
        if (surf) {
            rp.colorAttachments[0].texture = surf->tex;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
            any = true;
        }
    }
    if ((mask & 6) && z.addr) {
        uint32_t df = (z.format & 0x3F) == 0x05 ? 1 : (z.format == 0x80E ? 6 : (z.format & 0x3F) == 0x11 ? 3 : 7);
        MTLPixelFormat pf = df == 1 ? MTLPixelFormatDepth16Unorm : df == 6 ? MTLPixelFormatDepth32Float : MTLPixelFormatDepth32Float_Stencil8;
        auto* surf = rt_surface(z.addr & ~0x7FFu, z.addr & 0x700, 0x1000 | df, z.pitch, z.height, z.tilemode, true, pf);
        if (surf) {
            rp.depthAttachment.texture = surf->tex;
            rp.depthAttachment.loadAction = (mask & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = depth_value;
            any = true;
        }
    }
    if (!any) return;
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
}

void copy_surface(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t, uint32_t) {
    // GX2CopySurface (texture uploads from LINEAR_SPECIAL): the destination is re-read from guest
    // memory on next use, because the HLE side already wrote the tiled data (GX2CopySurfaceInternal)
}

namespace {
id<MTLTexture> g_tv;  // last TV scan-out image
}

void scanout_copy(const Surface& c, uint32_t target) {
    if (!g_ok || target != 1) return;  // TV only for now
    uint32_t addr = c.addr;
    if (c.tilemode >= 4) addr &= ~0x700u;  // macro-tile swizzle bits
    Fmt f = fmt_of(c.format);
    Surf* s = find_gpu_surface(addr, f.bpe, c.width, c.height, c.format);
    if (!s) return;
    ensure_cb();
    end_pass();
    if (!g_tv || g_tv.pixelFormat != s->tex.pixelFormat || g_tv.width != c.width || g_tv.height != c.height) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s->tex.pixelFormat width:c.width
                                                                                   height:c.height mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        g_tv = [g_dev newTextureWithDescriptor:d];
    }
    id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
    [b copyFromTexture:s->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(std::min<NSUInteger>(c.width, s->tex.width), std::min<NSUInteger>(c.height, s->tex.height), 1)
             toTexture:g_tv destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
}

namespace {
void write_png(const std::vector<uint8_t>& rgb, int w, int h, const std::string& path) {
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

void capture_tv(uint64_t frame) {
    if (!g_tv) return;
    uint32_t w = (uint32_t)g_tv.width, h = (uint32_t)g_tv.height;
    MTLPixelFormat pf = g_tv.pixelFormat;
    uint32_t bpp = pf == MTLPixelFormatRGBA16Float ? 8 : 4;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:(NSUInteger)w * h * bpp options:MTLResourceStorageModeShared];
    ensure_cb();
    end_pass();
    id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
    [b copyFromTexture:g_tv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * bpp destinationBytesPerImage:(NSUInteger)w * h * bpp];
    [b endEncoding];
    std::string path = g_capture_dir + "/frame_" + std::to_string(frame) + ".png";
    bool srgb = g_tv_srgb;
    [g_cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        const uint8_t* src = (const uint8_t*)buf.contents;
        auto enc = [&](float v) {
            v = std::clamp(v, 0.0f, 1.0f);
            if (srgb) v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
            return (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
        };
        for (uint32_t i = 0; i < w * h; i++) {
            float c[3];
            if (pf == MTLPixelFormatRGB10A2Unorm) {
                uint32_t v = ((const uint32_t*)src)[i];
                c[0] = (v & 1023) / 1023.0f, c[1] = ((v >> 10) & 1023) / 1023.0f, c[2] = ((v >> 20) & 1023) / 1023.0f;
            } else if (bpp == 8) {
                const __fp16* hp = (const __fp16*)(src + i * 8);
                c[0] = hp[0], c[1] = hp[1], c[2] = hp[2];
            } else {
                c[0] = src[i * 4] / 255.0f, c[1] = src[i * 4 + 1] / 255.0f, c[2] = src[i * 4 + 2] / 255.0f;
            }
            for (int k = 0; k < 3; k++) rgb[i * 3 + k] = enc(c[k]);
        }
        write_png(rgb, (int)w, (int)h, path);
    }];
    LOG("gpu_native: capture %s (%ux%u)", path.c_str(), w, h);
}
}  // namespace

void present() {
    if (!g_ok) return;
    g_frame++;
    if (g_capture_every && g_frame % g_capture_every == 0) capture_tv(g_frame);
    if (g_frame % 300 == 0)
        LOG("gpu_native: frame %llu: %llu draws so far, %llu skipped, %zu shaders, %zu pipelines, %zu surfaces",
            (unsigned long long)g_frame, (unsigned long long)g_draws, (unsigned long long)g_skipped, g_shaders.size(),
            g_psos.size(), g_surfaces.size());
}

void surface_sync(uint32_t, uint32_t) {}
void occlusion_begin(uint32_t) {}
void occlusion_end(uint32_t q) {
    // report "visible" until Metal visibility queries are wired up (start 0, end 1, CPU marker cleared)
    uint64_t z = 0, one = 1;
    memcpy(mem::host(q), &z, 8);
    memcpy(mem::host(q + 8), &one, 8);
    memcpy(mem::host(q + 16), &z, 8);
    memcpy(mem::host(q + 24), &z, 8);
    memcpy(mem::host(q + 32), &z, 8);
}
}  // namespace gpu_native::renderer
#endif  // BOTW_NATIVE_GPU
