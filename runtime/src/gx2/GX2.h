// Vendored from Cemu (src/Cafe/OS/libs/gx2/GX2.h + the register-base defines of src/Cafe/HW/Latte/Core/LattePM4.h),
// MPL-2.0. Common GX2 defines shared by the ported GX2 files. Cemu's legacy gx2Export_* declarations are dropped
// (those functions are typed GX2:: functions here, see the individual headers).
#pragma once
#include "../compat/cemu.h"
#include "../latte/isa/LatteConst.h"
#include "../latte/isa/LatteReg.h"
#include "GX2_Command.h"

// base defines for GX2
#define GX2_TRUE	1
#define GX2_FALSE	0
#define GX2_ENABLE	1
#define GX2_DISABLE	0

// register bases (LattePM4.h)
#ifndef LATTE_REG_BASE_CONFIG
#define LATTE_REG_BASE_CONFIG		0x2000
#define LATTE_REG_BASE_CONTEXT		0xA000
#define LATTE_REG_BASE_ALU_CONST	0xC000
#define LATTE_REG_BASE_LOOP_CONST	0xF880
#define LATTE_REG_BASE_RESOURCE		0xE000
#define LATTE_REG_BASE_SAMPLER		0xF000
#endif

// Cemu's placeholder address for scan buffers / uninitialized surfaces (MMU.h)
#ifndef MEMORY_TILINGAPERTURE_AREA_ADDR
#define MEMORY_TILINGAPERTURE_AREA_ADDR		(0xE8000000)
#endif

// write gather / command queue
#define GX2_COMMAND_RING_BUFFER_SIZE	(64*1024*1024) // 64MB

// color/depth buffers
#define GX2_SCAN_TARGET_TV				1
#define GX2_SCAN_TARGET_TV_RIGH			2
#define GX2_SCAN_TARGET_DRC_FIRST		4
#define GX2_SCAN_TARGET_DRC_SECOND		8

// special state
#define GX2_SPECIAL_STATE_COUNT			9

#include "GX2_Surface.h"
