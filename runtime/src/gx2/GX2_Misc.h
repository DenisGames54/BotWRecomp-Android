// Vendored from Cemu (src/Cafe/OS/libs/gx2/GX2_Misc.h), MPL-2.0. Only the parts used by the ported files
// (GX2Init/GX2SetTVBuffer/sGX2MainCoreIndex live in gx2_core.cpp / GX2_Command.h).
#pragma once
#include "../compat/cemu.h"

namespace GX2
{
	enum class GX2InvalidationFlag : uint32
	{
		GPU_ATTRIB        = (1<<0),
		GPU_TEXTURE       = (1<<1),
		GPU_UNIFORM_BLOCK = (1<<2),
		GPU_SHADER        = (1<<3),
		GPU_COLOR_BUFFER  = (1<<4),
		GPU_DEPTH_BUFFER  = (1<<5),
		CPU               = (1<<6),
		GPU_STREAM_OUT    = (1<<7),
		GPU_EXPORT_BUFFER = (1<<8)
	};

	void GX2Invalidate(GX2InvalidationFlag invalidationFlags, MPTR invalidationAddr, uint32 invalidationSize);
};

ENABLE_BITMASK_OPERATORS(GX2::GX2InvalidationFlag);
