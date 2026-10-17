// Vendored from Cemu (src/Cafe/OS/libs/gx2/GX2_Surface.h), MPL-2.0. GX2Surface / GX2ColorBuffer / GX2DepthBuffer.
#pragma once
#include "../compat/cemu.h"
#include "../latte/isa/LatteReg.h"
#include "../latte/addrlib/LatteAddrLib.h"

// todo - move into GX2 namespace

struct GX2Surface
{
	/* +0x000 */ betype<Latte::E_DIM> dim;
	/* +0x004 */ uint32be width;
	/* +0x008 */ uint32be height;
	/* +0x00C */ uint32be depth;
	/* +0x010 */ uint32be numLevels; // number of mipmap levels including base image. Should be at least 1
	/* +0x014 */ betype<Latte::E_GX2SURFFMT> format;
	/* +0x018 */ uint32be aa; // anti-aliasing mode
	/* +0x01C */ uint32be resFlag; // GX2_RESFLAG_* and GX2R_RESFLAG_*
	/* +0x020 */ uint32be imageSize;
	/* +0x024 */ uint32be imagePtr;
	/* +0x028 */ uint32be mipSize;
	/* +0x02C */ uint32be mipPtr;
	/* +0x030 */ betype<Latte::E_GX2TILEMODE> tileMode;
	/* +0x034 */ uint32be swizzle;
	/* +0x038 */ uint32be alignment;
	/* +0x03C */ uint32be pitch;
	/* +0x040 */ uint32be mipOffset[13];
}; // size: 0x74

static_assert(sizeof(betype<Latte::E_DIM>) == 4);
static_assert(sizeof(betype<Latte::E_GX2TILEMODE>) == 4);
static_assert(sizeof(GX2Surface) == 0x74);

// color and depth buffer

struct GX2ColorBuffer
{
	/* +0x00 */ GX2Surface		surface;
	/* +0x74 */ uint32be		viewMip;
	/* +0x78 */ uint32be		viewFirstSlice;
	/* +0x7C */ uint32be		viewNumSlices;
	/* +0x80 */ MEMPTR<void>	auxData;
	/* +0x84 */ uint32be		auxSize2;
	/* +0x88 */ uint32be		reg_size; // CB_COLOR*_SIZE
	/* +0x8C */ uint32be		reg_info; // CB_COLOR*_INFO
	/* +0x90 */ uint32be		reg_view; // CB_COLOR*_VIEW
	/* +0x94 */ uint32be		reg_mask; // CB_COLOR*_MASK
	/* +0x98 */ uint32be		reg4; // ?
};

static_assert(sizeof(GX2ColorBuffer) == 0x9C);

struct GX2DepthBuffer
{
	/* +0x00 */ GX2Surface		surface;
	/* +0x74 */ uint32be		viewMip;
	/* +0x78 */ uint32be		viewFirstSlice;
	/* +0x7C */ uint32be		viewNumSlices;
	/* +0x80 */ MEMPTR<void>	hiZPtr;
	/* +0x84 */ uint32be		hiZSize;
	/* +0x88 */ float32be		clearDepth;
	/* +0x8C */ uint32be		clearStencil;
	/* +0x90 */ uint32be		reg_size;
	/* +0x94 */ uint32be		reg_view;
	/* +0x98 */ uint32be		reg_base;
	/* +0x9C */ uint32be		reg_htile_surface;
	/* +0xA0 */ uint32be		reg_prefetch_limit;
	/* +0xA4 */ uint32be		reg_preload_control;
	/* +0xA8 */ uint32be		reg_poly_offset_db_fmt_cntl;
};

static_assert(sizeof(GX2DepthBuffer) == 0xAC);

namespace GX2
{
	void GX2CalculateSurfaceInfo(GX2Surface* surfacePtr, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* pSurfOut);

	Latte::E_ENDIAN_SWAP GetSurfaceFormatSwapMode(Latte::E_GX2SURFFMT fmt);
	uint32 GetSurfaceColorBufferExportFormat(Latte::E_GX2SURFFMT fmt);

	void GX2CalcSurfaceSizeAndAlignment(GX2Surface* surface);
	void GX2CalcDepthBufferHiZInfo(GX2DepthBuffer* depthBuffer, uint32be* sizeOut, uint32be* alignOut);
	void GX2CalcColorBufferAuxInfo(GX2ColorBuffer* colorBuffer, uint32be* sizeOut, uint32be* alignOut);
	void GX2SetSurfaceSwizzle(GX2Surface* surface, uint32 newSwizzle);

	// GX2_RenderTarget.cpp
	void GX2InitColorBufferRegs(GX2ColorBuffer* colorBuffer);
	void GX2InitDepthBufferRegs(GX2DepthBuffer* depthBuffer);
	void GX2SetColorBuffer(GX2ColorBuffer* colorBuffer, uint32 targetIndex);
	void GX2SetDepthBuffer(GX2DepthBuffer* depthBuffer);
};
