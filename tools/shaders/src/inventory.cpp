// See inventory.h. Formats (Cemu, MPL-2.0, re-implemented here for offline reading):
//   FileCache V3: u32 magic 0x8371b696, u32 extraVersion, u64 dataOffset, u64 tableOffset, u32 tableSize
//     (little-endian); table entries {u64 name1, u64 name2, u64 offset, u32 size, u8 flags, u8[3]}
//     at dataOffset+tableOffset; flag 1 = zlib (u32 BE uncompressed size + deflate stream).
//   shader entry: u8 version|type<<4, u64 baseHash, u64 auxHash (BE), then per type:
//     vertex:   u8 usesGS, regstate, program(fetch), program(vs)
//     geometry: u16 vsRingParameterCount, regstate, program(gs copy), program(gs)
//     pixel:    u8 usesGS, regstate, program(ps)
//   regstate: u8 1, u16 size, zstd(dict) of RLE(1854 words over g_gpuRegSerializerMapping_v1)
//   program:  u8 1, u32 size, u32 compressedSize, zstd(dict) data
#include "inventory.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>

#include <fstream>
#include <regex>
#include <sstream>

namespace latte_inv {
namespace {
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// bytes of a C array "<name>[...] = { 0x.., ... };" in a source file
std::vector<uint8_t> c_array(const std::string& src, const std::string& name) {
    std::vector<uint8_t> out;
    size_t p = src.find(name + "[");
    while (p != std::string::npos) {
        size_t eq = src.find('=', p), semi = src.find(';', p);
        if (eq != std::string::npos && eq < semi) break;  // the definition, not a declaration
        p = src.find(name + "[", p + 1);
    }
    if (p == std::string::npos) return out;
    size_t b = src.find('{', p), e = src.find("};", b);
    for (size_t i = b; i < e; i++) {
        if (src[i] == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
            out.push_back((uint8_t)strtoul(src.c_str() + i, nullptr, 16));
            i += 2;
        }
    }
    return out;
}

struct Dicts {
    ZSTD_DDict* shader = nullptr;
    ZSTD_DDict* regs = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> reg_map;  // (first register, count)
};

bool load_dicts(const std::string& cemu_root, Dicts& d, std::string& err) {
    std::string common = cemu_root + "/Cafe/HW/Latte/Common/";
    std::string ss = slurp(common + "ShaderSerializer.cpp"), rs = slurp(common + "RegisterSerializer.cpp");
    if (ss.empty() || rs.empty()) {
        err = "cannot read Cemu serializer sources under " + common;
        return false;
    }
    auto mk = [&](const std::string& src, const char* name, ZSTD_DDict*& out) {
        std::vector<uint8_t> z = c_array(src, name);
        std::vector<uint8_t> dict(0x1B800);
        size_t n = ZSTD_decompress(dict.data(), dict.size(), z.data(), z.size());
        if (ZSTD_isError(n)) return false;
        out = ZSTD_createDDict(dict.data(), n);
        return out != nullptr;
    };
    if (!mk(ss, "s_shaderDict", d.shader) || !mk(rs, "s_regDataDict", d.regs)) {
        err = "cannot build zstd dictionaries";
        return false;
    }
    size_t a = rs.find("g_gpuRegSerializerMapping_v1[] =");
    size_t e = rs.find("};", a);
    std::regex pair(R"(\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\})");
    std::string tbl = rs.substr(a, e - a);
    for (std::sregex_iterator it(tbl.begin(), tbl.end(), pair), end; it != end; ++it)
        d.reg_map.push_back({(uint32_t)strtoul((*it)[1].str().c_str(), nullptr, 16),
                             (uint32_t)strtoul((*it)[2].str().c_str(), nullptr, 16)});
    uint32_t total = 0;
    for (auto& [r, n] : d.reg_map) total += n;
    if (total != 1854) {
        err = "register map has " + std::to_string(total) + " registers, expected 1854";
        return false;
    }
    return true;
}

struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool bad = false;
    bool need(size_t k) {
        if (i + k > n) bad = true;
        return !bad;
    }
    uint8_t u8() { return need(1) ? p[i++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = (uint16_t)(p[i] << 8 | p[i + 1]);
        i += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 | (uint32_t)p[i + 2] << 8 | p[i + 3];
        i += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t hi = u32();
        return hi << 32 | u32();
    }
    const uint8_t* bytes(size_t k) {
        if (!need(k)) return nullptr;
        const uint8_t* r = p + i;
        i += k;
        return r;
    }
};

bool read_regs(Reader& r, const Dicts& d, std::vector<uint32_t>& regs) {
    if (r.u8() != 1) return false;
    uint16_t csize = r.u16();
    const uint8_t* c = r.bytes(csize);
    if (!c) return false;
    uint8_t rle[8 * 1024];
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    size_t n = ZSTD_decompress_usingDDict(dc, rle, sizeof rle, c, csize, d.regs);
    ZSTD_freeDCtx(dc);
    if (ZSTD_isError(n)) return false;
    // RLE: byte b; b&0x80 -> (b&0x7f) zero words, else b big-endian words
    std::vector<uint32_t> flat(1854);
    Reader q{rle, n};
    uint32_t idx = 0;
    while (idx < 1854) {
        uint8_t b = q.u8(), cnt = b & 0x7F;
        if (q.bad || !cnt || idx + cnt > 1854) return false;
        if (b & 0x80) idx += cnt;
        else
            while (cnt--) flat[idx++] = q.u32();
    }
    regs.assign(0x10000, 0);
    uint32_t k = 0;
    for (auto& [first, cnt] : d.reg_map)
        for (uint32_t j = 0; j < cnt; j++) regs[first + j] = flat[k++];
    return true;
}

bool read_program(Reader& r, const Dicts& d, std::vector<uint8_t>& out) {
    if (r.u8() != 1) return false;
    uint32_t size = r.u32(), csize = r.u32();
    if (r.bad || !size || size >= (128u << 20) || !csize) return false;
    const uint8_t* c = r.bytes(csize);
    if (!c) return false;
    out.resize(size);
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    size_t n = ZSTD_decompress_usingDDict(dc, out.data(), size, c, csize, d.shader);
    ZSTD_freeDCtx(dc);
    return !ZSTD_isError(n) && n == size;
}

bool parse_entry(const std::vector<uint8_t>& blob, const Dicts& d, Entry& e) {
    Reader r{blob.data(), blob.size()};
    uint8_t vt = r.u8();
    if ((vt & 0xF) != 1) return false;
    uint8_t type = vt >> 4;
    e.base_hash = r.u64();
    e.aux_hash = r.u64();
    auto regs = std::make_shared<std::vector<uint32_t>>();
    if (type == 0) {
        e.stage = Stage::Vertex;
        e.uses_gs = r.u8() != 0;
        if (!read_regs(r, d, *regs) || !read_program(r, d, e.fetch) || !read_program(r, d, e.program)) return false;
    } else if (type == 1) {
        e.stage = Stage::Geometry;
        e.vs_ring_params = r.u16();
        if (!read_regs(r, d, *regs) || !read_program(r, d, e.gs_copy) || !read_program(r, d, e.program)) return false;
    } else if (type == 2) {
        e.stage = Stage::Pixel;
        e.uses_gs = r.u8() != 0;
        if (!read_regs(r, d, *regs) || !read_program(r, d, e.program)) return false;
    } else
        return false;
    e.regs = regs;
    return !r.bad && r.i == r.n;
}
}  // namespace

bool load(const std::string& cemu_root, const std::string& path, Inventory& out, std::string& err) {
    static Dicts d;
    static bool have = false;
    if (!have && !(have = load_dicts(cemu_root, d, err))) return false;
    std::string f = slurp(path);
    if (f.size() < 32) {
        err = "cannot read " + path;
        return false;
    }
    auto le32 = [&](size_t o) { uint32_t v; memcpy(&v, f.data() + o, 4); return v; };
    auto le64 = [&](size_t o) { uint64_t v; memcpy(&v, f.data() + o, 8); return v; };
    if (le32(0) != 0x8371b696) {
        err = "not a FileCache V3 file: " + path;
        return false;
    }
    uint64_t data_off = le64(8), table_off = le64(16);
    uint32_t table_size = le32(24);
    size_t t = data_off + table_off;
    for (uint32_t k = 0; k < table_size / 32; k++) {
        size_t o = t + k * 32;
        if (o + 32 > f.size()) break;
        uint64_t n1 = le64(o), n2 = le64(o + 8), off = le64(o + 16);
        uint32_t size = le32(o + 24);
        uint8_t flags = (uint8_t)f[o + 28];
        if ((n1 == 0 && n2 == 0) || (n1 == 0xEFEFEFEFEFEFEFEFull && n2 == 0xFEFEFEFEFEFEFEFEull)) continue;
        if (data_off + off + size > f.size()) {
            out.bad++;
            continue;
        }
        const uint8_t* raw = (const uint8_t*)f.data() + data_off + off;
        std::vector<uint8_t> blob;
        if (flags & 1) {
            if (size < 4) continue;
            uLongf len = (uLongf)raw[0] << 24 | raw[1] << 16 | raw[2] << 8 | raw[3];
            blob.resize(len);
            uLong src_len = size - 4;
            if (uncompress2(blob.data(), &len, raw + 4, &src_len) != Z_OK) {
                out.bad++;
                continue;
            }
        } else
            blob.assign(raw, raw + size);
        out.files++;
        Entry e;
        if (parse_entry(blob, d, e)) out.entries.push_back(std::move(e));
        else out.bad++;
    }
    return true;
}
}  // namespace latte_inv
