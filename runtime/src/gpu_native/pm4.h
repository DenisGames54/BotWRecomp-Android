// PM4 packet and Latte register constants used by the native GPU path. The packet set is what
// this runtime's GX2 HLE (runtime/src/gx2, packet layouts from Cemu) emits; addresses are guest
// addresses (physical == virtual in this runtime). Register numbers follow Cemu's RegDefines.h.
#pragma once
#include <stdint.h>

namespace gpu_native::pm4 {
enum Op : uint32_t {
    NOP = 0x10, SET_PREDICATION = 0x20, DRAW_INDEX_2 = 0x27, CONTEXT_CONTROL = 0x28, INDEX_TYPE = 0x2A,
    DRAW_INDEX_AUTO = 0x2D, DRAW_INDEX_IMMD = 0x2E, NUM_INSTANCES = 0x2F, INDIRECT_BUFFER_PRIV = 0x32,
    STRMOUT_BUFFER_UPDATE = 0x34, MEM_SEMAPHORE = 0x39, WAIT_REG_MEM = 0x3C, MEM_WRITE = 0x3D,
    SURFACE_SYNC = 0x43, EVENT_WRITE = 0x46, EVENT_WRITE_EOP = 0x47, LOAD_CONFIG_REG = 0x60,
    LOAD_CONTEXT_REG = 0x61, LOAD_ALU_CONST = 0x62, LOAD_BOOL_CONST = 0x63, LOAD_LOOP_CONST = 0x64,
    LOAD_RESOURCE = 0x65, LOAD_SAMPLER = 0x66, SET_CONFIG_REG = 0x68, SET_CONTEXT_REG = 0x69,
    SET_ALU_CONST = 0x6A, SET_BOOL_CONST = 0x6B, SET_LOOP_CONST = 0x6C, SET_RESOURCE = 0x6D,
    SET_SAMPLER = 0x6E, SET_CTL_CONST = 0x6F, STRMOUT_BASE_UPDATE = 0x72, SET_ALL_CONTEXTS = 0x74,
    // this runtime's HLE packets (Cemu LattePM4.h)
    HLE_COPY_SURFACE_NEW = 0xEE, HLE_SYNC_ASYNC_OPERATIONS = 0xEF, HLE_REQUEST_SWAP_BUFFERS = 0xF0,
    HLE_WAIT_FOR_FLIP = 0xF1, HLE_BOTTOM_OF_PIPE_CB = 0xF2, HLE_COPY_COLORBUFFER_TO_SCANBUFFER = 0xF3,
    HLE_CLEAR_COLOR_DEPTH_STENCIL = 0xF5, HLE_SAMPLE_TIMER = 0xF7, HLE_TRIGGER_SCANBUFFER_SWAP = 0xF8,
    HLE_SPECIAL_STATE = 0xF9, HLE_BEGIN_OCCLUSION_QUERY = 0xFA, HLE_END_OCCLUSION_QUERY = 0xFB,
};

// register file bases for the SET_* / LOAD_* packet families
constexpr uint32_t kConfigBase = 0x2000, kContextBase = 0xA000, kAluConstBase = 0xC000, kResourceBase = 0xE000,
                   kSamplerBase = 0xF000, kLoopConstBase = 0xF880, kBoolConstBase = 0xF380, kCtlConstBase = 0xF3FC;
constexpr uint32_t kRegCount = 0x10000;

inline uint32_t base_for(uint32_t op) {
    switch (op) {
        case SET_CONFIG_REG: case LOAD_CONFIG_REG: return kConfigBase;
        case SET_CONTEXT_REG: case SET_ALL_CONTEXTS: case LOAD_CONTEXT_REG: return kContextBase;
        case SET_ALU_CONST: case LOAD_ALU_CONST: return kAluConstBase;
        case SET_RESOURCE: case LOAD_RESOURCE: return kResourceBase;
        case SET_SAMPLER: case LOAD_SAMPLER: return kSamplerBase;
        case SET_LOOP_CONST: case LOAD_LOOP_CONST: return kLoopConstBase;
        case SET_BOOL_CONST: case LOAD_BOOL_CONST: return kBoolConstBase;
        case SET_CTL_CONST: return kCtlConstBase;
        default: return 0;
    }
}

// registers (absolute indices)
enum Reg : uint32_t {
    DB_DEPTH_SIZE = 0xA000, DB_DEPTH_VIEW = 0xA001, DB_DEPTH_BASE = 0xA003, DB_DEPTH_INFO = 0xA004,
    CB_COLOR0_BASE = 0xA010 /* full address (GX2 HLE layout, as Cemu), not >>8 */, CB_COLOR0_SIZE = 0xA018, CB_COLOR0_VIEW = 0xA020, CB_COLOR0_INFO = 0xA028,
    CB_TARGET_MASK = 0xA08E, CB_SHADER_MASK = 0xA08F, SX_ALPHA_TEST_CONTROL = 0xA104, CB_BLEND_RED = 0xA105,
    PA_CL_VPORT_XSCALE = 0xA10F, SPI_VS_OUT_CONFIG = 0xA1B1, SPI_PS_IN_CONTROL_0 = 0xA1B3,
    CB_BLEND0_CONTROL = 0xA1E0, DB_DEPTH_CONTROL = 0xA200, CB_COLOR_CONTROL = 0xA202, DB_SHADER_CONTROL = 0xA203,
    PA_CL_CLIP_CNTL = 0xA204, PA_SU_SC_MODE_CNTL = 0xA205, PA_CL_VTE_CNTL = 0xA206,
    SQ_PGM_START_PS = 0xA210, SQ_PGM_SIZE_PS = 0xA211, SQ_PGM_START_VS = 0xA216, SQ_PGM_SIZE_VS = 0xA217,
    SQ_PGM_START_GS = 0xA21B, SQ_PGM_SIZE_GS = 0xA21C, SQ_PGM_START_ES = 0xA220, SQ_PGM_SIZE_ES = 0xA221,
    SQ_PGM_START_FS = 0xA225, SQ_PGM_SIZE_FS = 0xA226, VGT_GS_MODE = 0xA290, VGT_DMA_INDEX_TYPE = 0xA29F,
    VGT_STRMOUT_EN = 0xA2AC, PA_SC_AA_CONFIG = 0xA301, PA_SC_AA_MASK = 0xA312, DB_RENDER_CONTROL = 0xA343,
    VGT_PRIMITIVE_TYPE = 0x2256, SQ_CONFIG = 0x2300,
    SQ_VTX_BASE_VTX_LOC = 0xF3FC, SQ_VTX_START_INST_LOC = 0xF3FD,
};

// SET_RESOURCE register offsets (from kResourceBase); 7 dwords per slot
constexpr uint32_t kResPsTex = 0x000, kResPsUbo = 0x250, kResVsTex = 0x460, kResVsUbo = 0x7E0, kResAttrib = 0x8C0,
                   kResGsTex = 0x930, kResGsUbo = 0xCB0;
constexpr uint32_t kTexPerStage = 18, kUboPerStage = 16, kAttribBuffers = 16;

inline uint32_t hdr_type(uint32_t h) { return h >> 30; }
inline uint32_t hdr_count(uint32_t h) { return ((h >> 16) & 0x3FFF) + 1; }  // type 3: body dwords; type 0: registers
inline uint32_t hdr_op(uint32_t h) { return (h >> 8) & 0xFF; }

const char* op_name(uint32_t op);
}  // namespace gpu_native::pm4
