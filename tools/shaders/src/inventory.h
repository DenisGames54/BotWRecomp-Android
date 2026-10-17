// Shader inventory: reads Cemu's transferable shader cache (FileCache V3 container with
// zstd-dictionary-compressed register state and programs, see runtime/cemu/Cemu/FileCache and
// runtime/cemu/Cafe/HW/Latte/Core/LatteShaderCache.cpp). Read-only; the zstd dictionaries and the
// register serialization table are parsed straight out of the vendored Cemu sources.
#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace latte_inv {
enum class Stage : uint8_t { Vertex = 0, Geometry = 1, Pixel = 2 };

struct Entry {
    Stage stage;
    uint64_t base_hash = 0, aux_hash = 0;
    bool uses_gs = false;
    uint32_t vs_ring_params = 0;               // geometry entries
    std::shared_ptr<std::vector<uint32_t>> regs;  // full 64K register file (only serialized registers set)
    std::vector<uint8_t> fetch, program, gs_copy;  // little-endian program words, as the GPU reads them
};

struct Inventory {
    std::vector<Entry> entries;
    uint32_t files = 0, bad = 0;
};

// cemu_root: runtime/cemu (for the dictionaries); path: *_shaders.bin / *_mtlshaders.bin
bool load(const std::string& cemu_root, const std::string& path, Inventory& out, std::string& err);
}  // namespace latte_inv
