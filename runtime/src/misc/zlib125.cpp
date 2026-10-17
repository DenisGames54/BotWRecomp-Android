// zlib125: guest z_streams backed by host zlib. Each guest stream owns a host z_stream (host
// allocator, so the guest zalloc/zfree are never called); next/avail/total fields are copied across
// on every call. The guest `state` field is set non-null while the stream is initialized.
#include <mutex>
#include <unordered_map>
#include <zlib.h>

#include "../hle.h"

namespace {
// guest z_stream: +00 next_in, +04 avail_in, +08 total_in, +0C next_out, +10 avail_out,
// +14 total_out, +18 msg, +1C state, +20 zalloc, +24 zfree, +28 opaque, +2C data_type, +30 adler
struct Stream {
    z_stream z{};
    bool deflate = false;
};
std::mutex g_lock;
std::unordered_map<uint32_t, Stream*> g_streams;

Stream* sync_in(uint32_t gs) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_streams.find(gs);
    if (it == g_streams.end()) return nullptr;
    z_stream& z = it->second->z;
    z.next_in = mem::r32(gs + 0x00) ? mem::host(mem::r32(gs + 0x00)) : nullptr;
    z.avail_in = mem::r32(gs + 0x04);
    z.total_in = mem::r32(gs + 0x08);
    z.next_out = mem::r32(gs + 0x0C) ? mem::host(mem::r32(gs + 0x0C)) : nullptr;
    z.avail_out = mem::r32(gs + 0x10);
    z.total_out = mem::r32(gs + 0x14);
    return it->second;
}

void sync_out(uint32_t gs, const z_stream& z) {
    mem::w32(gs + 0x00, z.next_in ? mem::guest(z.next_in) : 0);
    mem::w32(gs + 0x04, z.avail_in);
    mem::w32(gs + 0x08, (uint32_t)z.total_in);
    mem::w32(gs + 0x0C, z.next_out ? mem::guest(z.next_out) : 0);
    mem::w32(gs + 0x10, z.avail_out);
    mem::w32(gs + 0x14, (uint32_t)z.total_out);
    mem::w32(gs + 0x18, 0);  // msg: host string, not visible to the guest
    mem::w32(gs + 0x2C, z.data_type);
    mem::w32(gs + 0x30, (uint32_t)z.adler);
}

void release(uint32_t gs) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_streams.find(gs);
    if (it == g_streams.end()) return;
    if (it->second->deflate) deflateEnd(&it->second->z);
    else inflateEnd(&it->second->z);
    delete it->second;
    g_streams.erase(it);
}

int32_t init(uint32_t gs, bool deflate, int level, int method, int window_bits, int mem_level, int strategy) {
    release(gs);  // re-init without End
    auto* s = new Stream;
    s->deflate = deflate;
    int r = deflate ? deflateInit2(&s->z, level, method, window_bits, mem_level, strategy)
                    : inflateInit2(&s->z, window_bits);
    if (r != Z_OK) {
        delete s;
        mem::w32(gs + 0x1C, 0);
        return r;
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_streams[gs] = s;
    }
    sync_in(gs);
    s->z.total_in = s->z.total_out = 0;
    sync_out(gs, s->z);
    mem::w32(gs + 0x1C, gs);
    return r;
}

int32_t run(uint32_t gs, int flush, bool deflate_op) {
    Stream* s = sync_in(gs);
    if (!s || s->deflate != deflate_op) return Z_STREAM_ERROR;
    int r = deflate_op ? deflate(&s->z, flush) : inflate(&s->z, flush);
    sync_out(gs, s->z);
    return r;
}

int32_t end(uint32_t gs) {
    Stream* s = sync_in(gs);
    if (!s) return Z_STREAM_ERROR;
    sync_out(gs, s->z);
    release(gs);
    mem::w32(gs + 0x1C, 0);
    return Z_OK;
}
}  // namespace

// inflateInit2_(strm, windowBits, version, stream_size)
HLE_EXPORT(zlib125, inflateInit2_) { RET(init(ARG(0), false, 0, 0, (int32_t)ARG(1), 0, 0)); }
HLE_EXPORT(zlib125, inflate) { RET(run(ARG(0), (int32_t)ARG(1), false)); }
HLE_EXPORT(zlib125, inflateEnd) { RET(end(ARG(0))); }
// deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, stream_size)
HLE_EXPORT(zlib125, deflateInit2_) {
    RET(init(ARG(0), true, (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5)));
}
HLE_EXPORT(zlib125, deflate) { RET(run(ARG(0), (int32_t)ARG(1), true)); }
HLE_EXPORT(zlib125, deflateEnd) { RET(end(ARG(0))); }
