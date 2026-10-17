// Lean PM4 decoder (see pm4_decoder.h). Semantics follow the packets as GX2 builds them
// (runtime/src/gx2, from Cemu's GX2) and Cemu's LatteCommandProcessor.cpp for reference.
#if defined(BOTW_NATIVE_GPU)
#include "pm4_decoder.h"

#include <string.h>

#include <thread>

#include "../log.h"
#include "../mem.h"
#include "renderer.h"

namespace gpu_native {
namespace backend {
void gpu_wait_for_flip_counter_change();
void gpu_raise_event(uint32_t e);
}  // namespace backend

using namespace pm4;

Decoder::Decoder() : m_regs(new uint32_t[kRegCount]()), m_shadow(new uint32_t[kRegCount]()) {
    // SQ_VTX_SEMANTIC_* default to "unused"
    for (int i = 0; i < 32; i++) m_regs[0xA0E0 + i] = 0xFF;
}

void Decoder::execute(uint32_t addr, uint32_t ndw) { run(addr, ndw, 0); }

void Decoder::set_regs(uint32_t first, uint32_t src, uint32_t n) {
    for (uint32_t k = 0; k < n && first + k < kRegCount; k++) {
        uint32_t v = mem::r32(src + k * 4);
        m_regs[first + k] = v;
        // state shadowing (CONTEXT_CONTROL 0x80000077, GX2 context states): mirror into the shadow area
        if (m_ctx_control0 == 0x80000077 && m_shadow[first + k]) mem::w32(m_shadow[first + k], v);
    }
    // SQ_VTX_SEMANTIC_CLEAR resets the semantic table
    if (first <= 0xA238 && first + n > 0xA238)
        for (int i = 0; i < 32; i++) m_regs[0xA0E0 + i] = 0xFF;
}

void Decoder::load_regs(uint32_t base, uint32_t p, uint32_t n) {
    // LOAD_*: shadow address, (unused), then (register offset, count) pairs
    uint32_t shadow = mem::r32(p);
    for (uint32_t k = 2; k + 1 < n; k += 2) {
        uint32_t off = mem::r32(p + k * 4), cnt = mem::r32(p + (k + 1) * 4);
        for (uint32_t j = 0; j < cnt; j++) {
            uint32_t reg = base + off + j;
            if (reg >= kRegCount) break;
            uint32_t a = shadow + (off + j) * 4;
            m_shadow[reg] = a;
            m_regs[reg] = mem::r32(a);
        }
    }
}

static renderer::Surface surface_from(uint32_t p) {
    // HLE_COPY_SURFACE_NEW operand: addr swizzle format pitch height slice dim tilemode aa
    renderer::Surface s;
    s.addr = mem::r32(p), s.swizzle = mem::r32(p + 4), s.format = mem::r32(p + 8), s.pitch = mem::r32(p + 12);
    s.height = mem::r32(p + 16), s.slice = mem::r32(p + 20), s.dim = mem::r32(p + 24);
    s.tilemode = mem::r32(p + 28), s.aa = mem::r32(p + 32);
    s.width = s.pitch;
    return s;
}

void Decoder::run(uint32_t addr, uint32_t ndw, int depth) {
    if (depth > 8) return;
    uint32_t i = 0;
    while (i < ndw) {
        const uint32_t pa = addr + i * 4;
        const uint32_t h = mem::r32(pa);
        const uint32_t type = hdr_type(h);
        if (type == 2) {
            i++;
            continue;
        }
        if (type == 0) {
            uint32_t n = hdr_count(h);
            set_regs(h & 0xFFFF, pa + 4, n);
            i += 1 + n;
            continue;
        }
        if (type != 3) {
            LOG("gpu_native: bad PM4 header %08x at %08x", h, pa);
            return;
        }
        const uint32_t op = hdr_op(h), n = hdr_count(h), b = pa + 4;  // b: body address
        auto arg = [&](uint32_t k) { return mem::r32(b + k * 4); };
        switch (op) {
            case SET_CONFIG_REG: case SET_CONTEXT_REG: case SET_ALL_CONTEXTS: case SET_ALU_CONST:
            case SET_BOOL_CONST: case SET_LOOP_CONST: case SET_RESOURCE: case SET_SAMPLER: case SET_CTL_CONST:
                set_regs(base_for(op) + arg(0), b + 4, n - 1);
                break;
            case LOAD_CONFIG_REG: case LOAD_CONTEXT_REG: case LOAD_ALU_CONST: case LOAD_LOOP_CONST:
            case LOAD_RESOURCE: case LOAD_SAMPLER:
                load_regs(base_for(op), b, n);
                break;
            case CONTEXT_CONTROL: m_ctx_control0 = arg(0); break;
            case INDEX_TYPE: m_regs[VGT_DMA_INDEX_TYPE] = arg(0); break;
            case NUM_INSTANCES: m_instances = arg(0); break;
            case DRAW_INDEX_2:
            case DRAW_INDEX_AUTO: {
                if (m_skip_predicated) break;
                renderer::Draw d{};
                d.count = op == DRAW_INDEX_2 ? arg(3) : arg(0);
                d.index_addr = op == DRAW_INDEX_2 ? arg(1) : 0;
                d.index_type = m_regs[VGT_DMA_INDEX_TYPE];
                d.instances = m_instances ? m_instances : 1;
                d.regs = m_regs.get();
                renderer::draw(d);
                break;
            }
            case DRAW_INDEX_IMMD:
                // indices inline in the packet (GX2DrawIndexedImmediateEx); not used by BotW so far
                LOG("gpu_native: DRAW_INDEX_IMMD (%u indices) not implemented", arg(0));
                break;
            case INDIRECT_BUFFER_PRIV: run(arg(0), arg(2), depth + 1); break;
            case SET_PREDICATION: {
                // conditional render on an occlusion query: draw unless the query reports 0 samples
                uint32_t q = arg(0), flags = arg(1);
                m_predicated = ((flags >> 13) & 7) != 0;
                m_skip_predicated = false;
                if (m_predicated && q) {
                    uint64_t start, end;
                    memcpy(&start, mem::host(q), 8);
                    memcpy(&end, mem::host(q + 8), 8);
                    bool pass = (end - start) != 0;
                    bool pixels_must_pass = (flags >> 31) & 1;
                    m_skip_predicated = pixels_must_pass ? !pass : pass;
                }
                break;
            }
            case SURFACE_SYNC: renderer::surface_sync(arg(2) << 8, arg(1) << 8); break;
            case MEM_WRITE: {
                // address | flags, mode, data words (Cemu LatteCP_itMemWrite)
                uint32_t a = arg(0) & ~3u, mode = arg(1);
                if (!a) break;
                if (mode == 0x40000) mem::w32(a, arg(2));  // u32
                else if (mode == 0x00000) mem::w32(a, arg(2)), mem::w32(a + 4, arg(3));  // u64 big-endian
                else if (mode == 0x20000) {  // u64 little-endian (occlusion query slots)
                    uint64_t v = ((uint64_t)arg(3) << 32) | arg(2);
                    memcpy(mem::host(a), &v, 8);
                }
                break;
            }
            case WAIT_REG_MEM: {
                // function | mem space, address, (hi), reference, mask, poll interval
                renderer::Scoped timer(renderer::T_WAIT_MEM);
                uint32_t fn = arg(0) & 7, a = arg(1) & ~3u, ref = arg(3), mask = arg(4);
                for (int spins = 0;; spins++) {
                    uint32_t v = mem::r32(a) & mask;
                    bool ok = fn == 0 ? true : fn == 1 ? v < ref : fn == 2 ? v <= ref : fn == 3 ? v == ref
                            : fn == 4 ? v != ref : fn == 5 ? v >= ref : fn == 6 ? v > ref : true;
                    if (ok) break;
                    if (spins > 100) std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                break;
            }
            case EVENT_WRITE_EOP: {
                // event, address lo, address hi | data select, data lo, data hi
                uint32_t a = arg(1);
                mem::w32(a, arg(4));
                mem::w32(a + 4, arg(3));
                break;
            }
            case HLE_SAMPLE_TIMER: mem::w64(arg(0), rt_timebase()); break;
            case HLE_BOTTOM_OF_PIPE_CB: {
                uint32_t a = arg(0);
                mem::w32(a, arg(1));
                mem::w32(a + 4, arg(2));
                backend::gpu_raise_event(1);  // TIMESTAMP_BOTTOM
                break;
            }
            case HLE_SPECIAL_STATE:
                m_regs[0xFFF0 + (arg(0) & 0xF)] = arg(1);  // kept in an unused register range for the renderer
                break;
            case HLE_BEGIN_OCCLUSION_QUERY: renderer::occlusion_begin(arg(0)); break;
            case HLE_END_OCCLUSION_QUERY: renderer::occlusion_end(arg(0)); break;
            case HLE_CLEAR_COLOR_DEPTH_STENCIL: {
                renderer::Surface c, d;
                c.addr = arg(1), c.format = arg(2), c.tilemode = arg(3), c.width = arg(4), c.height = arg(5);
                c.pitch = arg(6), c.slice = arg(7), c.num_slices = arg(8);
                d.addr = arg(9), d.format = arg(10), d.tilemode = arg(11), d.width = arg(12), d.height = arg(13);
                d.pitch = arg(14), d.slice = arg(15), d.num_slices = arg(16);
                float rgba[4] = {arg(17) / 255.0f, arg(18) / 255.0f, arg(19) / 255.0f, arg(20) / 255.0f};
                uint32_t dv = arg(21);
                float depth;
                memcpy(&depth, &dv, 4);
                renderer::clear(arg(0), c, d, rgba, depth, arg(22));
                break;
            }
            case HLE_COPY_SURFACE_NEW:
                renderer::copy_surface(surface_from(b + 16), surface_from(b + 52), arg(0), arg(1), arg(2), arg(3));
                break;
            case HLE_COPY_COLORBUFFER_TO_SCANBUFFER: {
                renderer::Surface c;
                c.addr = arg(0), c.width = arg(1), c.height = arg(2), c.pitch = arg(3), c.tilemode = arg(4);
                c.swizzle = arg(5), c.slice = arg(6), c.format = arg(7);
                renderer::scanout_copy(c, arg(8));
                break;
            }
            case HLE_REQUEST_SWAP_BUFFERS: break;  // flip requests are counted CPU-side (latte::add_flip_request)
            case HLE_TRIGGER_SCANBUFFER_SWAP: renderer::present(); break;
            case HLE_WAIT_FOR_FLIP: backend::gpu_wait_for_flip_counter_change(); break;
            case STRMOUT_BUFFER_UPDATE: case STRMOUT_BASE_UPDATE: break;  // streamout: not yet
            case EVENT_WRITE: case MEM_SEMAPHORE: case NOP: case HLE_SYNC_ASYNC_OPERATIONS: break;
            default: LOG("gpu_native: unhandled PM4 opcode %02x", op); break;
        }
        i += 1 + n;
    }
}
}  // namespace gpu_native
#endif  // BOTW_NATIVE_GPU
