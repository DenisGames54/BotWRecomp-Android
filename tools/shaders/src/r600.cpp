// R600/R700 (GPU7) program decoder, see r600.h.
#include "r600.h"

#include <stdio.h>

#include <algorithm>
#include <set>

namespace r600 {
namespace {
struct Ctx {
    const uint32_t* w;
    uint32_t n;
    Program* prog;
    void fail(const std::string& e) {
        if (prog->ok) prog->error = e;
        prog->ok = false;
    }
    uint32_t at(uint32_t i) {
        if (i >= n) {
            fail("read past end of program");
            return 0;
        }
        return w[i];
    }
};

bool is_alu_cf(uint32_t w1) { return ((w1 >> 23) & 0x7F) >= 0x40; }

// decode one CF instruction at slot i; returns false on unknown opcode
bool decode_cf(Ctx& c, uint32_t i, CfInst& cf) {
    uint32_t w0 = c.at(i * 2), w1 = c.at(i * 2 + 1);
    cf.index = i;
    if (is_alu_cf(w1)) {
        uint32_t op = (w1 >> 26) & 0xF;
        cf.op = (uint8_t)(0x80 | op);
        cf.addr = w0 & 0x3FFFFF;
        cf.kc_bank[0] = (w0 >> 22) & 0xF;
        cf.kc_bank[1] = (w0 >> 26) & 0xF;
        cf.kc_mode[0] = (w0 >> 30) & 3;
        cf.kc_mode[1] = w1 & 3;
        cf.kc_addr[0] = ((w1 >> 2) & 0xFF) * 16;
        cf.kc_addr[1] = ((w1 >> 10) & 0xFF) * 16;
        cf.count = ((w1 >> 18) & 0x7F) + 1;
        cf.alt_const = (w1 >> 25) & 1;
        cf.wqm = (w1 >> 30) & 1;
        cf.barrier = (w1 >> 31) & 1;
        switch (cf.op) {
            case CF_ALU: case CF_ALU_PUSH_BEFORE: case CF_ALU_POP_AFTER: case CF_ALU_POP2_AFTER:
            case CF_ALU_CONTINUE: case CF_ALU_BREAK: case CF_ALU_ELSE_AFTER:
                return true;
            default:
                return false;
        }
    }
    cf.op = (w1 >> 23) & 0x7F;
    cf.eop = (w1 >> 21) & 1;
    cf.valid_pixel = (w1 >> 22) & 1;
    cf.wqm = (w1 >> 30) & 1;
    cf.barrier = (w1 >> 31) & 1;
    if (cf.op >= CF_MEM_STREAM0 && cf.op <= CF_EXPORT_DONE) {
        // CF_ALLOC_EXPORT_WORD0 / WORD1_{SWIZ,BUF}
        cf.exp_array_base = w0 & 0x1FFF;
        cf.exp_type = (w0 >> 13) & 3;
        cf.exp_gpr = (w0 >> 15) & 0x7F;
        cf.exp_rel = (w0 >> 22) & 1;
        cf.exp_index_gpr = (w0 >> 23) & 0x7F;
        cf.exp_elem_size = (w0 >> 30) & 3;
        cf.exp_burst = (w1 >> 17) & 0xF;
        if (cf.op == CF_EXPORT || cf.op == CF_EXPORT_DONE) {
            for (int k = 0; k < 4; k++) cf.exp_sel[k] = (w1 >> (3 * k)) & 7;
        } else {
            cf.exp_array_size = w1 & 0xFFF;
            cf.exp_comp_mask = (w1 >> 12) & 0xF;
        }
        return true;
    }
    cf.addr = w0;
    cf.pop_count = w1 & 7;
    cf.cf_const = (w1 >> 3) & 0x1F;
    cf.cond = (w1 >> 8) & 3;
    cf.count = (((w1 >> 10) & 7) | (((w1 >> 19) & 1) << 3)) + 1;
    cf.call_count = (w1 >> 13) & 0x3F;
    switch (cf.op) {
        case CF_NOP: case CF_TEX: case CF_VTX: case CF_VTX_TC: case CF_LOOP_START: case CF_LOOP_END:
        case CF_LOOP_START_DX10: case CF_LOOP_START_NO_AL: case CF_LOOP_CONTINUE: case CF_LOOP_BREAK: case CF_JUMP:
        case CF_PUSH: case CF_PUSH_ELSE: case CF_ELSE: case CF_POP: case CF_POP_JUMP: case CF_POP_PUSH:
        case CF_POP_PUSH_ELSE: case CF_CALL: case CF_CALL_FS: case CF_RETURN: case CF_EMIT_VERTEX:
        case CF_EMIT_CUT_VERTEX: case CF_CUT_VERTEX: case CF_KILL:
            return true;
        default:
            return false;
    }
}

void note_src(Program& p, const CfInst& cf, AluSrc& s) {
    if (s.sel < 128) {
        p.use_gpr(s.sel);
        if (s.rel) p.uses_rel_gpr = true, p.uses_ar = true;
    } else if (s.sel < 192) {
        p.uses_kcache = true;
        int bank = s.sel < SRC_KCACHE1 ? 0 : 1;
        p.kcache_banks |= 1u << cf.kc_bank[bank];
        if (s.rel) p.uses_ar = true;
    } else if (s.sel >= SRC_CFILE) {
        p.uses_cfile = true;
        if (s.rel) p.uses_ar = true;
    }
}

void decode_alu_clause(Ctx& c, CfInst& cf) {
    Program& p = *c.prog;
    uint32_t i = 0;
    AluGroup g;
    uint32_t used_units = 0;
    while (i < cf.count && c.prog->ok) {
        uint32_t w0 = c.at((cf.addr + i) * 2), w1 = c.at((cf.addr + i) * 2 + 1);
        i++;
        AluInst a;
        a.src[0] = {(uint16_t)(w0 & 0x1FF), (uint8_t)((w0 >> 10) & 3), (bool)((w0 >> 12) & 1), false, (bool)((w0 >> 9) & 1)};
        a.src[1] = {(uint16_t)((w0 >> 13) & 0x1FF), (uint8_t)((w0 >> 23) & 3), (bool)((w0 >> 25) & 1), false,
                    (bool)((w0 >> 22) & 1)};
        a.index_mode = (w0 >> 26) & 7;
        bool last = (w0 >> 31) & 1;
        a.dst_gpr = (w1 >> 21) & 0x7F;
        a.dst_rel = (w1 >> 28) & 1;
        a.dst_chan = (w1 >> 29) & 3;
        a.clamp = (w1 >> 31) & 1;
        uint32_t op3 = (w1 >> 13) & 0x1F;
        if (op3 >= 8) {
            a.op3 = true;
            a.op = (uint16_t)op3;
            a.src[2] = {(uint16_t)(w1 & 0x1FF), (uint8_t)((w1 >> 10) & 3), (bool)((w1 >> 12) & 1), false,
                        (bool)((w1 >> 9) & 1)};
            a.nsrc = 3;
            a.write = true;
        } else {
            a.op = (uint16_t)((w1 >> 7) & 0x7FF);
            a.src[0].abs = w1 & 1;
            a.src[1].abs = (w1 >> 1) & 1;
            a.update_exec = (w1 >> 2) & 1;
            a.update_pred = (w1 >> 3) & 1;
            a.write = (w1 >> 4) & 1;
            a.omod = (w1 >> 5) & 3;
            a.nsrc = 2;
            if (!op2_name(a.op)) {
                char b[64];
                snprintf(b, sizeof b, "unknown ALU OP2 0x%x", a.op);
                c.fail(b);
            }
        }
        // unit assignment: vector unit by destination channel unless taken or trans-only
        uint32_t unit = a.dst_chan;
        if (!a.op3 && is_trans_only(a.op)) unit = 4;
        if (unit < 4 && (used_units >> unit) & 1) unit = 4;
        if (unit == 4 && (used_units >> 4) & 1) c.fail("ALU group uses the trans unit twice");
        used_units |= 1u << unit;
        a.unit = (uint8_t)unit;
        if (a.dst_rel) p.uses_rel_gpr = true, p.uses_ar = true;
        if (a.write || a.op3) p.use_gpr(a.dst_gpr);
        for (int k = 0; k < a.nsrc; k++) note_src(p, cf, a.src[k]);
        if (!a.op3 && (a.op == KILLE || a.op == KILLGT || a.op == KILLGE || a.op == KILLNE || a.op == KILLE_INT ||
                       a.op == KILLGT_INT || a.op == KILLGE_INT || a.op == KILLNE_INT || a.op == KILLGT_UINT ||
                       a.op == KILLGE_UINT))
            p.uses_kill = true;
        if (!a.op3 && a.op == CUBE) p.uses_cube = true;
        if (!a.op3 && (a.op == MOVA || a.op == MOVA_FLOOR || a.op == MOVA_INT)) p.uses_ar = true;
        g.insts.push_back(a);
        if (last) {
            // literals follow the group: 2 or 4 dwords depending on which channels are referenced
            uint32_t lit_mask = 0;
            for (auto& x : g.insts)
                for (int k = 0; k < x.nsrc; k++)
                    if (x.src[k].sel == SRC_LITERAL) lit_mask |= 1u << x.src[k].chan;
            if (lit_mask) {
                uint32_t pairs = (lit_mask & 0xC) ? 2 : 1;
                for (uint32_t k = 0; k < pairs * 2; k++) g.literal[k] = c.at((cf.addr + i) * 2 + k);
                i += pairs;
            }
            cf.groups.push_back(std::move(g));
            g = AluGroup{};
            used_units = 0;
        }
    }
    if (!g.insts.empty()) c.fail("ALU clause ends inside an instruction group");
}

void decode_tex_clause(Ctx& c, CfInst& cf, bool vtx) {
    Program& p = *c.prog;
    for (uint32_t k = 0; k < cf.count && p.ok; k++) {
        uint32_t base = cf.addr * 2 + k * 4;
        uint32_t w0 = c.at(base), w1 = c.at(base + 1), w2 = c.at(base + 2);
        TexInst t;
        t.op = w0 & 0x1F;
        if (vtx || t.op == TEX_VFETCH || t.op == TEX_SEMANTIC) {
            // VTX_WORD0/1/2
            t.op = vtx ? (uint8_t)(w0 & 0x1F) : t.op;
            t.fetch_type = (w0 >> 5) & 3;
            t.resource = (w0 >> 8) & 0xFF;
            t.src_gpr = (w0 >> 16) & 0x7F;
            t.src_rel = (w0 >> 23) & 1;
            t.src_sel_x = (w0 >> 24) & 3;
            t.mega_fetch = (w0 >> 26) & 0x3F;
            t.dst_gpr = w1 & 0x7F;
            t.dst_rel = (w1 >> 7) & 1;
            t.semantic = w1 & 0xFF;
            for (int j = 0; j < 4; j++) t.dst_sel[j] = (w1 >> (9 + 3 * j)) & 7;
            t.use_const_fields = (w1 >> 21) & 1;
            t.data_format = (w1 >> 22) & 0x3F;
            t.num_format = (w1 >> 28) & 3;
            t.format_signed = (w1 >> 30) & 1;
            t.srf_mode = (w1 >> 31) & 1;
            t.vtx_offset = w2 & 0xFFFF;
            t.endian = (w2 >> 16) & 3;
            p.use_gpr(t.src_gpr);
            if (t.op != TEX_SEMANTIC) p.use_gpr(t.dst_gpr);
        } else if (t.op == TEX_MEM) {
            t.src_gpr = (w0 >> 16) & 0x7F;
            t.src_rel = (w0 >> 23) & 1;
            t.src_sel_x = (w0 >> 24) & 3;
            t.dst_gpr = w1 & 0x7F;
            t.dst_rel = (w1 >> 7) & 1;
            for (int j = 0; j < 4; j++) t.dst_sel[j] = (w1 >> (9 + 3 * j)) & 7;
            t.data_format = (w1 >> 22) & 0x3F;
            t.num_format = (w1 >> 28) & 3;
            t.format_signed = (w1 >> 30) & 1;
            t.mem_array_base = w2 & 0x1FFF;
            t.endian = (w2 >> 16) & 3;
            t.mem_array_size = (w2 >> 20) & 0xFFF;
            p.use_gpr(t.src_gpr);
            p.use_gpr(t.dst_gpr);
        } else {
            t.resource = (w0 >> 8) & 0xFF;
            t.src_gpr = (w0 >> 16) & 0x7F;
            t.src_rel = (w0 >> 23) & 1;
            t.dst_gpr = w1 & 0x7F;
            t.dst_rel = (w1 >> 7) & 1;
            for (int j = 0; j < 4; j++) t.dst_sel[j] = (w1 >> (9 + 3 * j)) & 7;
            int lb = (w1 >> 21) & 0x7F;
            t.lod_bias = (int8_t)(lb & 0x40 ? lb | 0x80 : lb);
            for (int j = 0; j < 4; j++) t.coord_norm[j] = (w1 >> (28 + j)) & 1;
            for (int j = 0; j < 3; j++) {
                int o = (w2 >> (5 * j)) & 0x1F;
                t.offset[j] = (int8_t)(o & 0x10 ? o | 0xE0 : o);
            }
            t.sampler = (w2 >> 15) & 0x1F;
            for (int j = 0; j < 4; j++) t.src_sel[j] = (w2 >> (20 + 3 * j)) & 7;
            p.use_gpr(t.src_gpr);
            bool writes = t.op != TEX_SET_GRADIENTS_H && t.op != TEX_SET_GRADIENTS_V && t.op != TEX_SET_CUBEMAP_INDEX;
            if (writes) p.use_gpr(t.dst_gpr);
            switch (t.op) {
                case TEX_LD: case TEX_GET_RESINFO: case TEX_GET_NUM_SAMPLES: case TEX_GET_LOD: case TEX_FETCH4:
                case TEX_SAMPLE: case TEX_SAMPLE_L: case TEX_SAMPLE_LB: case TEX_SAMPLE_LZ: case TEX_SAMPLE_G:
                case TEX_SAMPLE_G_L: case TEX_SAMPLE_G_LB: case TEX_SAMPLE_G_LZ: case TEX_SAMPLE_C: case TEX_SAMPLE_C_L:
                case TEX_SAMPLE_C_LB: case TEX_SAMPLE_C_LZ: case TEX_SAMPLE_C_G: case TEX_SAMPLE_C_G_L:
                case TEX_SAMPLE_C_G_LB: case TEX_SAMPLE_C_G_LZ:
                    if (t.resource < 32) p.tex_used |= 1u << t.resource;
                    if (t.sampler < 32) p.sampler_used |= 1u << t.sampler;
                    if (t.op >= TEX_SAMPLE_C && t.resource < 32) p.tex_compare |= 1u << t.resource;
                    if (t.op == TEX_SAMPLE_G || t.op == TEX_SAMPLE_G_L || t.op == TEX_SAMPLE_G_LB ||
                        t.op == TEX_SAMPLE_G_LZ || t.op >= TEX_SAMPLE_C_G)
                        p.uses_gradients = true;
                    break;
                case TEX_GET_GRADIENTS_H: case TEX_GET_GRADIENTS_V: break;
                case TEX_SET_GRADIENTS_H: case TEX_SET_GRADIENTS_V: p.uses_gradients = true; break;
                case TEX_SET_CUBEMAP_INDEX: break;
                default: {
                    char b[64];
                    snprintf(b, sizeof b, "unsupported TEX instruction 0x%x", t.op);
                    c.fail(b);
                }
            }
        }
        if (t.src_rel || t.dst_rel) p.uses_rel_gpr = true, p.uses_ar = true;
        cf.tex.push_back(t);
    }
}

void decode_list(Ctx& c, uint32_t start, std::vector<CfInst>& out, bool sub, std::set<uint32_t>& calls) {
    Program& p = *c.prog;
    for (uint32_t i = start; i < c.n / 2 && p.ok; i++) {
        CfInst cf;
        if (!decode_cf(c, i, cf)) {
            char b[64];
            snprintf(b, sizeof b, "unknown CF instruction 0x%x at %u", cf.op, i);
            c.fail(b);
            return;
        }
        if (cf.op >= 0x80) decode_alu_clause(c, cf);
        else if (cf.op == CF_TEX) decode_tex_clause(c, cf, false);
        else if (cf.op == CF_VTX || cf.op == CF_VTX_TC) decode_tex_clause(c, cf, true);
        else if (cf.op == CF_CALL) calls.insert(cf.addr);
        else if (cf.op == CF_CALL_FS) p.calls_fs = true;
        else if (cf.op == CF_EXPORT || cf.op == CF_EXPORT_DONE || (cf.op >= CF_MEM_STREAM0 && cf.op <= CF_MEM_RING)) {
            for (uint32_t b = 0; b <= cf.exp_burst; b++) {
                p.use_gpr(cf.exp_gpr + b);
                uint32_t m = 0;
                if (cf.op == CF_EXPORT || cf.op == CF_EXPORT_DONE) {
                    for (int k = 0; k < 4; k++)
                        if (cf.exp_sel[k] != 7) m |= 1u << k;
                } else
                    m = cf.exp_comp_mask;
                p.exports[(uint32_t)cf.exp_type << 16 | (cf.exp_array_base + b)] |= m;
            }
            if (cf.exp_rel) p.uses_rel_gpr = true;
        }
        bool end = cf.eop || (sub && cf.op == CF_RETURN);
        out.push_back(std::move(cf));
        if (end) return;
    }
    if (!sub) c.fail("program has no END_OF_PROGRAM");
}

// predicate-stack depth bookkeeping, as Cemu's analyzer does it
void assign_depth(Program& p, std::vector<CfInst>& list) {
    int d = 0;
    for (auto& cf : list) {
        switch (cf.op) {
            case CF_ALU_PUSH_BEFORE:
                d++;
                cf.depth = d;
                p.uses_pred_stack = true;
                break;
            case CF_ALU_POP_AFTER: cf.depth = d; d -= 1; p.uses_pred_stack = true; break;
            case CF_ALU_POP2_AFTER: cf.depth = d; d -= 2; p.uses_pred_stack = true; break;
            case CF_POP: cf.depth = d; d -= cf.pop_count; p.uses_pred_stack = true; break;
            case CF_ELSE: case CF_ALU_ELSE_AFTER: case CF_ALU_BREAK: case CF_ALU_CONTINUE:
                cf.depth = d;
                p.uses_pred_stack = true;
                break;
            default: cf.depth = d; break;
        }
        p.max_depth = std::max(p.max_depth, d);
        if (d < 0) {
            if (p.ok) p.error = "predicate stack underflow";
            p.ok = false;
            d = 0;
        }
    }
}
}  // namespace

Program parse(const uint32_t* words, uint32_t nwords) {
    Program p;
    Ctx c{words, nwords, &p};
    std::set<uint32_t> calls;
    decode_list(c, 0, p.cf, false, calls);
    for (uint32_t a : calls) {
        if (!p.ok) break;
        std::set<uint32_t> nested;
        decode_list(c, a, p.subs[a], true, nested);
        if (!nested.empty()) c.fail("nested subroutine calls");
    }
    if (p.ok) {
        assign_depth(p, p.cf);
        for (auto& [a, l] : p.subs) assign_depth(p, l);
    }
    return p;
}

bool is_trans_only(uint16_t op) {
    switch (op) {
        case COS: case SIN: case RECIP_CLAMPED: case RECIP_FF: case RECIP_IEEE: case RECIPSQRT_IEEE:
        case RECIPSQRT_CLAMPED: case RECIPSQRT_FF: case MULLO_INT: case MULLO_UINT: case MULHI_INT: case MULHI_UINT:
        case FLT_TO_INT: case FLT_TO_UINT: case INT_TO_FLT: case UINT_TO_FLT: case LOG_CLAMPED: case LOG_IEEE:
        case EXP_IEEE: case SQRT_IEEE: case RECIP_INT: case RECIP_UINT:
            return true;
        default:
            return false;
    }
}

bool is_reduction(const AluInst& a) {
    return !a.op3 && (a.op == DOT4 || a.op == DOT4_IEEE || a.op == CUBE || a.op == MAX4);
}

const char* op2_name(uint16_t op) {
    switch (op) {
#define N(x) case x: return #x;
        N(ADD) N(MUL) N(MUL_IEEE) N(MAX) N(MIN) N(MAX_DX10) N(MIN_DX10) N(SETE) N(SETGT) N(SETGE) N(SETNE)
        N(SETE_DX10) N(SETGT_DX10) N(SETGE_DX10) N(SETNE_DX10) N(FRACT) N(TRUNC) N(CEIL) N(RNDNE) N(FLOOR) N(MOVA)
        N(MOVA_FLOOR) N(MOVA_INT) N(MOV) N(NOP) N(PRED_SETGT_UINT) N(PRED_SETGE_UINT) N(PRED_SETE) N(PRED_SETGT)
        N(PRED_SETGE) N(PRED_SETNE) N(PRED_SET_INV) N(PRED_SET_POP) N(PRED_SET_CLR) N(PRED_SET_RESTORE)
        N(PRED_SETE_PUSH) N(PRED_SETGT_PUSH) N(PRED_SETGE_PUSH) N(PRED_SETNE_PUSH) N(KILLE) N(KILLGT) N(KILLGE)
        N(KILLNE) N(AND_INT) N(OR_INT) N(XOR_INT) N(NOT_INT) N(ADD_INT) N(SUB_INT) N(MAX_INT) N(MIN_INT) N(MAX_UINT)
        N(MIN_UINT) N(SETE_INT) N(SETGT_INT) N(SETGE_INT) N(SETNE_INT) N(SETGT_UINT) N(SETGE_UINT) N(KILLGT_UINT)
        N(KILLGE_UINT) N(PRED_SETE_INT) N(PRED_SETGT_INT) N(PRED_SETGE_INT) N(PRED_SETNE_INT) N(KILLE_INT)
        N(KILLGT_INT) N(KILLGE_INT) N(KILLNE_INT) N(DOT4) N(DOT4_IEEE) N(CUBE) N(MAX4) N(MOVA_GPR_INT) N(EXP_IEEE)
        N(LOG_CLAMPED) N(LOG_IEEE) N(RECIP_CLAMPED) N(RECIP_FF) N(RECIP_IEEE) N(RECIPSQRT_CLAMPED) N(RECIPSQRT_FF)
        N(RECIPSQRT_IEEE) N(SQRT_IEEE) N(FLT_TO_INT) N(INT_TO_FLT) N(UINT_TO_FLT) N(SIN) N(COS) N(ASHR_INT)
        N(LSHR_INT) N(LSHL_INT) N(MULLO_INT) N(MULHI_INT) N(MULLO_UINT) N(MULHI_UINT) N(RECIP_INT) N(RECIP_UINT)
        N(FLT_TO_UINT)
#undef N
        default:
            return nullptr;
    }
}
}  // namespace r600
