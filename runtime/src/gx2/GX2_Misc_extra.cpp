// Vendored from Cemu (src/Cafe/OS/libs/gx2/GX2_Misc.cpp: GX2Invalidate; src/Cafe/OS/libs/gx2/GX2.cpp: GX2CalcTVSize,
// GX2CalcDRCSize), MPL-2.0. The rest of GX2.cpp / GX2_Misc.cpp (init, swap, TV/DRC setup, ...) is in gx2_core.cpp.
// The legacy gx2Export_* functions are converted to typed GX2:: functions with the same argument order.
//
// The last section holds the exports the game imports that Cemu does not implement at all. Cemu maps such imports to
// its "unsupported import" stub (PPCInterpreter_handleUnsupportedHLECall: log once, r3 = 0, return), so they are
// ported as exactly that: no packets, no state changes, 0 returned where the API has a return value.
#include "../compat/cemu_export.h"
#include "../latte/isa/LatteReg.h"
#include "GX2.h"
#include "GX2_Misc.h"
#include "GX2_Resource.h"
#include "GX2_Shader.h"
#include "GX2_State.h"
#include "GX2_Surface.h"

void LatteBufferCache_notifyDCFlush(uint32_t address, uint32_t size);  // vendored Cemu GPU

namespace GX2
{
	/****** GX2_Misc.cpp ******/

	void GX2Invalidate(GX2InvalidationFlag invalidationFlags, MPTR invalidationAddr, uint32 invalidationSize)
	{
		Latte::E_COHER_CNTL coherCntl{};

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_ATTRIB) || HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_TEXTURE))
	        coherCntl |= Latte::E_COHER_CNTL::TC_ACTION_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_UNIFORM_BLOCK))
	        coherCntl |= Latte::E_COHER_CNTL::TC_ACTION_ENA | Latte::E_COHER_CNTL::SH_ACTION_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_SHADER))
	        coherCntl |= Latte::E_COHER_CNTL::SH_ACTION_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_COLOR_BUFFER))
	    {
	        coherCntl |= Latte::E_COHER_CNTL::CB_ACTION_ENA | Latte::E_COHER_CNTL::CB_ALL_DEST_BASE_ENA;
	    }

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_DEPTH_BUFFER))
	        coherCntl |= Latte::E_COHER_CNTL::DB_ACTION_ENA | Latte::E_COHER_CNTL::DB_DEST_BASE_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_STREAM_OUT))
	    {
	        coherCntl |= Latte::E_COHER_CNTL::SX_ACTION_ENA
	                   | Latte::E_COHER_CNTL::SO0_DEST_BASE_ENA | Latte::E_COHER_CNTL::SO1_DEST_BASE_ENA
	                   | Latte::E_COHER_CNTL::SO2_DEST_BASE_ENA | Latte::E_COHER_CNTL::SO3_DEST_BASE_ENA;
	    }

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_EXPORT_BUFFER))
	    {
	        coherCntl |= Latte::E_COHER_CNTL::SX_ACTION_ENA
	                   | Latte::E_COHER_CNTL::DB_ACTION_ENA
	                   | Latte::E_COHER_CNTL::CB_ACTION_ENA
	                   | Latte::E_COHER_CNTL::TC_ACTION_ENA
	                   | Latte::E_COHER_CNTL::DEST_BASE_0_ENA;
	    }

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::CPU))
	        LatteBufferCache_notifyDCFlush(invalidationAddr, invalidationSize);

	    if (coherCntl == static_cast<Latte::E_COHER_CNTL>(0))
	        return; // no GPU-side invalidation

	    coherCntl |= Latte::E_COHER_CNTL::ENGINE_ME;

	    uint32 physicalAddr = memory_virtualToPhysical(invalidationAddr);
	    uint32 sizeUnits = (invalidationSize == 0xFFFFFFFFu) ? 0x00FFFFFFu : ((invalidationSize + 0xFF) >> 8);

	    GX2ReserveCmdSpace(5);
	    gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SURFACE_SYNC, 4));
	    gx2WriteGather_submitU32AsBE(static_cast<uint32>(coherCntl));
	    gx2WriteGather_submitU32AsBE(sizeUnits);
	    gx2WriteGather_submitU32AsBE(physicalAddr >> 8);
	    gx2WriteGather_submitU32AsBE(4u);
	}

	/****** GX2.cpp ******/

#define GX2_TV_RENDER_NONE			0
#define GX2_TV_RENDER_480			1
#define GX2_TV_RENDER_480_WIDE		2
#define GX2_TV_RENDER_720			3
#define GX2_TV_RENDER_720I			4
#define GX2_TV_RENDER_1080			5
#define GX2_TV_RENDER_COUNT			6

	struct
	{
		sint32 width;
		sint32 height;
	}tvScanBufferResolutions[GX2_TV_RENDER_COUNT] = {
	{0,0},
	{640,480},
	{854,480},
	{1280,720},
	{1280,720},
	{1920,1080}
	};

	void _GX2InitScanBuffer(GX2ColorBuffer* colorBuffer, sint32 width, sint32 height, Latte::E_GX2SURFFMT format)
	{
		colorBuffer->surface.resFlag = GX2_RESFLAG_USAGE_TEXTURE | GX2_RESFLAG_USAGE_COLOR_BUFFER;
		colorBuffer->surface.width = width;
		colorBuffer->surface.height = height;
		colorBuffer->viewFirstSlice = 0;
		colorBuffer->viewNumSlices = 1;
		colorBuffer->viewMip = 0;
		colorBuffer->surface.numLevels = 1;
		colorBuffer->surface.dim = Latte::E_DIM::DIM_2D;
		colorBuffer->surface.swizzle = 0;
		colorBuffer->surface.depth = 1;
		colorBuffer->surface.tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_GENERAL;
		colorBuffer->surface.format = format;
		colorBuffer->surface.mipPtr = MPTR_NULL;
		colorBuffer->surface.aa = 0;
		GX2::GX2CalcSurfaceSizeAndAlignment(&colorBuffer->surface);
		colorBuffer->surface.resFlag = GX2_RESFLAG_USAGE_TEXTURE | GX2_RESFLAG_USAGE_COLOR_BUFFER | GX2_RESFLAG_USAGE_SCAN_BUFFER;
	}

	// gx2Export_GX2CalcTVSize: r3 = tvRenderMode, r4 = format, r5 = bufferingMode, r6 = sizeOut, r7 = scaleNeededOut
	void GX2CalcTVSize(uint32 tvRenderMode, uint32 formatRaw, uint32 bufferingMode, MPTR outputSizeMPTR, MPTR outputScaleNeededMPTR)
	{
		Latte::E_GX2SURFFMT format = (Latte::E_GX2SURFFMT)formatRaw;

		cemu_assert(tvRenderMode < GX2_TV_RENDER_COUNT);

		uint32 width = tvScanBufferResolutions[tvRenderMode].width;
		uint32 height = tvScanBufferResolutions[tvRenderMode].height;

		GX2ColorBuffer colorBuffer;
		memset(&colorBuffer, 0, sizeof(GX2ColorBuffer));
		_GX2InitScanBuffer(&colorBuffer, width, height, format);

		uint32 imageSize = colorBuffer.surface.imageSize;
		uint32 alignment = colorBuffer.surface.alignment;

		uint32 alignmentPaddingSize = (alignment - (imageSize%alignment)) % alignment;

		uint32 uknMult = 1; // probably for interlaced?
		if (tvRenderMode == GX2_TV_RENDER_720I)
			uknMult = 2;

		uint32 adjustedBufferingMode = bufferingMode;
		if (tvRenderMode < GX2_TV_RENDER_720)
			adjustedBufferingMode = 4;

		uint32 bufferedImageSize = (imageSize + alignmentPaddingSize) * adjustedBufferingMode;
		bufferedImageSize = bufferedImageSize * uknMult - alignmentPaddingSize;

		memory_writeU32(outputSizeMPTR, bufferedImageSize);
		memory_writeU32(outputScaleNeededMPTR, 0); // todo
	}

	// gx2Export_GX2CalcDRCSize: r3 = drcMode, r4 = format, r5 = bufferingMode, r6 = sizeOut, r7 = scaleNeededOut
	void GX2CalcDRCSize(sint32 drcMode, uint32 format, uint32 bufferingMode, MPTR sizeMPTR, MPTR scaleNeededMPTR)
	{
		uint32 width = 0;
		uint32 height = 0;
		if (drcMode > 0)
		{
			width = 854;
			height = 480;
		}

		GX2ColorBuffer colorBuffer = {};
		memset(&colorBuffer, 0, sizeof(colorBuffer));
		_GX2InitScanBuffer(&colorBuffer, width, height, (Latte::E_GX2SURFFMT)format);

		uint32 imageSize = colorBuffer.surface.imageSize;
		uint32 alignment = colorBuffer.surface.alignment;

		uint32 alignmentPaddingSize = (alignment - (imageSize%alignment)) % alignment;


		uint32 adjustedBufferingMode = bufferingMode;

		uint32 bufferedImageSize = (imageSize + alignmentPaddingSize) * adjustedBufferingMode;
		bufferedImageSize = bufferedImageSize - alignmentPaddingSize;

		memory_writeU32(sizeMPTR, bufferedImageSize);
		memory_writeU32(scaleNeededMPTR, 0);
	}

	/****** Imported by BotW but not implemented by Cemu (Cemu: unsupported-import stub, r3 = 0) ******/
	// Parameters follow the public GX2 API; none of them is read. Real GX2 writes registers for most of these
	// (noted per function); Cemu's GPU side does not read any of those registers.

	// GX2ExpandAAColorBuffer(GX2ColorBuffer*): real GX2 decompresses MSAA color (CMASK/FMASK expand).
	void GX2ExpandAAColorBuffer(MPTR colorBuffer)
	{
	}

	// GX2ExpandDepthBuffer(GX2DepthBuffer*): real GX2 decompresses HiZ/HTILE.
	void GX2ExpandDepthBuffer(MPTR depthBuffer)
	{
	}

	// GX2GetGeometryShaderGPRs(GX2GeometryShader*): real GX2 returns SQ_PGM_RESOURCES_GS.NUM_GPRS. Cemu returns 0.
	uint32 GX2GetGeometryShaderGPRs(MPTR geometryShader)
	{
		return 0;
	}

	// GX2GetGeometryShaderStackEntries(GX2GeometryShader*): real GX2 returns SQ_PGM_RESOURCES_GS.STACK_SIZE. Cemu returns 0.
	uint32 GX2GetGeometryShaderStackEntries(MPTR geometryShader)
	{
		return 0;
	}

	// GX2InitDepthBufferHiZEnable(GX2DepthBuffer*, BOOL): real GX2 toggles the HTILE bits in the stored depth regs.
	void GX2InitDepthBufferHiZEnable(MPTR depthBuffer, uint32 enable)
	{
	}

	// GX2SetAlphaToMaskReg(GX2AlphaToMaskReg*): real GX2 writes DB_ALPHA_TO_MASK.
	void GX2SetAlphaToMaskReg(MPTR alphaToMaskReg)
	{
	}

	// GX2SetGeometryShaderInputRingBuffer(void* buffer, u32 size): real GX2 writes SQ_ESGS_RING_BASE/SIZE.
	void GX2SetGeometryShaderInputRingBuffer(MPTR buffer, uint32 size)
	{
	}

	// GX2SetGeometryShaderOutputRingBuffer(void* buffer, u32 size): real GX2 writes SQ_GSVS_RING_BASE/SIZE.
	void GX2SetGeometryShaderOutputRingBuffer(MPTR buffer, uint32 size)
	{
	}

	// GX2SetLineWidth(f32 width): real GX2 writes PA_SU_LINE_CNTL.
	void GX2SetLineWidth(float width)
	{
	}
}

// GX2_Misc.cpp
CAFE_EXPORT(gx2, GX2Invalidate, GX2::GX2Invalidate);
// GX2.cpp
CAFE_EXPORT(gx2, GX2CalcTVSize, GX2::GX2CalcTVSize);
CAFE_EXPORT(gx2, GX2CalcDRCSize, GX2::GX2CalcDRCSize);
// not implemented by Cemu
CAFE_EXPORT(gx2, GX2ExpandAAColorBuffer, GX2::GX2ExpandAAColorBuffer);
CAFE_EXPORT(gx2, GX2ExpandDepthBuffer, GX2::GX2ExpandDepthBuffer);
CAFE_EXPORT(gx2, GX2GetGeometryShaderGPRs, GX2::GX2GetGeometryShaderGPRs);
CAFE_EXPORT(gx2, GX2GetGeometryShaderStackEntries, GX2::GX2GetGeometryShaderStackEntries);
CAFE_EXPORT(gx2, GX2InitDepthBufferHiZEnable, GX2::GX2InitDepthBufferHiZEnable);
CAFE_EXPORT(gx2, GX2SetAlphaToMaskReg, GX2::GX2SetAlphaToMaskReg);
CAFE_EXPORT(gx2, GX2SetGeometryShaderInputRingBuffer, GX2::GX2SetGeometryShaderInputRingBuffer);
CAFE_EXPORT(gx2, GX2SetGeometryShaderOutputRingBuffer, GX2::GX2SetGeometryShaderOutputRingBuffer);
CAFE_EXPORT(gx2, GX2SetLineWidth, GX2::GX2SetLineWidth);
