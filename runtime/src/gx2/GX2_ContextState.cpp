// Vendored from Cemu (src/Cafe/OS/libs/gx2/GX2_ContextState.cpp), MPL-2.0. Context state shadowing, default state and
// the context-restore display list; packets are identical to Cemu. The legacy gx2Export_* functions are converted to
// typed GX2:: functions with the same argument order, and the whole file lives in namespace GX2 (Cemu: global).
// Dropped: Cemu's Binding of Isaac workaround in GX2SetContextState (keyed on RPX hash 0x6BCD618E, never BotW).
#include "../compat/cemu_export.h"
#include "../latte/isa/LatteReg.h"
#include "../latte/isa/RegDefines.h"
#include "GX2.h"
#include "GX2_Command.h"
#include "GX2_Shader.h"
#include "GX2_State.h"

namespace GX2
{

#define GPU7_REG_AREA_SIZE_CONFIG_REG	0xB00
#define GPU7_REG_AREA_SIZE_CONTEXT_REG	0x400
#define GPU7_REG_AREA_SIZE_ALU_CONST	0x800
#define GPU7_REG_AREA_SIZE_LOOP_CONST	0x60
#define GPU7_REG_AREA_SIZE_RESOURCE		0xD9E
#define GPU7_REG_AREA_SIZE_SAMPLER		0xA2 // (guessed)

#define _GX2_CALC_SHADOWMEM_NUM_U32(__v) (((((__v)*4)+0xFF)&~0xFF)/4)

MPTR gx2CurrentContextStateMPTR = MPTR_NULL;

typedef struct  
{
	uint32 regOffset;
	uint32 regCount;
}GX2RegLoadPktEntry_t;

GX2RegLoadPktEntry_t aluConst_loadPktEntries[1] = // base: 0xC000
{
	{0, 0x800},
};

GX2RegLoadPktEntry_t loopConst_loadPktEntries[1] = // base: 0xF880
{
	{0, 0x60},
};

GX2RegLoadPktEntry_t samplerReg_loadPktEntries[3] = // base: 0xF000
{
	{0, 0x36},
	{0x36, 0x36},
	{0x6C, 0x36},
};

GX2RegLoadPktEntry_t configReg_loadPktEntries[0xF] = // base: 0x2000
{
	{0x300, 0x6},
	{0x900, 0x48},
	{0x980, 0x48},
	{0xA00, 0x48},
	{0x310, 0xC},
	{0x542, 0x1},
	{0x235, 0x1},
	{0x232, 0x2},
	{0x23A, 0x1},
	{0x256, 0x1},
	{0x60C, 0x1},
	{0x5C5, 0x1},
	{0x2C8, 0x1},
	{0x363, 0x1},
	{0x404, 0x2}
};

GX2RegLoadPktEntry_t contextReg_loadPktEntries[0x2D] = // base: 0xA000
{
	{0x0, 0x2},
	{0x3, 0x3},
	{0xA, 0x4},
	{0x10, 0x38},
	{0x50, 0x34},
	{0x8E, 0x4},
	{0x94, 0x40},
	{0x100, 0x9},
	{0x10C, 0x3},
	{0x10F, 0x60},
	{0x185, 0xA},
	{0x191, 0x27},
	{0x1E0, 0x9},
	{0x200, 0x1},
	{0x202, 0x7},
	{0xE0, 0x20},
	{0x210, 0x29},
	{0x250, 0x34},
	{0x290, 0x1},
	{0x292, 0x2},
	{0x2A1, 0x1},
	{0x2A5, 0x1},
	{0x2A8, 0x2},
	{0x2AC, 0x3},
	{0x2CA, 0x1},
	{0x2CC, 0x1},
	{0x2CE, 0x1},
	{0x300, 0x9},
	{0x30C, 0x1},
	{0x312, 0x1},
	{0x316, 0x2},
	{0x343, 0x2},
	{0x349, 0x3},
	{0x34C, 0x2},
	{0x351, 0x1},
	{0x37E, 0x6},
	{0x2B4, 0x3},
	{0x2B8, 0x3},
	{0x2BC, 0x3},
	{0x2C0, 0x3},
	{0x2C8, 0x1},
	{0x29B, 0x1},
	{0x8C, 0x1},
	{0xD5, 0x1},
	{0x284, 0xC}
};

GX2RegLoadPktEntry_t resourceReg_loadPktEntries[9] = // base: 0xE000
{
	{0, 0x70},	// ps tex
	{0x380, 0x70},
	{0x460, 0x70}, // vs tex
	{0x7E0, 0x70},
	{0x8B9, 0x7},
	{0x8C0, 0x70},
	{0x930, 0x70}, // gs tex
	{0xCB0, 0x70},
	{0xD89, 0x7}
};

typedef struct  
{
	// Hardware view of context state (register areas)
	uint32 areaConfigReg[_GX2_CALC_SHADOWMEM_NUM_U32(GPU7_REG_AREA_SIZE_CONFIG_REG)];
	uint32 areaContextReg[_GX2_CALC_SHADOWMEM_NUM_U32(GPU7_REG_AREA_SIZE_CONTEXT_REG)];
	uint32 areaALUConst[_GX2_CALC_SHADOWMEM_NUM_U32(GPU7_REG_AREA_SIZE_ALU_CONST)];
	uint32 areaLoopConst[_GX2_CALC_SHADOWMEM_NUM_U32(GPU7_REG_AREA_SIZE_LOOP_CONST)];
	uint32 areaResource[_GX2_CALC_SHADOWMEM_NUM_U32(GPU7_REG_AREA_SIZE_RESOURCE)];
	uint32 areaSampler[_GX2_CALC_SHADOWMEM_NUM_U32(GPU7_REG_AREA_SIZE_SAMPLER)];
}GX2HwContextState_t;

typedef struct  
{
	GX2HwContextState_t hwContext;
	uint32 enableProfling;
	/* + 0x9804 */ uint32be loadDL_size;
	uint8 ukn9808[0x3FC-4];
	uint8 ukn9C00[0x200];
	/* +0x9E00 */ uint8 loadDL_buffer[0x300]; // this displaylist caches the IT_LOAD_* commands for swapping out context
}GX2ContextState_t;

static_assert(offsetof(GX2ContextState_t, loadDL_size) == 0x9804);
static_assert(sizeof(GX2ContextState_t) == 0xA100);

uint32 _GX2Context_CalcShadowMemSize(uint32 regCount)
{
	return (regCount*4+0xFF)&~0xFF;
}

uint32 _GX2Context_CalcStateSize()
{
	uint32 contextStateSize = 0;
	contextStateSize += _GX2Context_CalcShadowMemSize(GPU7_REG_AREA_SIZE_CONFIG_REG);
	contextStateSize += _GX2Context_CalcShadowMemSize(GPU7_REG_AREA_SIZE_CONTEXT_REG);
	contextStateSize += _GX2Context_CalcShadowMemSize(GPU7_REG_AREA_SIZE_ALU_CONST);
	contextStateSize += _GX2Context_CalcShadowMemSize(GPU7_REG_AREA_SIZE_LOOP_CONST);
	contextStateSize += _GX2Context_CalcShadowMemSize(GPU7_REG_AREA_SIZE_RESOURCE);
	contextStateSize += _GX2Context_CalcShadowMemSize(GPU7_REG_AREA_SIZE_SAMPLER);
	return contextStateSize;
}

void _GX2Context_CreateLoadDL()
{
	GX2::GX2ReserveCmdSpace(3);
	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_CONTEXT_CONTROL, 2));
	gx2WriteGather_submitU32AsBE(0x80000077);
	gx2WriteGather_submitU32AsBE(0x80000077);
}

void _GX2Context_WriteCmdDisableStateShadowing()
{
	GX2::GX2ReserveCmdSpace(3);
	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_CONTEXT_CONTROL, 2));
	gx2WriteGather_submitU32AsBE(0x80000000);
	gx2WriteGather_submitU32AsBE(0x80000000);
}

void _GX2Context_cmdLoad(void* gx2ukn, uint32 pm4Header, MPTR physAddrRegArea, uint32 waitForIdle, uint32 numRegOffsetEntries, GX2RegLoadPktEntry_t* regOffsetEntries)
{
	GX2::GX2ReserveCmdSpace(3 + numRegOffsetEntries*2);
	gx2WriteGather_submitU32AsBE(pm4Header);
	gx2WriteGather_submitU32AsBE(physAddrRegArea);
	gx2WriteGather_submitU32AsBE(waitForIdle);
	for(uint32 i=0; i<numRegOffsetEntries; i++)
	{
		gx2WriteGather_submitU32AsBE(regOffsetEntries[i].regOffset); // regOffset
		gx2WriteGather_submitU32AsBE(regOffsetEntries[i].regCount); // regCount
	}
}

#define __cmdStateLoad(__gx2State, __pm4Command, __regArea, __waitForIdle, __regLoadPktEntries) _GX2Context_cmdLoad(NULL, pm4HeaderType3(__pm4Command, 2+sizeof(__regLoadPktEntries)/sizeof(__regLoadPktEntries[0])*2), memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(__regArea)), __waitForIdle, sizeof(__regLoadPktEntries)/sizeof(__regLoadPktEntries[0]), __regLoadPktEntries)

void _GX2Context_WriteCmdRestoreState(GX2ContextState_t* gx2ContextState, uint32 ukn)
{
	MPTR physAddrContextState = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(gx2ContextState));
	_GX2Context_CreateLoadDL();
	__cmdStateLoad(NULL, IT_LOAD_CONFIG_REG, gx2ContextState->hwContext.areaConfigReg, 0x80000000, configReg_loadPktEntries);
	__cmdStateLoad(NULL, IT_LOAD_CONTEXT_REG, gx2ContextState->hwContext.areaContextReg, 0, contextReg_loadPktEntries);
	__cmdStateLoad(NULL, IT_LOAD_ALU_CONST, gx2ContextState->hwContext.areaALUConst, 0, aluConst_loadPktEntries);
	__cmdStateLoad(NULL, IT_LOAD_LOOP_CONST, gx2ContextState->hwContext.areaLoopConst, 0, loopConst_loadPktEntries);
	__cmdStateLoad(NULL, IT_LOAD_RESOURCE, gx2ContextState->hwContext.areaResource, 0, resourceReg_loadPktEntries);
	__cmdStateLoad(NULL, IT_LOAD_SAMPLER, gx2ContextState->hwContext.areaSampler, 0, samplerReg_loadPktEntries);
}

void GX2SetDefaultState()
{
	GX2::GX2ReserveCmdSpace(0x100);

	GX2::GX2SetShaderModeEx(GX2::GX2_SHADER_MODE::UNIFORM_REGISTER, 48, 64, 0, 0, 200, 192);

	Latte::LATTE_PA_CL_VTE_CNTL reg{};
	reg.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true);
	reg.set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
	reg.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true);
	reg.set_VTX_W0_FMT(true);
	gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
		Latte::REGADDR::PA_CL_VTE_CNTL - 0xA000,
		reg);

	uint32 stencilTestEnable = GX2_FALSE;
	uint32 backStencilEnable = GX2_FALSE;
	uint32 frontStencilFunc = 0;
	uint32 frontStencilZPass = 0;
	uint32 frontStencilZFail = 0;
	uint32 frontStencilFail = 0; 
	uint32 backStencilFunc = 0;
	uint32 backStencilZPass = 0;
	uint32 backStencilZFail = 0;
	uint32 backStencilFail = 0;

	uint32 depthControlReg = 0;
	// depth stuff
	depthControlReg |= (1<<1);
	depthControlReg |= (1<<2);
	depthControlReg |= ((1&7)<<4);
	// stencil stuff
	depthControlReg |= ((stencilTestEnable&1)<<0);
	depthControlReg |= ((backStencilEnable&1)<<7);
	depthControlReg |= ((frontStencilFunc&7)<<8);
	depthControlReg |= ((frontStencilZPass&7)<<14);
	depthControlReg |= ((frontStencilZFail&7)<<17);
	depthControlReg |= ((frontStencilFail&7)<<11);
	depthControlReg |= ((backStencilFunc&7)<<20);
	depthControlReg |= ((backStencilZPass&7)<<26);
	depthControlReg |= ((backStencilZFail&3)<<29);
	depthControlReg |= ((backStencilFail&7)<<23);

	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1+1));
	gx2WriteGather_submitU32AsBE(Latte::REGADDR::DB_DEPTH_CONTROL-0xA000);
	gx2WriteGather_submitU32AsBE(depthControlReg);

	GX2::GX2SetAlphaTest(GX2_DISABLE, GX2::GX2_ALPHAFUNC::LESS, 0.0f);
	GX2::GX2SetPolygonControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW, GX2_DISABLE, GX2_DISABLE, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE::UKN0, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES, GX2_DISABLE, GX2_DISABLE, GX2_DISABLE);
	GX2::GX2SetPolygonOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	GX2::GX2SetPrimitiveRestartIndex(0xffffffff);
	GX2::GX2SetTargetChannelMasks(0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF);
	GX2::GX2SetBlendConstantColor(0.0f, 0.0f, 0.0f, 0.0f);
	GX2::GX2SetPointSize(1.0f, 1.0f);
	GX2::GX2SetPointLimits(1.0f, 1.0f);
	GX2::GX2SetColorControl(GX2::GX2_LOGICOP::COPY, GX2_DISABLE, GX2_DISABLE, GX2_ENABLE);
	GX2::GX2SetRasterizerClipControlEx(true, true, false);

	// Set clear depth to 1.0 (workaround for Darksiders 2. Investigate whether actual GX2 driver also sets this)
	float depth = 1.0;
	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1+1));
	gx2WriteGather_submitU32AsBE(mmDB_DEPTH_CLEAR - 0xA000);
	gx2WriteGather_submitU32AsBE(*(uint32*)&depth); // depth (as float)

	// reset HLE special states
	for (sint32 i = 0; i <= GX2_SPECIAL_STATE_COUNT; i++)
	{
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_SPECIAL_STATE, 2));
		gx2WriteGather_submitU32AsBE(i); // state id
		gx2WriteGather_submitU32AsBE(0); // disable
	}
}

void _GX2ContextCreateRestoreStateDL(GX2ContextState_t* gx2ContextState)
{
	// begin display list
	cemu_assert_debug(!GX2::GX2GetDisplayListWriteStatus()); // must not already be writing to a display list
	GX2::GX2BeginDisplayList((void*)gx2ContextState->loadDL_buffer, sizeof(gx2ContextState->loadDL_buffer));
	_GX2Context_WriteCmdRestoreState(gx2ContextState, 0);
	uint32 displayListSize = GX2::GX2EndDisplayList((void*)gx2ContextState->loadDL_buffer);
	gx2ContextState->loadDL_size = displayListSize;
}

// gx2Export_GX2SetupContextStateEx: r3 = contextState, r4 = enableProfiling
void GX2SetupContextStateEx(GX2ContextState_t* gx2ContextState, uint32 enableProfiling)
{
	cemu_assert_debug(enableProfiling == 0 || enableProfiling == 1);

	uint32 hwContextSize = _GX2Context_CalcStateSize();
	if( hwContextSize != sizeof(GX2HwContextState_t) )
		assert_dbg();
	if( sizeof(GX2HwContextState_t) != 0x9800 )
		assert_dbg(); // GX2 HW context size mismatch
	if( sizeof(GX2ContextState_t) != 0xA100 )
		assert_dbg(); // GX2 context size mismatch

	memset(gx2ContextState, 0x00, sizeof(GX2ContextState_t));
	gx2ContextState->enableProfling = _swapEndianU32(enableProfiling&1);
	_GX2Context_WriteCmdRestoreState(gx2ContextState, 1);

	gx2CurrentContextStateMPTR = memory_getVirtualOffsetFromPointer(gx2ContextState);

	_GX2Context_CreateLoadDL();
	GX2SetDefaultState();
	_GX2ContextCreateRestoreStateDL(gx2ContextState);
}

// gx2Export_GX2SetContextState: r3 = contextState (may be NULL)
void GX2SetContextState(GX2ContextState_t* gx2ContextState)
{
	if( gx2ContextState == nullptr )
	{
		// disable state shadowing
		_GX2Context_WriteCmdDisableStateShadowing();
		return;
	}
	// (Cemu's Binding of Isaac workaround branch dropped, see top of file)
	if (gx2ContextState->loadDL_size == 0)
	{
		_GX2Context_CreateLoadDL();
		_GX2Context_WriteCmdRestoreState(gx2ContextState, 0);
	}
	else
	{
		_GX2Context_CreateLoadDL();
		GX2::GX2CallDisplayList(memory_getVirtualOffsetFromPointer(gx2ContextState->loadDL_buffer), gx2ContextState->loadDL_size);
	}
	// set new context state
	gx2CurrentContextStateMPTR = memory_getVirtualOffsetFromPointer(gx2ContextState);
	// todo: Save/restore GX2 special state as well -> handle this by correctly emulating the state
}

// gx2Export_GX2GetContextStateDisplayList: r3 = contextState, r4 = displayListPtrOut, r5 = displayListSizeOut
void GX2GetContextStateDisplayList(GX2ContextState_t* gx2ContextState, uint32be* displayListPtrOut, uint32be* displayListSizeOut)
{
	*displayListPtrOut = memory_getVirtualOffsetFromPointer(gx2ContextState->loadDL_buffer);
	*displayListSizeOut = gx2ContextState->loadDL_size;
}

} // namespace GX2

CAFE_EXPORT(gx2, GX2SetDefaultState, GX2::GX2SetDefaultState);
CAFE_EXPORT(gx2, GX2SetupContextStateEx, GX2::GX2SetupContextStateEx);
CAFE_EXPORT(gx2, GX2SetContextState, GX2::GX2SetContextState);
CAFE_EXPORT(gx2, GX2GetContextStateDisplayList, GX2::GX2GetContextStateDisplayList);
