// R600/R700 ("Latte", GPU7) shader program decoder: control-flow program, ALU clauses (with
// instruction-group slot assignment and literals), TEX/VTX clauses and exports, into a small IR
// that msl.cpp turns into Metal. Encodings per AMD's R600/R700 ISA documents; Cemu's
// LatteDecompiler was used as a reference for the GPU7-specific details.
#pragma once
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace r600 {
// CF opcodes (CF_WORD1 bits 23..29)
enum Cf : uint8_t {
    CF_NOP = 0x00, CF_TEX = 0x01, CF_VTX = 0x02, CF_VTX_TC = 0x03, CF_LOOP_START = 0x04, CF_LOOP_END = 0x05,
    CF_LOOP_START_DX10 = 0x06, CF_LOOP_START_NO_AL = 0x07, CF_LOOP_CONTINUE = 0x08, CF_LOOP_BREAK = 0x09,
    CF_JUMP = 0x0A, CF_PUSH = 0x0B, CF_PUSH_ELSE = 0x0C, CF_ELSE = 0x0D, CF_POP = 0x0E, CF_POP_JUMP = 0x0F,
    CF_POP_PUSH = 0x10, CF_POP_PUSH_ELSE = 0x11, CF_CALL = 0x12, CF_CALL_FS = 0x13, CF_RETURN = 0x14,
    CF_EMIT_VERTEX = 0x15, CF_EMIT_CUT_VERTEX = 0x16, CF_CUT_VERTEX = 0x17, CF_KILL = 0x18,
    CF_MEM_STREAM0 = 0x20, CF_MEM_STREAM1 = 0x21, CF_MEM_STREAM2 = 0x22, CF_MEM_STREAM3 = 0x23,
    CF_MEM_SCRATCH = 0x24, CF_MEM_REDUCTION = 0x25, CF_MEM_RING = 0x26, CF_EXPORT = 0x27, CF_EXPORT_DONE = 0x28,
    // ALU clauses (CF_ALU_WORD1 bits 26..29), stored with 0x80 set to keep them apart
    CF_ALU = 0x88, CF_ALU_PUSH_BEFORE = 0x89, CF_ALU_POP_AFTER = 0x8A, CF_ALU_POP2_AFTER = 0x8B,
    CF_ALU_CONTINUE = 0x8D, CF_ALU_BREAK = 0x8E, CF_ALU_ELSE_AFTER = 0x8F,
};

// ALU OP2 opcodes (ALU_WORD1_OP2 bits 7..17)
enum Op2 : uint16_t {
    ADD = 0x00, MUL = 0x01, MUL_IEEE = 0x02, MAX = 0x03, MIN = 0x04, MAX_DX10 = 0x05, MIN_DX10 = 0x06,
    SETE = 0x08, SETGT = 0x09, SETGE = 0x0A, SETNE = 0x0B, SETE_DX10 = 0x0C, SETGT_DX10 = 0x0D, SETGE_DX10 = 0x0E,
    SETNE_DX10 = 0x0F, FRACT = 0x10, TRUNC = 0x11, CEIL = 0x12, RNDNE = 0x13, FLOOR = 0x14, MOVA = 0x15,
    MOVA_FLOOR = 0x16, MOVA_INT = 0x18, MOV = 0x19, NOP = 0x1A, PRED_SETGT_UINT = 0x1E, PRED_SETGE_UINT = 0x1F,
    PRED_SETE = 0x20, PRED_SETGT = 0x21, PRED_SETGE = 0x22, PRED_SETNE = 0x23, PRED_SET_INV = 0x24,
    PRED_SET_POP = 0x25, PRED_SET_CLR = 0x26, PRED_SET_RESTORE = 0x27, PRED_SETE_PUSH = 0x28,
    PRED_SETGT_PUSH = 0x29, PRED_SETGE_PUSH = 0x2A, PRED_SETNE_PUSH = 0x2B, KILLE = 0x2C, KILLGT = 0x2D,
    KILLGE = 0x2E, KILLNE = 0x2F, AND_INT = 0x30, OR_INT = 0x31, XOR_INT = 0x32, NOT_INT = 0x33, ADD_INT = 0x34,
    SUB_INT = 0x35, MAX_INT = 0x36, MIN_INT = 0x37, MAX_UINT = 0x38, MIN_UINT = 0x39, SETE_INT = 0x3A,
    SETGT_INT = 0x3B, SETGE_INT = 0x3C, SETNE_INT = 0x3D, SETGT_UINT = 0x3E, SETGE_UINT = 0x3F,
    KILLGT_UINT = 0x40, KILLGE_UINT = 0x41, PRED_SETE_INT = 0x42, PRED_SETGT_INT = 0x43, PRED_SETGE_INT = 0x44,
    PRED_SETNE_INT = 0x45, KILLE_INT = 0x46, KILLGT_INT = 0x47, KILLGE_INT = 0x48, KILLNE_INT = 0x49,
    DOT4 = 0x50, DOT4_IEEE = 0x51, CUBE = 0x52, MAX4 = 0x53, MOVA_GPR_INT = 0x60, EXP_IEEE = 0x61,
    LOG_CLAMPED = 0x62, LOG_IEEE = 0x63, RECIP_CLAMPED = 0x64, RECIP_FF = 0x65, RECIP_IEEE = 0x66,
    RECIPSQRT_CLAMPED = 0x67, RECIPSQRT_FF = 0x68, RECIPSQRT_IEEE = 0x69, SQRT_IEEE = 0x6A, FLT_TO_INT = 0x6B,
    INT_TO_FLT = 0x6C, UINT_TO_FLT = 0x6D, SIN = 0x6E, COS = 0x6F, ASHR_INT = 0x70, LSHR_INT = 0x71,
    LSHL_INT = 0x72, MULLO_INT = 0x73, MULHI_INT = 0x74, MULLO_UINT = 0x75, MULHI_UINT = 0x76, RECIP_INT = 0x77,
    RECIP_UINT = 0x78, FLT_TO_UINT = 0x79,
};
// ALU OP3 opcodes (ALU_WORD1_OP3 bits 13..17)
enum Op3 : uint16_t {
    MUL_LIT = 0x0C, MUL_LIT_M2 = 0x0D, MUL_LIT_M4 = 0x0E, MUL_LIT_D2 = 0x0F, MULADD = 0x10, MULADD_M2 = 0x11,
    MULADD_M4 = 0x12, MULADD_D2 = 0x13, MULADD_IEEE = 0x14, MULADD_IEEE_M2 = 0x15, MULADD_IEEE_M4 = 0x16,
    MULADD_IEEE_D2 = 0x17, CNDE = 0x18, CNDGT = 0x19, CNDGE = 0x1A, CNDE_INT = 0x1C, CNDGT_INT = 0x1D,
    CNDGE_INT = 0x1E,
};
// TEX clause opcodes (TEX_WORD0 bits 0..4)
enum Tex : uint8_t {
    TEX_VFETCH = 0x00, TEX_SEMANTIC = 0x01, TEX_MEM = 0x02, TEX_LD = 0x03, TEX_GET_RESINFO = 0x04,
    TEX_GET_NUM_SAMPLES = 0x05, TEX_GET_LOD = 0x06, TEX_GET_GRADIENTS_H = 0x07, TEX_GET_GRADIENTS_V = 0x08,
    TEX_SET_GRADIENTS_H = 0x0B, TEX_SET_GRADIENTS_V = 0x0C, TEX_PASS = 0x0D, TEX_SET_CUBEMAP_INDEX = 0x0E,
    TEX_FETCH4 = 0x0F, TEX_SAMPLE = 0x10, TEX_SAMPLE_L = 0x11, TEX_SAMPLE_LB = 0x12, TEX_SAMPLE_LZ = 0x13,
    TEX_SAMPLE_G = 0x14, TEX_SAMPLE_G_L = 0x15, TEX_SAMPLE_G_LB = 0x16, TEX_SAMPLE_G_LZ = 0x17, TEX_SAMPLE_C = 0x18,
    TEX_SAMPLE_C_L = 0x19, TEX_SAMPLE_C_LB = 0x1A, TEX_SAMPLE_C_LZ = 0x1B, TEX_SAMPLE_C_G = 0x1C,
    TEX_SAMPLE_C_G_L = 0x1D, TEX_SAMPLE_C_G_LB = 0x1E, TEX_SAMPLE_C_G_LZ = 0x1F,
};

// ALU source selects
constexpr uint16_t SRC_KCACHE0 = 128, SRC_KCACHE1 = 160, SRC_0 = 248, SRC_1 = 249, SRC_1_INT = 250,
                   SRC_M_1_INT = 251, SRC_0_5 = 252, SRC_LITERAL = 253, SRC_PV = 254, SRC_PS = 255, SRC_CFILE = 256;

struct AluSrc {
    uint16_t sel = 0;
    uint8_t chan = 0;
    bool neg = false, abs = false, rel = false;
};

struct AluInst {
    bool op3 = false;
    uint16_t op = 0;
    AluSrc src[3];
    uint8_t nsrc = 0;
    uint8_t dst_gpr = 0, dst_chan = 0, omod = 0, index_mode = 0, unit = 0;  // unit: 0..3 xyzw, 4 trans
    bool dst_rel = false, write = false, clamp = false, update_exec = false, update_pred = false;
};

struct AluGroup {
    std::vector<AluInst> insts;
    uint32_t literal[4] = {};
};

struct TexInst {
    uint8_t op = 0;
    uint8_t resource = 0, sampler = 0, src_gpr = 0, dst_gpr = 0;
    bool src_rel = false, dst_rel = false;
    uint8_t src_sel[4] = {0, 1, 2, 3}, dst_sel[4] = {0, 1, 2, 3};
    bool coord_norm[4] = {true, true, true, true};
    int8_t offset[3] = {};
    int8_t lod_bias = 0;  // 7-bit signed, 1/2 units? (as Cemu: raw value)
    // VTX fetch (TEX_VFETCH / semantic in fetch shaders)
    uint8_t fetch_type = 0, data_format = 0, num_format = 0, endian = 0, semantic = 0, src_sel_x = 0, mega_fetch = 0;
    bool format_signed = false, srf_mode = false, use_const_fields = false;
    uint32_t vtx_offset = 0;
    // MEM read
    uint32_t mem_array_base = 0, mem_array_size = 0;
};

struct CfInst {
    uint8_t op = CF_NOP;
    uint32_t index = 0;       // CF slot
    uint32_t addr = 0, count = 0;
    uint8_t cond = 0, pop_count = 0, cf_const = 0, call_count = 0;
    bool eop = false, barrier = false, wqm = false, valid_pixel = false;
    // ALU clause
    uint8_t kc_bank[2] = {}, kc_mode[2] = {};
    uint32_t kc_addr[2] = {};
    bool alt_const = false;
    std::vector<AluGroup> groups;
    // TEX / VTX clause
    std::vector<TexInst> tex;
    // export / mem write
    uint8_t exp_type = 0, exp_gpr = 0, exp_index_gpr = 0, exp_elem_size = 0, exp_burst = 0, exp_comp_mask = 0;
    bool exp_rel = false;
    uint32_t exp_array_base = 0, exp_array_size = 0;
    uint8_t exp_sel[4] = {0, 1, 2, 3};
    int depth = 0;  // predicate stack depth before this instruction
};

struct Program {
    std::vector<CfInst> cf;                        // main program
    std::map<uint32_t, std::vector<CfInst>> subs;  // CALL targets
    bool ok = true;
    std::string error;
    // analysis
    uint32_t gpr_used[4] = {};  // bitmask of GPRs referenced (128 bits)
    bool uses_rel_gpr = false, uses_ar = false, uses_kcache = false, uses_cfile = false, uses_pred_stack = false;
    bool uses_kill = false, uses_cube = false, uses_gradients = false, calls_fs = false;
    int max_depth = 0;
    uint32_t kcache_banks = 0;  // bitmask of UBO slots read via kcache
    uint32_t tex_used = 0;       // bitmask of texture resource slots
    uint32_t tex_compare = 0;    // slots sampled with SAMPLE_C*
    uint32_t sampler_used = 0;
    std::map<uint32_t, uint32_t> exports;  // (type<<16 | array_base) -> component mask
    void use_gpr(uint32_t g) {
        if (g < 128) gpr_used[g / 32] |= 1u << (g % 32);
    }
    bool gpr(uint32_t g) const { return g < 128 && (gpr_used[g / 32] >> (g % 32)) & 1; }
};

// words: the program as the GPU reads it (little-endian u32s)
Program parse(const uint32_t* words, uint32_t nwords);
const char* op2_name(uint16_t op);
bool is_trans_only(uint16_t op2);
bool is_reduction(const AluInst& a);
}  // namespace r600
