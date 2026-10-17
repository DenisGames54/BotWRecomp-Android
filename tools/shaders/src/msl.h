// R600 program IR (r600.h) -> Metal Shading Language, ahead of time.
//
// Native shader ABI (what the runtime binds; docs/native-renderer.md):
//   buffer(0)          StageInfo: per-draw constants (native texture sizes, render-scale factors,
//                      attribute strides, alpha-test reference, window->clip transform)
//   buffer(1..)        assigned compactly per shader and listed in Shader::bindings:
//                      "cfile" (ALU constant registers), "ub<N>" (uniform block N, read straight
//                      from guest memory, little-endian words as the GPU sees them), "ab<N>"
//                      (attribute buffer N, big-endian guest data; fetch + byte swap in the shader)
//   texture(N)/sampler(N)  GX2 texture / sampler slot N of the stage
//   VS outputs / PS inputs are named by Latte semantic id ([[user(sNN)]]), so any VS links to any PS.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace msl {
enum class Stage { Vertex, Pixel, Geometry };

struct Input {
    Stage stage;
    const uint8_t* program;
    uint32_t program_size;
    const uint8_t* fetch = nullptr;  // vertex: fetch shader program
    uint32_t fetch_size = 0;
    const uint32_t* regs;  // register state captured with the shader (64K entries)
    std::string name;      // function name
};

struct Binding {
    std::string kind;  // "info", "cfile", "ub", "ab", "tex", "smp"
    uint32_t slot;     // GX2 slot (ub/ab/tex/smp)
    uint32_t index;    // Metal argument index
};

struct Output {
    bool ok = false;
    std::string error;
    std::string source;
    std::vector<Binding> bindings;
    std::vector<std::string> variant_regs;  // register state baked into this translation (variant key)
    std::vector<std::pair<uint32_t, uint32_t>> deps;  // the same as (register, value) pairs
    // resource usage, for binding at draw time
    uint32_t tex_mask = 0, sampler_mask = 0, ub_mask = 0, ab_mask = 0;
    bool uses_cfile = false;
    std::vector<uint32_t> vs_out_semantics, ps_in_semantics;
    uint32_t ps_color_mask = 0;
};

Output emit(const Input& in);
}  // namespace msl
