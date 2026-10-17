// R600 IR -> MSL (see msl.h). Translation model:
//  * GPRs are int4 bit patterns (as_type<> to float where an op needs it; free on Apple GPUs), so
//    integer, float and raw moves keep the exact bits the R600 would.
//  * An ALU instruction group reads its sources, then all results are committed together (R600
//    groups are parallel); PV/PS hold the previous group's vector/trans results.
//  * Predicate/exec-mask flow uses the per-lane active-mask stack scheme of Cemu's decompiler
//    (activeMask / activeMaskC arrays), which maps 1:1 onto structured MSL.
//  * State the shader depends on is read from the register snapshot that came with it (vertex
//    semantic table, export semantics, PS input interpolation, texture dimensions/formats, clip
//    space convention); each such register is listed in Output::variant_regs.
#include "msl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>

#include "r600.h"

namespace msl {
using namespace r600;

namespace {
constexpr uint32_t mmSQ_VTX_SEMANTIC_0 = 0xA0E0, mmSPI_VS_OUT_ID_0 = 0xA185, mmSPI_PS_INPUT_CNTL_0 = 0xA191,
                   mmSPI_PS_IN_CONTROL_0 = 0xA1B3, mmSPI_PS_IN_CONTROL_1 = 0xA1B4, mmPA_CL_VTE_CNTL = 0xA206,
                   mmPA_CL_CLIP_CNTL = 0xA204, mmCB_SHADER_MASK = 0xA08F, mmSX_ALPHA_TEST_CONTROL = 0xA104,
                   mmVGT_INSTANCE_STEP_RATE_0 = 0xA2A8;
constexpr uint32_t kTexBase[3] = {0xE460, 0xE000, 0xE930};  // VS, PS, GS texture resource words

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}
const char* kChan = "xyzw";

const char* kPrelude = R"MSL(#include <metal_stdlib>
using namespace metal;
#define F(x) as_type<float>(x)
#define F4(x) as_type<float4>(x)
#define I(x) as_type<int>(x)
#define I4(x) as_type<int4>(x)
// R600 MUL/MULADD/DOT4 (non-IEEE): 0 * anything = 0, also for inf/nan
static inline float mul_nz(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
static inline int f2i(float x) { return isnan(x) ? 0 : int(clamp(trunc(x), -2147483648.0f, 2147483520.0f)); }
static inline uint f2u(float x) { return isnan(x) ? 0u : uint(clamp(trunc(x), 0.0f, 4294967040.0f)); }
static inline float recip_clamped(float x) { float r = 1.0f / x; return isinf(r) ? copysign(FLT_MAX, r) : r; }
static inline float recip_ff(float x) { float r = 1.0f / x; return isinf(r) ? copysign(0.0f, r) : r; }
static inline float rsq_clamped(float x) { float r = rsqrt(x); return isinf(r) ? copysign(FLT_MAX, r) : r; }
static inline float rsq_ff(float x) { float r = rsqrt(x); return isinf(r) ? copysign(0.0f, r) : r; }
static inline float log_clamped(float x) { float r = log2(x); return isinf(r) ? copysign(FLT_MAX, r) : r; }
static inline int sat_bits(int v) { return I(saturate(F(v))); }
static inline uint bswap32(uint v) { return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24); }
static inline uint bswap16(uint v) { return ((v >> 8) & 0xffu) | ((v & 0xffu) << 8); }
// unaligned little-endian loads from guest memory
static inline uint ld8(device const uchar* p, uint o) { return uint(p[o]); }
static inline uint ld16(device const uchar* p, uint o) { return uint(p[o]) | (uint(p[o + 1]) << 8); }
static inline uint ld32(device const uchar* p, uint o) {
    return uint(p[o]) | (uint(p[o + 1]) << 8) | (uint(p[o + 2]) << 16) | (uint(p[o + 3]) << 24);
}
static inline float snorm(int v, int bits) { return max(float(v) / float((1 << (bits - 1)) - 1), -1.0f); }
static inline int sext(uint v, int bits) { return int(v << (32 - bits)) >> (32 - bits); }
// R600 CUBE: src0.zzxy / src1.yxzz -> (T, S, 2*major axis, face)
static inline int4 redc_cube(float4 a, float4 b) {
    float x = b.y, y = b.x, z = a.x;
    float ax = abs(x), ay = abs(y), az = abs(z);
    float3 stm; float face;
    if (az >= ax && az >= ay) { stm = float3(-y, z < 0.0f ? -x : x, z * 2.0f); face = z < 0.0f ? 5.0f : 4.0f; }
    else if (ay >= ax) { stm = float3(y < 0.0f ? -z : z, x, y * 2.0f); face = y < 0.0f ? 3.0f : 2.0f; }
    else { stm = float3(-y, x < 0.0f ? z : -z, x * 2.0f); face = x < 0.0f ? 1.0f : 0.0f; }
    return I4(float4(stm, face));
}
// inverse of the CUBE face projection: (s, t) in [1, 2] plus face -> direction
static inline float3 cube_dir(float2 st, float face) {
    float2 tc = (st - float2(1.5f)) * 2.0f;
    int f = int(face);
    if (f == 0) return float3(1.0f, -tc.y, -tc.x);
    if (f == 1) return float3(-1.0f, -tc.y, tc.x);
    if (f == 2) return float3(tc.x, 1.0f, tc.y);
    if (f == 3) return float3(tc.x, -1.0f, -tc.y);
    if (f == 4) return float3(tc.x, -tc.y, 1.0f);
    return float3(-tc.x, -tc.y, -1.0f);
}
struct StageInfo {
    float4 tex_size[18];      // native (guest) width, height, depth/layers, mip count per texture slot
    float4 tex_scale[18];     // host texture size / native size (render scale) per slot
    uint attrib_stride[16];   // vertex attribute buffer strides (bytes)
    uint4 misc;               // x base vertex, y base instance
    float4 frag_coord_scale;  // xy: native / host framebuffer scale
    float4 window_to_clip;    // xy scale, zw offset (vertex shaders that output window coordinates)
    float alpha_ref, point_size, pad0, pad1;
};
)MSL";

struct Emitter {
    const Input& in;
    Output& out;
    Program prog;
    std::string body;
    int indent = 1;
    bool rel_regs = false;
    int stage_idx = 0;  // 0 VS, 1 PS, 2 GS
    // resources
    std::map<uint32_t, std::string> tex_type;  // slot -> MSL type
    std::map<uint32_t, uint32_t> tex_dim;      // slot -> Latte dim
    std::set<uint32_t> tex_int;                // slots with integer formats
    std::set<uint32_t> attrib_buffers;         // VS
    uint32_t next_buffer = 1;
    std::map<uint32_t, uint32_t> ub_index;     // ub slot -> metal index
    std::map<uint32_t, uint32_t> ab_index;     // attribute buffer -> metal index
    int cfile_index = -1;
    std::set<uint32_t> variant;
    bool uses_cube_index = false;
    // VS outputs / PS inputs
    std::set<uint32_t> vs_out_sems;  // semantic ids written
    bool writes_psize = false;
    std::set<uint32_t> ps_colors;  // color targets written
    bool writes_depth = false;

    Emitter(const Input& i, Output& o) : in(i), out(o) {}

    uint32_t reg(uint32_t r) {
        variant.insert(r);
        return in.regs[r];
    }
    void line(const std::string& s) {
        body.append(indent * 4, ' ');
        body += s;
        body += '\n';
    }
    bool fail(const std::string& e) {
        if (out.error.empty()) out.error = e;
        return false;
    }

    // ---- registers ----
    std::string gpr(uint32_t g, bool rel = false, uint32_t index_mode = 0) {
        if (rel) return fmt("R[(%u + %s) & 127]", g, ar(index_mode).c_str());
        if (rel_regs) return fmt("R[%u]", g);
        return fmt("R%u", g);
    }
    std::string ar(uint32_t index_mode) {
        // INDEX_AR_X..W, INDEX_LOOP (aL), INDEX_GLOBAL
        if (index_mode < 4) return fmt("AR.%c", kChan[index_mode]);
        if (index_mode == 4) return "aL";
        return "0";
    }

    // source operand as int bits
    std::string src(const CfInst& cf, const AluGroup& g, const AluInst& a, int k) {
        const AluSrc& s = a.src[k];
        std::string v;
        uint32_t sel = s.sel;
        if (sel < 128) v = gpr(sel, s.rel, a.index_mode) + "." + kChan[s.chan];
        else if (sel < 192) {
            int bank = sel < SRC_KCACHE1 ? 0 : 1;
            uint32_t idx = cf.kc_addr[bank] + (sel - (bank ? SRC_KCACHE1 : SRC_KCACHE0));
            uint32_t slot = cf.kc_bank[bank];
            std::string ix = s.rel ? fmt("%u + %s", idx, ar(a.index_mode).c_str()) : fmt("%u", idx);
            v = fmt("ub%u[%s].%c", slot, ix.c_str(), kChan[s.chan]);
        } else if (sel >= SRC_CFILE) {
            std::string ix = s.rel ? fmt("(%u + %s) & 255", sel - SRC_CFILE, ar(a.index_mode).c_str())
                                   : fmt("%u", sel - SRC_CFILE);
            v = fmt("cfile[%s].%c", ix.c_str(), kChan[s.chan]);
        } else
            switch (sel) {
                case SRC_0: v = "0"; break;
                case SRC_1: v = "0x3f800000"; break;
                case SRC_1_INT: v = "1"; break;
                case SRC_M_1_INT: v = "-1"; break;
                case SRC_0_5: v = "0x3f000000"; break;
                case SRC_LITERAL: v = fmt("int(0x%08xu)", g.literal[s.chan]); break;
                case SRC_PV: v = fmt("PV.%c", kChan[s.chan]); break;
                case SRC_PS: v = "PS"; break;
                default:
                    fail(fmt("unsupported ALU source select %u", sel));
                    v = "0";
            }
        if (s.abs) v = "(" + v + " & 0x7fffffff)";
        if (s.neg) v = "(" + v + " ^ int(0x80000000u))";
        return v;
    }
    std::string fsrc(const CfInst& cf, const AluGroup& g, const AluInst& a, int k) {
        return "F(" + src(cf, g, a, k) + ")";
    }
    static bool is_plain_const(const AluSrc& s) {
        return s.sel == SRC_LITERAL || s.sel == SRC_1 || s.sel == SRC_0_5;  // finite, non-zero by construction
    }

    // ---- ALU ----
    // expression producing the instruction result (int bits); "" for no result. float_result tells
    // whether omod/clamp apply.
    std::string alu_expr(const CfInst& cf, const AluGroup& g, const AluInst& a, bool& float_result, int clause_kind) {
        auto S = [&](int k) { return src(cf, g, a, k); };
        auto FS = [&](int k) { return fsrc(cf, g, a, k); };
        auto U = [&](int k) { return "uint(" + S(k) + ")"; };
        float_result = true;
        if (a.op3) {
            bool plain = is_plain_const(a.src[0]) || is_plain_const(a.src[1]);
            std::string mul = plain ? "(" + FS(0) + " * " + FS(1) + ")" : "mul_nz(" + FS(0) + ", " + FS(1) + ")";
            switch (a.op) {
                case MULADD: return "I(" + mul + " + " + FS(2) + ")";
                case MULADD_M2: return "I((" + mul + " + " + FS(2) + ") * 2.0f)";
                case MULADD_M4: return "I((" + mul + " + " + FS(2) + ") * 4.0f)";
                case MULADD_D2: return "I((" + mul + " + " + FS(2) + ") * 0.5f)";
                case MULADD_IEEE: return "I(fma(" + FS(0) + ", " + FS(1) + ", " + FS(2) + "))";
                case MULADD_IEEE_M2: return "I(fma(" + FS(0) + ", " + FS(1) + ", " + FS(2) + ") * 2.0f)";
                case MULADD_IEEE_M4: return "I(fma(" + FS(0) + ", " + FS(1) + ", " + FS(2) + ") * 4.0f)";
                case MULADD_IEEE_D2: return "I(fma(" + FS(0) + ", " + FS(1) + ", " + FS(2) + ") * 0.5f)";
                case CNDE: float_result = false; return "(" + FS(0) + " == 0.0f ? " + S(1) + " : " + S(2) + ")";
                case CNDGT: float_result = false; return "(" + FS(0) + " > 0.0f ? " + S(1) + " : " + S(2) + ")";
                case CNDGE: float_result = false; return "(" + FS(0) + " >= 0.0f ? " + S(1) + " : " + S(2) + ")";
                case CNDE_INT: float_result = false; return "(" + S(0) + " == 0 ? " + S(1) + " : " + S(2) + ")";
                case CNDGT_INT: float_result = false; return "(" + S(0) + " > 0 ? " + S(1) + " : " + S(2) + ")";
                case CNDGE_INT: float_result = false; return "(" + S(0) + " >= 0 ? " + S(1) + " : " + S(2) + ")";
                default: fail(fmt("unsupported ALU OP3 0x%x", a.op)); return "0";
            }
        }
        auto cmpf = [&](const char* o) { return "(" + FS(0) + " " + o + " " + FS(1) + ")"; };
        auto cmpi = [&](const char* o) { return "(" + S(0) + " " + o + " " + S(1) + ")"; };
        auto cmpu = [&](const char* o) { return "(" + U(0) + " " + o + " " + U(1) + ")"; };
        auto pred = [&](const std::string& cond) {
            // predicate result drives the clause's exec mask (PUSH_BEFORE) or loop break (ALU_BREAK)
            line("pred = " + cond + ";");
            if (a.update_exec || a.update_pred) {
                if (clause_kind == CF_ALU_PUSH_BEFORE) {
                    line(fmt("am[%d] = pred;", cf.depth));
                    line(fmt("amc[%d] = pred && amc[%d];", cf.depth + 1, cf.depth));
                } else if (clause_kind == CF_ALU_BREAK) {
                    line("alu_break = alu_break || !pred;");
                } else if (clause_kind == CF_ALU_CONTINUE) {
                    line("alu_continue = alu_continue || !pred;");
                }
            }
            return std::string("(pred ? 0 : 0x3f800000)");
        };
        auto kill = [&](const std::string& cond) {
            if (stage_idx == 1) line("if (" + cond + ") discard_fragment();");
            return std::string("(" + cond + " ? 0x3f800000 : 0)");
        };
        switch (a.op) {
            case ADD: return "I(" + FS(0) + " + " + FS(1) + ")";
            case MUL:
                if (is_plain_const(a.src[0]) || is_plain_const(a.src[1])) return "I(" + FS(0) + " * " + FS(1) + ")";
                return "I(mul_nz(" + FS(0) + ", " + FS(1) + "))";
            case MUL_IEEE: return "I(" + FS(0) + " * " + FS(1) + ")";
            case MAX: case MAX_DX10: return "I(fmax(" + FS(0) + ", " + FS(1) + "))";
            case MIN: case MIN_DX10: return "I(fmin(" + FS(0) + ", " + FS(1) + "))";
            case SETE: return "(" + cmpf("==") + " ? 0x3f800000 : 0)";
            case SETGT: return "(" + cmpf(">") + " ? 0x3f800000 : 0)";
            case SETGE: return "(" + cmpf(">=") + " ? 0x3f800000 : 0)";
            case SETNE: return "(" + cmpf("!=") + " ? 0x3f800000 : 0)";
            case SETE_DX10: float_result = false; return "(" + cmpf("==") + " ? -1 : 0)";
            case SETGT_DX10: float_result = false; return "(" + cmpf(">") + " ? -1 : 0)";
            case SETGE_DX10: float_result = false; return "(" + cmpf(">=") + " ? -1 : 0)";
            case SETNE_DX10: float_result = false; return "(" + cmpf("!=") + " ? -1 : 0)";
            case FRACT: return "I(fract(" + FS(0) + "))";
            case TRUNC: return "I(trunc(" + FS(0) + "))";
            case CEIL: return "I(ceil(" + FS(0) + "))";
            case RNDNE: return "I(rint(" + FS(0) + "))";
            case FLOOR: return "I(floor(" + FS(0) + "))";
            case MOVA: case MOVA_FLOOR: {
                float_result = false;
                std::string f = a.op == MOVA ? "floor(" + FS(0) + " + 0.5f)" : "floor(" + FS(0) + ")";
                line(fmt("{ float m = %s; AR.%c = (m >= -256.0f && m <= 255.0f) ? int(m) : -256; }", f.c_str(),
                         kChan[a.dst_chan]));
                return "I(" + f + ")";
            }
            case MOVA_INT: {
                float_result = false;
                line(fmt("{ int m = %s; AR.%c = (m >= -256 && m <= 255) ? m : -256; }", S(0).c_str(), kChan[a.dst_chan]));
                return S(0);
            }
            case MOV:
                float_result = a.src[0].abs || a.src[0].neg;  // modifiers make it a float move
                return S(0);
            case NOP: return "";
            case PRED_SETE: return pred(cmpf("=="));
            case PRED_SETGT: return pred(cmpf(">"));
            case PRED_SETGE: return pred(cmpf(">="));
            case PRED_SETNE: return pred(cmpf("!="));
            case PRED_SETE_INT: float_result = false; return pred(cmpi("=="));
            case PRED_SETGT_INT: float_result = false; return pred(cmpi(">"));
            case PRED_SETGE_INT: float_result = false; return pred(cmpi(">="));
            case PRED_SETNE_INT: float_result = false; return pred(cmpi("!="));
            case PRED_SETGT_UINT: float_result = false; return pred(cmpu(">"));
            case PRED_SETGE_UINT: float_result = false; return pred(cmpu(">="));
            case KILLE: return kill(cmpf("=="));
            case KILLGT: return kill(cmpf(">"));
            case KILLGE: return kill(cmpf(">="));
            case KILLNE: return kill(cmpf("!="));
            case KILLE_INT: float_result = false; return kill(cmpi("=="));
            case KILLGT_INT: float_result = false; return kill(cmpi(">"));
            case KILLGE_INT: float_result = false; return kill(cmpi(">="));
            case KILLNE_INT: float_result = false; return kill(cmpi("!="));
            case KILLGT_UINT: float_result = false; return kill(cmpu(">"));
            case KILLGE_UINT: float_result = false; return kill(cmpu(">="));
            case AND_INT: float_result = false; return "(" + S(0) + " & " + S(1) + ")";
            case OR_INT: float_result = false; return "(" + S(0) + " | " + S(1) + ")";
            case XOR_INT: float_result = false; return "(" + S(0) + " ^ " + S(1) + ")";
            case NOT_INT: float_result = false; return "(~" + S(0) + ")";
            case ADD_INT: float_result = false; return "int(" + U(0) + " + " + U(1) + ")";
            case SUB_INT: float_result = false; return "int(" + U(0) + " - " + U(1) + ")";
            case MAX_INT: float_result = false; return "max(" + S(0) + ", " + S(1) + ")";
            case MIN_INT: float_result = false; return "min(" + S(0) + ", " + S(1) + ")";
            case MAX_UINT: float_result = false; return "int(max(" + U(0) + ", " + U(1) + "))";
            case MIN_UINT: float_result = false; return "int(min(" + U(0) + ", " + U(1) + "))";
            case SETE_INT: float_result = false; return "(" + cmpi("==") + " ? -1 : 0)";
            case SETGT_INT: float_result = false; return "(" + cmpi(">") + " ? -1 : 0)";
            case SETGE_INT: float_result = false; return "(" + cmpi(">=") + " ? -1 : 0)";
            case SETNE_INT: float_result = false; return "(" + cmpi("!=") + " ? -1 : 0)";
            case SETGT_UINT: float_result = false; return "(" + cmpu(">") + " ? -1 : 0)";
            case SETGE_UINT: float_result = false; return "(" + cmpu(">=") + " ? -1 : 0)";
            case EXP_IEEE: return "I(exp2(" + FS(0) + "))";
            case LOG_CLAMPED: return "I(log_clamped(" + FS(0) + "))";
            case LOG_IEEE: return "I(log2(" + FS(0) + "))";
            case RECIP_CLAMPED: return "I(recip_clamped(" + FS(0) + "))";
            case RECIP_FF: return "I(recip_ff(" + FS(0) + "))";
            case RECIP_IEEE: return "I(1.0f / " + FS(0) + ")";
            case RECIPSQRT_CLAMPED: return "I(rsq_clamped(" + FS(0) + "))";
            case RECIPSQRT_FF: return "I(rsq_ff(" + FS(0) + "))";
            case RECIPSQRT_IEEE: return "I(rsqrt(" + FS(0) + "))";
            case SQRT_IEEE: return "I(sqrt(" + FS(0) + "))";
            case FLT_TO_INT: float_result = false; return "f2i(" + FS(0) + ")";
            case FLT_TO_UINT: float_result = false; return "int(f2u(" + FS(0) + "))";
            case INT_TO_FLT: return "I(float(" + S(0) + "))";
            case UINT_TO_FLT: return "I(float(" + U(0) + "))";
            case SIN: return "I(sin(" + FS(0) + " * 6.2831853f))";
            case COS: return "I(cos(" + FS(0) + " * 6.2831853f))";
            case ASHR_INT: float_result = false; return "(" + S(0) + " >> (" + S(1) + " & 31))";
            case LSHR_INT: float_result = false; return "int(" + U(0) + " >> (" + U(1) + " & 31u))";
            case LSHL_INT: float_result = false; return "int(" + U(0) + " << (" + U(1) + " & 31u))";
            case MULLO_INT: float_result = false; return "int(" + U(0) + " * " + U(1) + ")";
            case MULLO_UINT: float_result = false; return "int(" + U(0) + " * " + U(1) + ")";
            case MULHI_INT: float_result = false; return "mulhi(" + S(0) + ", " + S(1) + ")";
            case MULHI_UINT: float_result = false; return "int(mulhi(" + U(0) + ", " + U(1) + "))";
            case RECIP_UINT: float_result = false; return "int(" + U(0) + " == 0u ? 0xffffffffu : 0xffffffffu / " + U(0) + ")";
            default:
                fail(fmt("unsupported ALU OP2 %s (0x%x)", op2_name(a.op) ? op2_name(a.op) : "?", a.op));
                return "0";
        }
    }

    void emit_group(const CfInst& cf, const AluGroup& g, int clause_kind) {
        // 1) compute every slot into t<unit>
        bool have[5] = {};
        std::string dst_expr[5];
        const AluInst* inst[5] = {};
        for (size_t k = 0; k < g.insts.size(); k++) {
            const AluInst& a = g.insts[k];
            if (is_reduction(a)) {
                // four consecutive vector slots x,y,z,w form one reduction
                if (k + 4 > g.insts.size()) {
                    fail("truncated reduction group");
                    return;
                }
                const AluInst* r[4] = {&g.insts[k], &g.insts[k + 1], &g.insts[k + 2], &g.insts[k + 3]};
                std::string s0[4], s1[4];
                for (int j = 0; j < 4; j++) s0[j] = fsrc(cf, g, *r[j], 0), s1[j] = fsrc(cf, g, *r[j], 1);
                std::string res;
                if (a.op == DOT4) {
                    res = "I(mul_nz(" + s0[0] + ", " + s1[0] + ") + mul_nz(" + s0[1] + ", " + s1[1] + ") + mul_nz(" + s0[2] +
                          ", " + s1[2] + ") + mul_nz(" + s0[3] + ", " + s1[3] + "))";
                } else if (a.op == DOT4_IEEE) {
                    res = "I(dot(float4(" + s0[0] + ", " + s0[1] + ", " + s0[2] + ", " + s0[3] + "), float4(" + s1[0] + ", " +
                          s1[1] + ", " + s1[2] + ", " + s1[3] + ")))";
                } else if (a.op == MAX4) {
                    res = "I(fmax(fmax(" + s0[0] + ", " + s0[1] + "), fmax(" + s0[2] + ", " + s0[3] + ")))";
                }
                if (a.op == CUBE) {
                    line("int4 cube = redc_cube(float4(" + s0[0] + ", " + s0[1] + ", " + s0[2] + ", " + s0[3] + "), float4(" +
                         s1[0] + ", " + s1[1] + ", " + s1[2] + ", " + s1[3] + "));");
                }
                for (int j = 0; j < 4; j++) {
                    const AluInst& rj = *r[j];
                    uint32_t u = rj.unit;
                    if (u > 3 || have[u]) {
                        fail("bad reduction slot assignment");
                        return;
                    }
                    std::string v = a.op == CUBE ? fmt("cube.%c", kChan[j]) : res;
                    if (rj.omod) v = omod(v, rj.omod);
                    if (rj.clamp) v = "sat_bits(" + v + ")";
                    line(fmt("int t%c = %s;", "xyzwt"[u], v.c_str()));
                    have[u] = true;
                    inst[u] = r[j];
                }
                k += 3;
                continue;
            }
            bool float_result = true;
            std::string e = alu_expr(cf, g, a, float_result, clause_kind);
            if (e.empty()) continue;
            if (float_result && a.omod) e = omod(e, a.omod);
            if (a.clamp && float_result) e = "sat_bits(" + e + ")";
            if (have[a.unit]) {
                fail("two instructions in one ALU unit");
                return;
            }
            line(fmt("int t%c = %s;", "xyzwt"[a.unit], e.c_str()));
            have[a.unit] = true;
            inst[a.unit] = &a;
        }
        // 2) commit GPR writes, then PV/PS
        for (int u = 0; u < 5; u++) {
            const AluInst* a = inst[u];
            if (!a || !(a->write || a->op3)) continue;
            line(fmt("%s.%c = t%c;", gpr(a->dst_gpr, a->dst_rel, a->index_mode).c_str(), kChan[a->dst_chan], "xyzwt"[u]));
        }
        std::string pv;
        for (int u = 0; u < 4; u++)
            if (have[u]) pv += fmt("PV.%c = t%c; ", kChan[u], kChan[u]);
        if (have[4]) pv += "PS = tt;";
        if (!pv.empty()) line(pv);
    }

    std::string omod(const std::string& e, int m) {
        if (m == 1) return "I(F(" + e + ") * 2.0f)";
        if (m == 2) return "I(F(" + e + ") * 4.0f)";
        if (m == 3) return "I(F(" + e + ") * 0.5f)";
        return e;
    }

    void emit_alu_clause(const CfInst& cf) {
        for (auto& g : cf.groups) {
            line("{");
            indent++;
            emit_group(cf, g, cf.op);
            indent--;
            line("}");
        }
    }

    // ---- TEX ----
    std::string tex_src(const TexInst& t, int c, bool as_float) {
        uint32_t sel = t.src_sel[c];
        std::string g = gpr(t.src_gpr, t.src_rel, 0);
        if (sel < 4) return as_float ? fmt("F(%s.%c)", g.c_str(), kChan[sel]) : fmt("%s.%c", g.c_str(), kChan[sel]);
        if (sel == 4) return as_float ? "0.0f" : "0";
        if (sel == 5) return as_float ? "1.0f" : "1";
        return as_float ? "0.0f" : "0";
    }

    void write_tex_result(const TexInst& t, const std::string& vec4_int_expr) {
        // vec4_int_expr: int4 of results; dst_sel picks components / constants
        std::string g = gpr(t.dst_gpr, t.dst_rel, 0);
        line("{");
        indent++;
        line("int4 r = " + vec4_int_expr + ";");
        for (int c = 0; c < 4; c++) {
            uint32_t s = t.dst_sel[c];
            if (s == 7) continue;
            std::string v = s < 4 ? fmt("r.%c", kChan[s]) : s == 4 ? "0" : s == 5 ? "0x3f800000" : "0";
            line(fmt("%s.%c = %s;", g.c_str(), kChan[c], v.c_str()));
        }
        indent--;
        line("}");
    }

    bool emit_tex(const TexInst& t) {
        if (t.op == TEX_VFETCH) return emit_vfetch(t);
        if (t.op == TEX_MEM) return fail("TEX MEM read (scatter buffer) not supported");
        if (t.op == TEX_SET_GRADIENTS_H || t.op == TEX_SET_GRADIENTS_V) {
            line(fmt("grad%c = float4(%s, %s, %s, %s);", t.op == TEX_SET_GRADIENTS_H ? 'H' : 'V', tex_src(t, 0, true).c_str(),
                     tex_src(t, 1, true).c_str(), tex_src(t, 2, true).c_str(), tex_src(t, 3, true).c_str()));
            return true;
        }
        if (t.op == TEX_GET_GRADIENTS_H || t.op == TEX_GET_GRADIENTS_V) {
            if (stage_idx != 1) return fail("derivatives outside a pixel shader");
            std::string v = fmt("float4(%s, %s, %s, %s)", tex_src(t, 0, true).c_str(), tex_src(t, 1, true).c_str(),
                                tex_src(t, 2, true).c_str(), tex_src(t, 3, true).c_str());
            write_tex_result(t, fmt("I4(%s(%s))", t.op == TEX_GET_GRADIENTS_H ? "dfdx" : "dfdy", v.c_str()));
            return true;
        }
        if (t.op == TEX_SET_CUBEMAP_INDEX) {
            line(fmt("cube_index%u = %s;", t.resource, tex_src(t, 0, true).c_str()));
            uses_cube_index = true;
            return true;
        }
        uint32_t slot = t.resource;
        if (!tex_type.count(slot)) return fail(fmt("texture slot %u without a declaration", slot));
        uint32_t dim = tex_dim[slot];
        bool is_int = tex_int.count(slot);
        bool compare = t.op >= TEX_SAMPLE_C;
        std::string tx = fmt("tex%u", slot), sm = fmt("smp%u", t.sampler);
        auto c = [&](int k) { return tex_src(t, k, true); };
        if (t.op == TEX_GET_RESINFO) {
            // native size (render scale must not leak into the guest's view of texture sizes)
            write_tex_result(t, fmt("int4(info.tex_size[%u])", slot));
            return true;
        }
        if (t.op == TEX_GET_NUM_SAMPLES) {
            write_tex_result(t, "int4(1)");
            return true;
        }
        if (t.op == TEX_GET_LOD) {
            if (stage_idx != 1 || dim == 0 || dim == 4) {
                write_tex_result(t, "int4(0)");
                return true;
            }
            std::string coord = dim == 2 ? fmt("float3(%s, %s, %s)", c(0).c_str(), c(1).c_str(), c(2).c_str())
                                         : fmt("float2(%s, %s)", c(0).c_str(), c(1).c_str());
            if (dim == 3) return fail("GET_LOD on a cube map");
            write_tex_result(t, fmt("I4(float4(%s.calculate_clamped_lod(%s, %s), %s.calculate_unclamped_lod(%s, %s), 0.0f, 0.0f))",
                                    tx.c_str(), sm.c_str(), coord.c_str(), tx.c_str(), sm.c_str(), coord.c_str()));
            return true;
        }
        // coordinates
        bool unnorm = !(t.coord_norm[0] && t.coord_norm[1]);
        std::string sx = c(0), sy = c(1), sz = c(2);
        if (unnorm && t.op != TEX_LD) {
            // unnormalized (texel) coordinates are in native texels
            if (!t.coord_norm[0]) sx = "(" + sx + fmt(" / info.tex_size[%u].x)", slot);
            if (!t.coord_norm[1]) sy = "(" + sy + fmt(" / info.tex_size[%u].y)", slot);
        }
        std::string coord, extra;
        switch (dim) {
            case 0: coord = sx; break;                                            // 1D
            case 1: case 6: coord = "float2(" + sx + ", " + sy + ")"; break;      // 2D, 2D MSAA
            case 2: coord = "float3(" + sx + ", " + sy + ", " + sz + ")"; break;  // 3D
            case 3: {                                                             // cube (array)
                coord = "cube_dir(float2(" + sx + ", " + sy + "), " + sz + ")";
                extra = fmt(", uint(cube_index%u)", slot);
                uses_cube_index = true;
                break;
            }
            case 4: coord = sx; extra = ", uint(rint(" + sy + "))"; break;                              // 1D array
            case 5: case 7: coord = "float2(" + sx + ", " + sy + ")"; extra = ", uint(rint(" + sz + "))"; break;  // 2D array
            default: return fail(fmt("texture dim %u", dim));
        }
        std::string lod;
        bool vs = stage_idx != 1;
        uint8_t op = t.op;
        if (vs && (op == TEX_SAMPLE || op == TEX_SAMPLE_C || op == TEX_FETCH4)) op = op == TEX_SAMPLE_C ? TEX_SAMPLE_C_LZ : op == TEX_SAMPLE ? TEX_SAMPLE_LZ : op;
        bool has_lod = dim != 0 && dim != 4;  // 1D textures have no mips in Metal
        switch (op) {
            case TEX_SAMPLE_L: case TEX_SAMPLE_C_L: if (has_lod) lod = ", level(" + c(3) + ")"; break;
            case TEX_SAMPLE_LB: case TEX_SAMPLE_C_LB: if (has_lod) lod = ", bias(" + c(3) + ")"; break;
            case TEX_SAMPLE_LZ: case TEX_SAMPLE_C_LZ: if (has_lod) lod = ", level(0.0f)"; break;
            case TEX_SAMPLE_G: case TEX_SAMPLE_C_G:
                if (dim == 1 || dim == 5) lod = ", gradient2d(gradH.xy, gradV.xy)";
                else if (dim == 2) lod = ", gradient3d(gradH.xyz, gradV.xyz)";
                else if (dim == 3) lod = ", gradientcube(gradH.xyz, gradV.xyz)";
                break;
            case TEX_SAMPLE_G_L: case TEX_SAMPLE_C_G_L: if (has_lod) lod = ", level(" + c(3) + ")"; break;
            case TEX_SAMPLE_G_LZ: case TEX_SAMPLE_C_G_LZ: if (has_lod) lod = ", level(0.0f)"; break;
            default: break;
        }
        std::string offs;
        if ((t.offset[0] || t.offset[1] || t.offset[2]) && (dim == 1 || dim == 5 || dim == 2)) {
            // R600 offsets are in half texels
            if (dim == 2) offs = fmt(", int3(%d, %d, %d)", t.offset[0] / 2, t.offset[1] / 2, t.offset[2] / 2);
            else offs = fmt(", int2(%d, %d)", t.offset[0] / 2, t.offset[1] / 2);
        }
        if (op == TEX_LD || (op == TEX_SAMPLE && unnorm && t.coord_norm[2] == false && t.coord_norm[3] == false)) {
            // texel fetch: integer coordinates in native texels, scaled to the host texture
            std::string ix, iy;
            if (op == TEX_LD) {  // integer texel coordinates
                ix = fmt("uint(float(%s) * info.tex_scale[%u].x)", tex_src(t, 0, false).c_str(), slot);
                iy = fmt("uint(float(%s) * info.tex_scale[%u].y)", tex_src(t, 1, false).c_str(), slot);
            } else {  // unnormalized float coordinates
                ix = fmt("uint(%s * info.tex_scale[%u].x)", c(0).c_str(), slot);
                iy = fmt("uint(%s * info.tex_scale[%u].y)", c(1).c_str(), slot);
            }
            std::string lv = op == TEX_LD ? tex_src(t, 3, false) : "0";
            std::string rd;
            switch (dim) {
                case 0: rd = fmt("%s.read(%s)", tx.c_str(), ix.c_str()); break;
                case 1: rd = fmt("%s.read(uint2(%s, %s), uint(%s))", tx.c_str(), ix.c_str(), iy.c_str(), lv.c_str()); break;
                case 6: rd = fmt("%s.read(uint2(%s, %s), 0)", tx.c_str(), ix.c_str(), iy.c_str()); break;
                case 5: rd = fmt("%s.read(uint2(%s, %s), uint(%s), uint(%s))", tx.c_str(), ix.c_str(), iy.c_str(),
                                 tex_src(t, 2, false).c_str(), lv.c_str()); break;
                case 2: rd = fmt("%s.read(uint3(%s, %s, uint(%s)), uint(%s))", tx.c_str(), ix.c_str(), iy.c_str(),
                                 tex_src(t, 2, false).c_str(), lv.c_str()); break;
                default: return fail(fmt("texel fetch from dim %u", dim));
            }
            write_tex_result(t, is_int ? "int4(" + rd + ")" : "I4(" + rd + ")");
            return true;
        }
        if (dim == 6 || dim == 7) return fail("filtered sample from an MSAA texture");
        if (is_int) return fail("filtered sample from an integer texture");
        std::string call;
        if (compare) {
            // depth compare; reference is .w (or .z for SAMPLE_C_L on 2D, as Cemu found)
            std::string ref = (op == TEX_SAMPLE_C_L && dim == 1) ? c(2) : c(3);
            if (dim == 3) return fail("compare sampling from a cube map");
            call = fmt("float4(%s.sample_compare(%s, %s%s, %s%s%s))", tx.c_str(), sm.c_str(), coord.c_str(), extra.c_str(),
                       ref.c_str(), lod.c_str(), offs.c_str());
        } else if (op == TEX_FETCH4) {
            if (dim != 1 && dim != 5) return fail("FETCH4 on a non-2D texture");
            // gather returns (-,+),(+,+),(+,-),(-,-); FETCH4 order is yzxw of that
            call = fmt("%s.gather(%s, %s%s%s).yzxw", tx.c_str(), sm.c_str(), coord.c_str(), extra.c_str(), offs.c_str());
        } else {
            call = fmt("%s.sample(%s, %s%s%s%s)", tx.c_str(), sm.c_str(), coord.c_str(), extra.c_str(), lod.c_str(), offs.c_str());
        }
        write_tex_result(t, is_int ? "int4(" + call + ")" : "I4(" + call + ")");
        return true;
    }

    // vertex fetch from an attribute buffer (fetch shader or TEX-clause VFETCH)
    std::string fetch_expr(uint32_t buffer, const std::string& index_expr, uint32_t offset, uint32_t fmt_code,
                           uint32_t num_format, bool is_signed, uint32_t endian, uint8_t* ds_out_float_one,
                           std::string& err) {
        // element layout
        struct L { int comps, bits; bool flt, packed; };
        L l{};
        switch (fmt_code) {
            case 0x01: l = {1, 8, false, false}; break;
            case 0x07: l = {2, 8, false, false}; break;
            case 0x1A: l = {4, 8, false, false}; break;
            case 0x05: l = {1, 16, false, false}; break;
            case 0x0F: l = {2, 16, false, false}; break;
            case 0x1F: l = {4, 16, false, false}; break;
            case 0x06: l = {1, 16, true, false}; break;
            case 0x10: l = {2, 16, true, false}; break;
            case 0x20: l = {4, 16, true, false}; break;
            case 0x0D: l = {1, 32, false, false}; break;
            case 0x1D: l = {2, 32, false, false}; break;
            case 0x2F: l = {3, 32, false, false}; break;
            case 0x22: l = {4, 32, false, false}; break;
            case 0x0E: l = {1, 32, true, false}; break;
            case 0x1E: l = {2, 32, true, false}; break;
            case 0x30: l = {3, 32, true, false}; break;
            case 0x23: l = {4, 32, true, false}; break;
            case 0x19: l = {4, 0, false, true}; break;  // 2_10_10_10 (x in the low bits)
            default:
                err = fmt("vertex format 0x%x", fmt_code);
                return "int4(0)";
        }
        std::string base = fmt("(%s) * info.attrib_stride[%u] + %uu", index_expr.c_str(), buffer, offset);
        std::string p = fmt("ab%u", buffer);
        std::string comp[4];
        if (l.packed) {
            std::string w = fmt("ld32(%s, %s)", p.c_str(), base.c_str());
            if (endian == 2 || endian == 1) w = "bswap32(" + w + ")";
            std::string raw[4] = {"((" + w + ") & 0x3ffu)", "(((" + w + ") >> 10) & 0x3ffu)", "(((" + w + ") >> 20) & 0x3ffu)",
                                  "((" + w + ") >> 30)"};
            int bits[4] = {10, 10, 10, 2};
            for (int k = 0; k < 4; k++) {
                if (num_format == 1) comp[k] = is_signed ? fmt("sext(%s, %d)", raw[k].c_str(), bits[k]) : "int(" + raw[k] + ")";
                else if (num_format == 0)
                    comp[k] = is_signed ? fmt("I(snorm(sext(%s, %d), %d))", raw[k].c_str(), bits[k], bits[k])
                                        : fmt("I(float(%s) / %d.0f)", raw[k].c_str(), (1 << bits[k]) - 1);
                else comp[k] = is_signed ? fmt("I(float(sext(%s, %d)))", raw[k].c_str(), bits[k]) : "I(float(" + raw[k] + "))";
            }
        } else {
            int bytes = l.bits / 8;
            for (int k = 0; k < l.comps; k++) {
                std::string o = fmt("%s + %du", base.c_str(), k * bytes);
                std::string raw;
                if (bytes == 1) raw = fmt("ld8(%s, %s)", p.c_str(), o.c_str());
                else if (bytes == 2) {
                    raw = fmt("ld16(%s, %s)", p.c_str(), o.c_str());
                    if (endian == 1) raw = "bswap16(" + raw + ")";
                    else if (endian == 2) {  // 8in32 on 16-bit components: pairs swap within the dword
                        raw = fmt("bswap16(ld16(%s, %s + %du - %du))", p.c_str(), base.c_str(), (k ^ 1) * 2, 0);
                    }
                } else {
                    raw = fmt("ld32(%s, %s)", p.c_str(), o.c_str());
                    if (endian == 2) raw = "bswap32(" + raw + ")";
                    else if (endian == 1) raw = "((" + raw + " >> 8 & 0x00ff00ffu) | (" + raw + " << 8 & 0xff00ff00u))";
                }
                if (l.flt) {
                    comp[k] = l.bits == 16 ? "I(float(as_type<half>(ushort(" + raw + "))))" : "int(" + raw + ")";
                } else if (num_format == 1) {
                    comp[k] = is_signed ? fmt("sext(%s, %d)", raw.c_str(), l.bits) : "int(" + raw + ")";
                } else if (num_format == 0) {
                    comp[k] = is_signed ? fmt("I(snorm(sext(%s, %d), %d))", raw.c_str(), l.bits, l.bits)
                                        : (l.bits == 32 ? "I(float(" + raw + ") / 4294967295.0f)"
                                                        : fmt("I(float(%s) / %d.0f)", raw.c_str(), (1 << l.bits) - 1));
                } else {
                    comp[k] = is_signed ? fmt("I(float(sext(%s, %d)))", raw.c_str(), l.bits) : "I(float(" + raw + "))";
                }
            }
            for (int k = l.comps; k < 4; k++) comp[k] = k == 3 ? (num_format == 1 && !l.flt ? "1" : "0x3f800000") : "0";
        }
        if (ds_out_float_one) *ds_out_float_one = !(num_format == 1 && !l.flt);
        return "int4(" + comp[0] + ", " + comp[1] + ", " + comp[2] + ", " + comp[3] + ")";
    }

    bool emit_vfetch(const TexInst& t) {
        if (t.resource >= 0x80 && t.resource < 0x90) {
            // uniform buffer read through the vertex cache (index in src.x, 16-byte elements)
            uint32_t slot = t.resource - 0x80;
            need_ub(slot);
            std::string ix = fmt("%s.%c", gpr(t.src_gpr, t.src_rel).c_str(), kChan[t.src_sel_x]);
            write_tex_result(t, fmt("ub%u[uint(%s) + %uu]", slot, ix.c_str(), t.vtx_offset / 16));
            return true;
        }
        if (stage_idx != 0) return fail(fmt("VFETCH from buffer id 0x%x outside a vertex shader", t.resource));
        if (t.resource < 0xA0 || t.resource >= 0xB0) return fail(fmt("VFETCH from buffer id 0x%x", t.resource));
        uint32_t b = t.resource - 0xA0;
        need_ab(b);
        std::string err;
        std::string ix = fmt("uint(%s.%c)", gpr(t.src_gpr, t.src_rel).c_str(), kChan[t.src_sel_x]);
        std::string e = fetch_expr(b, ix, t.vtx_offset, t.data_format, t.num_format, t.format_signed, t.endian, nullptr, err);
        if (!err.empty()) return fail(err);
        write_tex_result(t, e);
        return true;
    }

    void need_ub(uint32_t slot) {
        if (!ub_index.count(slot)) ub_index[slot] = 0;
    }
    void need_ab(uint32_t b) { attrib_buffers.insert(b); }

    // ---- exports ----
    std::string export_vec(const CfInst& cf, uint32_t burst, bool as_float) {
        std::string g = gpr(cf.exp_gpr + burst, cf.exp_rel, 0);
        std::string comp[4];
        for (int k = 0; k < 4; k++) {
            uint32_t s = cf.exp_sel[k];
            comp[k] = s < 4 ? fmt("%s.%c", g.c_str(), kChan[s]) : s == 5 ? "0x3f800000" : "0";
        }
        std::string v = "int4(" + comp[0] + ", " + comp[1] + ", " + comp[2] + ", " + comp[3] + ")";
        return as_float ? "F4(" + v + ")" : v;
    }

    bool emit_export(const CfInst& cf) {
        if (cf.op != CF_EXPORT && cf.op != CF_EXPORT_DONE) {
            if (cf.op == CF_MEM_STREAM0 || cf.op == CF_MEM_STREAM1 || cf.op == CF_MEM_STREAM2 || cf.op == CF_MEM_STREAM3) {
                streamout = true;
                line("// streamout write (MEM_STREAM) -> guest memory: not emitted in this pass");
                return true;
            }
            if (cf.op == CF_MEM_RING) return fail("MEM_RING write (geometry shader path)");
            return fail(fmt("export CF 0x%x", cf.op));
        }
        if (stage_idx == 0) {
            if (cf.exp_type == 1) {
                if (cf.exp_array_base == 60) {
                    line("{");
                    indent++;
                    line("float4 p = " + export_vec(cf, 0, true) + ";");
                    uint32_t vte = reg(mmPA_CL_VTE_CNTL);
                    if ((vte & 0x3F) != 0x3F) line("p.xy = p.xy * info.window_to_clip.xy + info.window_to_clip.zw;");
                    if (!((reg(mmPA_CL_CLIP_CNTL) >> 19) & 1)) line("p.z = (p.z + p.w) * 0.5f;  // GL clip space -> Metal");
                    line("out.pos = p;");
                    indent--;
                    line("}");
                } else if (cf.exp_array_base == 61) {
                    writes_psize = true;
                    line("out.psize = " + export_vec(cf, 0, true) + ".x;");
                } else
                    line(fmt("// export pos %u ignored (clip/cull distances)", cf.exp_array_base));
                return true;
            }
            if (cf.exp_type == 2) {
                for (uint32_t b = 0; b <= cf.exp_burst; b++) {
                    uint32_t idx = cf.exp_array_base + b;
                    if (idx >= 32) continue;
                    uint32_t sem = (reg(mmSPI_VS_OUT_ID_0 + idx / 4) >> (8 * (idx % 4))) & 0xFF;
                    if (sem == 0xFF) continue;
                    vs_out_sems.insert(sem);
                    std::string mask;
                    for (int k = 0; k < 4; k++)
                        if (cf.exp_sel[k] != 7) mask += kChan[k];
                    if (mask.empty()) continue;
                    std::string v = export_vec(cf, b, true);
                    if (mask.size() == 4) line(fmt("out.s%u = %s;", sem, v.c_str()));
                    else line(fmt("out.s%u.%s = %s.%s;", sem, mask.c_str(), v.c_str(), mask.c_str()));
                }
                return true;
            }
            return fail(fmt("vertex export type %u", cf.exp_type));
        }
        if (stage_idx == 1) {
            if (cf.exp_type == 0 && cf.exp_array_base < 8) {
                uint32_t mask = reg(mmCB_SHADER_MASK);
                for (uint32_t b = 0; b <= cf.exp_burst; b++) {
                    // export index -> color target: n-th target enabled in CB_SHADER_MASK
                    int target = -1, n = 0;
                    for (int m = 0; m < 8; m++) {
                        if (!((mask >> (m * 4)) & 0xF)) continue;
                        if (n == (int)(cf.exp_array_base + b)) {
                            target = m;
                            break;
                        }
                        n++;
                    }
                    if (target < 0) continue;
                    std::string v = export_vec(cf, b, true);
                    if (target == 0) {
                        uint32_t at = reg(mmSX_ALPHA_TEST_CONTROL);
                        if ((at >> 3) & 1) {
                            static const char* cmp[8] = {nullptr, "<", "==", "<=", ">", "!=", ">=", nullptr};
                            uint32_t func = at & 7;
                            if (func == 0) line("discard_fragment();");
                            else if (cmp[func]) line(fmt("if (!((%s).w %s info.alpha_ref)) discard_fragment();", v.c_str(), cmp[func]));
                        }
                    }
                    ps_colors.insert(target);
                    uint32_t nt = (reg(0xA028 + target) >> 12) & 7;  // CB_COLORn_INFO number type
                    if (nt == 4) line(fmt("out.c%d = as_type<uint4>(%s);", target, export_vec(cf, b, false).c_str()));
                    else if (nt == 5) line(fmt("out.c%d = %s;", target, export_vec(cf, b, false).c_str()));
                    else line(fmt("out.c%d = %s;", target, v.c_str()));
                }
                return true;
            }
            if (cf.exp_type == 0 && cf.exp_array_base == 61) {
                writes_depth = true;
                line("out.depth = " + export_vec(cf, 0, true) + ".x;");
                return true;
            }
            return fail(fmt("pixel export type %u base %u", cf.exp_type, cf.exp_array_base));
        }
        return fail("geometry shader exports");
    }
    bool streamout = false;

    // ---- control flow ----
    std::string amc(int d) { return fmt("amc[%d]", d); }
    std::string am(int d) { return fmt("am[%d]", d); }

    bool emit_cf(const CfInst& cf, bool in_sub) {
        bool flow = prog.uses_pred_stack;
        const int d = cf.depth;
        switch (cf.op) {
            case CF_NOP: case CF_CALL_FS: case CF_JUMP: case CF_RETURN: case CF_POP_JUMP:
                if (cf.op == CF_POP_JUMP && cf.pop_count) line(fmt("%s = %s && %s;", amc(d + 1 - cf.pop_count).c_str(), am(d - cf.pop_count).c_str(), amc(d - cf.pop_count).c_str()));
                return true;
            case CF_ALU: case CF_ALU_PUSH_BEFORE: case CF_ALU_POP_AFTER: case CF_ALU_POP2_AFTER: case CF_ALU_BREAK:
            case CF_ALU_CONTINUE: case CF_ALU_ELSE_AFTER: {
                if (flow) {
                    line(fmt("if (%s) {", amc(cf.op == CF_ALU_PUSH_BEFORE ? d : d + 1).c_str()));
                    indent++;
                }
                if (cf.op == CF_ALU_PUSH_BEFORE) {
                    line(fmt("%s = %s;", am(d).c_str(), d > 0 ? am(d - 1).c_str() : "true"));
                    line(fmt("%s = %s;", amc(d + 1).c_str(), amc(d).c_str()));
                }
                if (cf.op == CF_ALU_BREAK) line("bool alu_break = false;");
                if (cf.op == CF_ALU_CONTINUE) line("bool alu_continue = false;");
                emit_alu_clause(cf);
                if (cf.op == CF_ALU_BREAK) line("if (alu_break) break;");
                if (cf.op == CF_ALU_CONTINUE) line("if (alu_continue) continue;");
                if (flow) {
                    indent--;
                    line("}");
                }
                if (cf.op == CF_ALU_PUSH_BEFORE) {
                    line("else {");
                    line(fmt("    %s = false; %s = false;", am(d).c_str(), amc(d + 1).c_str()));
                    line("}");
                } else if (cf.op == CF_ALU_POP_AFTER) {
                    line(fmt("%s = %s && %s;", amc(d).c_str(), am(d - 1).c_str(), amc(d - 1).c_str()));
                } else if (cf.op == CF_ALU_POP2_AFTER) {
                    line(fmt("%s = %s && %s;", amc(d - 1).c_str(), am(d - 2).c_str(), amc(d - 2).c_str()));
                } else if (cf.op == CF_ALU_ELSE_AFTER) {
                    line(fmt("%s = !%s;", am(d).c_str(), am(d).c_str()));
                    line(fmt("%s = %s && %s;", amc(d + 1).c_str(), am(d).c_str(), amc(d).c_str()));
                }
                return out.error.empty();
            }
            case CF_TEX: case CF_VTX: case CF_VTX_TC: {
                if (flow) {
                    line(fmt("if (%s) {", amc(d + 1).c_str()));
                    indent++;
                }
                for (auto& t : cf.tex)
                    if (!emit_tex(t)) return false;
                if (flow) {
                    indent--;
                    line("}");
                }
                return true;
            }
            case CF_EXPORT: case CF_EXPORT_DONE: case CF_MEM_STREAM0: case CF_MEM_STREAM1: case CF_MEM_STREAM2:
            case CF_MEM_STREAM3: case CF_MEM_RING:
                return emit_export(cf);
            case CF_ELSE:
                line(fmt("%s = !%s;", am(d).c_str(), am(d).c_str()));
                line(fmt("%s = %s && %s;", amc(d + 1).c_str(), am(d).c_str(), amc(d).c_str()));
                return true;
            case CF_POP:
                line(fmt("%s = %s && %s;", amc(d + 1 - cf.pop_count).c_str(), am(d - cf.pop_count).c_str(),
                         amc(d - cf.pop_count).c_str()));
                return true;
            case CF_LOOP_START: case CF_LOOP_START_DX10: case CF_LOOP_START_NO_AL:
                line("{");
                indent++;
                line(fmt("bool loop_active = %s;", amc(d + 1).c_str()));
                line("int loop_guard = 0;");
                if (cf.op == CF_LOOP_START) line("int aL = 0;");
                line(fmt("while (%s && loop_guard++ < 4096) {", amc(d + 1).c_str()));
                indent++;
                return true;
            case CF_LOOP_END:
                indent--;
                line("}");
                line(fmt("%s = loop_active;", amc(d + 1).c_str()));
                indent--;
                line("}");
                return true;
            case CF_LOOP_BREAK:
                line(fmt("if (%s) break;", amc(d + 1).c_str()));
                return true;
            case CF_LOOP_CONTINUE:
                line(fmt("if (%s) continue;", amc(d + 1).c_str()));
                return true;
            case CF_CALL: {
                if (in_sub) return fail("nested CALL");
                auto it = prog.subs.find(cf.addr);
                if (it == prog.subs.end()) return fail("CALL target missing");
                line("{  // inlined subroutine");
                indent++;
                for (auto& s : it->second)
                    if (!emit_cf(s, true)) return false;
                indent--;
                line("}");
                return true;
            }
            case CF_KILL:
                if (stage_idx == 1) line(fmt("if (%s) discard_fragment();", amc(d + 1).c_str()));
                return true;
            default:
                return fail(fmt("unsupported CF instruction 0x%x", cf.op));
        }
    }

    // ---- textures ----
    bool declare_textures() {
        uint32_t base = kTexBase[stage_idx == 0 ? 0 : stage_idx == 1 ? 1 : 2];
        for (uint32_t slot = 0; slot < 32; slot++) {
            if (!((prog.tex_used >> slot) & 1)) continue;
            if (slot >= 18) return fail(fmt("texture slot %u", slot));
            uint32_t w0 = reg(base + slot * 7), w4 = reg(base + slot * 7 + 4);
            uint32_t dim = w0 & 7;
            tex_dim[slot] = dim;
            bool compare = (prog.tex_compare >> slot) & 1;
            bool is_int = ((w4 >> 8) & 3) == 1;
            if (is_int) tex_int.insert(slot);
            std::string ct = is_int ? (((w4 & 3) == 1) ? "int" : "uint") : "float";
            if (is_int && ct == "uint") ct = "uint";
            std::string t;
            if (compare) {
                switch (dim) {
                    case 1: t = "depth2d<float>"; break;
                    case 5: t = "depth2d_array<float>"; break;
                    case 3: t = "depthcube_array<float>"; break;
                    default: return fail(fmt("compare sampling on dim %u", dim));
                }
            } else
                switch (dim) {
                    case 0: t = "texture1d<" + ct + ">"; break;
                    case 1: t = "texture2d<" + ct + ">"; break;
                    case 2: t = "texture3d<" + ct + ">"; break;
                    case 3: t = "texturecube_array<" + ct + ">"; break;
                    case 4: t = "texture1d_array<" + ct + ">"; break;
                    case 5: t = "texture2d_array<" + ct + ">"; break;
                    case 6: t = "texture2d_ms<" + ct + ">"; break;
                    case 7: t = "texture2d_ms_array<" + ct + ">"; break;
                }
            tex_type[slot] = t;
        }
        return true;
    }

    // ---- vertex fetch prologue from the fetch shader ----
    bool vertex_prologue(std::string& pro) {
        if (!in.fetch || !in.fetch_size) return true;
        // fetch shaders: CF program of VTX_TC clauses (+ RETURN), or legacy bare VTX instructions
        const uint32_t* w = (const uint32_t*)in.fetch;
        uint32_t n = in.fetch_size / 4;
        std::vector<TexInst> attrs;
        bool cf_form = (w[0] & 1) == 0 && w[0] <= 0x30 && (w[1] & ~((3u << 10) | (1u << 19))) == 0x01800000;
        if (cf_form) {
            // fetch programs end with RETURN, not END_OF_PROGRAM: walk CF until RETURN
            for (uint32_t i = 0; i < n / 2; i++) {
                uint32_t w1 = w[i * 2 + 1];
                uint32_t op = (w1 >> 23) & 0x7F;
                if (op == CF_RETURN) break;
                if (op == CF_VTX_TC || op == CF_VTX) {
                    uint32_t addr = w[i * 2], count = (((w1 >> 10) & 7) | (((w1 >> 19) & 1) << 3)) + 1;
                    for (uint32_t k = 0; k < count; k++) {
                        uint32_t b = addr * 2 + k * 4;
                        if (b + 3 >= n) return fail("fetch clause out of range");
                        TexInst t;
                        uint32_t w0 = w[b], v1 = w[b + 1], w2 = w[b + 2];
                        t.op = w0 & 0x1F;
                        t.fetch_type = (w0 >> 5) & 3;
                        t.resource = (w0 >> 8) & 0xFF;
                        t.src_sel_x = (w0 >> 24) & 3;
                        t.semantic = v1 & 0xFF;
                        t.dst_gpr = v1 & 0x7F;
                        for (int j = 0; j < 4; j++) t.dst_sel[j] = (v1 >> (9 + 3 * j)) & 7;
                        t.data_format = (v1 >> 22) & 0x3F;
                        t.num_format = (v1 >> 28) & 3;
                        t.format_signed = (v1 >> 30) & 1;
                        t.vtx_offset = w2 & 0xFFFF;
                        t.endian = (w2 >> 16) & 3;
                        attrs.push_back(t);
                    }
                } else if (op != CF_NOP)
                    return fail(fmt("fetch shader CF 0x%x", op));
            }
        } else {
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                uint32_t w0 = w[i];
                if ((w0 & 0x1F) != 1) continue;  // VTX_INST_SEMANTIC
                TexInst t;
                uint32_t v1 = w[i + 1], w2 = w[i + 2];
                t.op = 1;
                t.fetch_type = (w0 >> 5) & 3;
                t.resource = (w0 >> 8) & 0xFF;
                t.src_sel_x = (w0 >> 24) & 3;
                t.semantic = v1 & 0xFF;
                for (int j = 0; j < 4; j++) t.dst_sel[j] = (v1 >> (9 + 3 * j)) & 7;
                t.data_format = (v1 >> 22) & 0x3F;
                t.num_format = (v1 >> 28) & 3;
                t.format_signed = (v1 >> 30) & 1;
                t.vtx_offset = w2 & 0xFFFF;
                t.endian = (w2 >> 16) & 3;
                attrs.push_back(t);
            }
        }
        for (auto& t : attrs) {
            // semantic id -> GPR via SQ_VTX_SEMANTIC_n (R0 is reserved for vertex/instance id)
            int gidx = -1;
            for (int f = 0; f < 32; f++)
                if ((reg(mmSQ_VTX_SEMANTIC_0 + f) & 0xFF) == t.semantic) {
                    gidx = f + 1;
                    break;
                }
            if (gidx < 0 || !prog.gpr(gidx)) continue;
            if (t.resource < 0xA0 || t.resource >= 0xB0) {
                pro += fmt("    %s = int4(0);  // attribute from invalid buffer 0x%x\n", gpr(gidx).c_str(), t.resource);
                continue;
            }
            uint32_t b = t.resource - 0xA0;
            need_ab(b);
            // fetch index: per-vertex (vid) or per-instance (iid / divisor)
            std::string ix = "vid";
            if (t.fetch_type == 1) {  // instance data
                if (t.src_sel_x == 3) ix = "iid";  // divisor 1
                else if (t.src_sel_x == 1) ix = fmt("iid / %uu", std::max<uint32_t>(1, reg(mmVGT_INSTANCE_STEP_RATE_0)));
                else if (t.src_sel_x == 2) ix = fmt("iid / %uu", std::max<uint32_t>(1, reg(mmVGT_INSTANCE_STEP_RATE_0 + 1)));
                else ix = "iid";
            } else if (t.fetch_type == 2)
                ix = "0u";
            std::string err;
            uint8_t one_is_float = 1;
            std::string e = fetch_expr(b, ix, t.vtx_offset, t.data_format, t.num_format, t.format_signed, t.endian,
                                       &one_is_float, err);
            if (!err.empty()) return fail(err);
            pro += "    {\n        int4 a = " + e + ";\n";
            for (int c = 0; c < 4; c++) {
                uint32_t s = t.dst_sel[c];
                if (s == 7) continue;
                std::string v = s < 4 ? fmt("a.%c", kChan[s]) : s == 4 ? "0" : (one_is_float ? "0x3f800000" : "1");
                pro += fmt("        %s.%c = %s;\n", gpr(gidx).c_str(), kChan[c], v.c_str());
            }
            pro += "    }\n";
        }
        return true;
    }

    bool run() {
        stage_idx = in.stage == Stage::Vertex ? 0 : in.stage == Stage::Pixel ? 1 : 2;
        if (stage_idx == 2) return fail("geometry shaders are not translated yet (BotW: none measured)");
        prog = r600::parse((const uint32_t*)in.program, in.program_size / 4);
        if (!prog.ok) return fail("decode: " + prog.error);
        rel_regs = prog.uses_rel_gpr;
        if (!declare_textures()) return false;
        // PS input GPRs must be declared even if only written by interpolation
        uint32_t ps_ctl0 = 0, ps_ctl1 = 0, num_inputs = 0;
        if (stage_idx == 1) {
            ps_ctl0 = reg(mmSPI_PS_IN_CONTROL_0);
            ps_ctl1 = reg(mmSPI_PS_IN_CONTROL_1);
            num_inputs = std::min<uint32_t>(ps_ctl0 & 0x3F, 32);
        }
        // body
        std::string pro;
        if (stage_idx == 0) {
            if (prog.gpr(0)) pro += fmt("    %s = int4(int(vid), 0, 0, int(iid));\n", gpr(0).c_str());
            if (!vertex_prologue(pro)) return false;
        } else {
            bool pos_ena = (ps_ctl0 >> 8) & 1;
            uint32_t pos_addr = (ps_ctl0 >> 10) & 0x1F;
            for (uint32_t i = 0; i < num_inputs; i++) {
                if (!prog.gpr(i) && !rel_regs) continue;
                if (pos_ena && i == pos_addr) {
                    pro += fmt("    %s = I4(float4(in.pos.xy * info.frag_coord_scale.xy, in.pos.z, 1.0f / in.pos.w));\n",
                               gpr(i).c_str());
                    continue;
                }
                uint32_t sem = reg(mmSPI_PS_INPUT_CNTL_0 + i) & 0xFF;
                pro += fmt("    %s = I4(in.s%u);\n", gpr(i).c_str(), sem);
            }
            if (pos_ena && pos_addr >= num_inputs && prog.gpr(pos_addr))
                pro += fmt("    %s = I4(float4(in.pos.xy * info.frag_coord_scale.xy, in.pos.z, 1.0f / in.pos.w));\n",
                           gpr(pos_addr).c_str());
            if ((ps_ctl1 >> 8) & 1) {  // front face
                uint32_t r = (ps_ctl1 >> 12) & 0x1F, ch = (ps_ctl1 >> 9) & 3;
                pro += fmt("    %s.%c = front ? 0x3f800000 : 0;\n", gpr(r).c_str(), kChan[ch]);
            }
            uint32_t param_gen = (ps_ctl0 >> 15) & 0xF;
            if (param_gen) {
                uint32_t r = (ps_ctl0 >> 19) & 0x7F;
                pro += fmt("    %s = I4(float4(point_coord, point_coord));\n", gpr(r).c_str());
                uses_point_coord = true;
            }
        }
        for (auto& cf : prog.cf)
            if (!emit_cf(cf, false)) return false;

        // assemble
        std::string s = kPrelude;
        std::string decl_regs;
        if (rel_regs) decl_regs = "    int4 R[128];\n    for (int i = 0; i < 128; i++) R[i] = int4(0);\n";
        else
            for (uint32_t g = 0; g < 128; g++)
                if (prog.gpr(g)) decl_regs += fmt("    int4 R%u = int4(0);\n", g);
        // bindings
        out.bindings.push_back({"info", 0, 0});
        std::string params = "constant StageInfo& info [[buffer(0)]]";
        if (prog.uses_cfile) {
            cfile_index = (int)next_buffer++;
            params += fmt(",\n    constant int4* cfile [[buffer(%d)]]", cfile_index);
            out.bindings.push_back({"cfile", 0, (uint32_t)cfile_index});
        }
        for (uint32_t slot = 0; slot < 16; slot++)
            if ((prog.kcache_banks >> slot) & 1) need_ub(slot);
        for (auto& [slot, idx] : ub_index) {
            idx = next_buffer++;
            params += fmt(",\n    device const int4* ub%u [[buffer(%u)]]", slot, idx);
            out.bindings.push_back({"ub", slot, idx});
        }
        for (uint32_t b : attrib_buffers) {
            uint32_t idx = next_buffer++;
            params += fmt(",\n    device const uchar* ab%u [[buffer(%u)]]", b, idx);
            out.bindings.push_back({"ab", b, idx});
        }
        if (next_buffer > 31) return fail("more than 31 buffer bindings");
        for (auto& [slot, t] : tex_type) {
            params += fmt(",\n    %s tex%u [[texture(%u)]]", t.c_str(), slot, slot);
            out.bindings.push_back({"tex", slot, slot});
        }
        for (uint32_t smp = 0; smp < 32; smp++) {
            if (!((prog.sampler_used >> smp) & 1)) continue;
            if (smp >= 16) return fail(fmt("sampler slot %u (Metal has 16)", smp));
            params += fmt(",\n    sampler smp%u [[sampler(%u)]]", smp, smp);
            out.bindings.push_back({"smp", smp, smp});
        }
        if (stage_idx == 0) {
            s += "struct VOut {\n    float4 pos [[position]];\n";
            for (uint32_t sem : vs_out_sems) s += fmt("    float4 s%u [[user(s%u)]];\n", sem, sem);
            if (writes_psize) s += "    float psize [[point_size]];\n";
            s += "};\n";
            s += "vertex VOut " + in.name + "(uint vid [[vertex_id]], uint iid [[instance_id]],\n    " + params + ") {\n";
            s += "    VOut out = {};\n";
        } else {
            s += "struct PIn {\n    float4 pos [[position]];\n";
            std::set<uint32_t> seen;
            for (uint32_t i = 0; i < num_inputs; i++) {
                bool pos_ena = (ps_ctl0 >> 8) & 1;
                if (pos_ena && i == ((ps_ctl0 >> 10) & 0x1F)) continue;
                uint32_t c = reg(mmSPI_PS_INPUT_CNTL_0 + i), sem = c & 0xFF;
                if (!seen.insert(sem).second) continue;
                out.ps_in_semantics.push_back(sem);
                std::string q;
                if ((c >> 10) & 1) q = " [[flat]]";
                else if ((c >> 12) & 1) q = (c >> 11) & 1 ? " [[centroid_no_perspective]]" : " [[center_no_perspective]]";
                else if ((c >> 11) & 1) q = " [[centroid_perspective]]";
                s += fmt("    float4 s%u [[user(s%u)]]%s;\n", sem, sem, q.c_str());
            }
            s += "};\nstruct POut {\n";
            for (uint32_t c : ps_colors) {
                uint32_t nt = (in.regs[0xA028 + c] >> 12) & 7;
                s += fmt("    %s c%u [[color(%u)]];\n", nt == 4 ? "uint4" : nt == 5 ? "int4" : "float4", c, c);
            }
            if (writes_depth) s += "    float depth [[depth(any)]];\n";
            if (ps_colors.empty() && !writes_depth) s += "    float4 c0 [[color(0)]];\n";
            s += "};\n";
            s += "fragment POut " + in.name + "(PIn in [[stage_in]], bool front [[front_facing]]";
            if (uses_point_coord) s += ", float2 point_coord [[point_coord]]";
            s += ",\n    " + params + ") {\n";
            s += "    POut out = {};\n";
        }
        s += decl_regs;
        s += "    int4 PV = int4(0);\n    int PS = 0;\n    int4 AR = int4(0);\n    bool pred = true;\n";
        if (prog.uses_gradients) s += "    float4 gradH = float4(0.0f), gradV = float4(0.0f);\n";
        if (uses_cube_index)
            for (auto& [slot, t] : tex_type)
                if (tex_dim[slot] == 3) s += fmt("    float cube_index%u = 0.0f;\n", slot);
        if (prog.uses_pred_stack) {
            int n = prog.max_depth + 3;
            s += fmt("    bool am[%d];\n    bool amc[%d];\n", n, n + 1);
            s += fmt("    for (int i = 0; i < %d; i++) am[i] = false;\n    for (int i = 0; i < %d; i++) amc[i] = false;\n", n, n + 1);
            s += "    am[0] = true; amc[0] = true; amc[1] = true;\n";
        }
        s += pro;
        s += body;
        s += "    return out;\n}\n";
        out.source = s;
        for (uint32_t r : variant) {
            out.variant_regs.push_back(fmt("%04x=%08x", r, in.regs[r]));
            out.deps.push_back({r, in.regs[r]});
        }
        for (auto& [slot, t] : tex_type) out.tex_mask |= 1u << slot;
        out.sampler_mask = prog.sampler_used;
        for (auto& [slot, idx] : ub_index) out.ub_mask |= 1u << slot;
        for (uint32_t b : attrib_buffers) out.ab_mask |= 1u << b;
        out.uses_cfile = prog.uses_cfile;
        out.vs_out_semantics.assign(vs_out_sems.begin(), vs_out_sems.end());
        for (uint32_t c : ps_colors) out.ps_color_mask |= 1u << c;
        return true;
    }
    bool uses_point_coord = false;
};
}  // namespace

Output emit(const Input& in) {
    Output o;
    Emitter e(in, o);
    o.ok = e.run() && o.error.empty();
    return o;
}
}  // namespace msl
