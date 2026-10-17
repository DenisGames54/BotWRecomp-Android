// snd_core DRC virtual surround (DRCVS): the GamePad speaker downmix. Not implemented by Cemu.
// GamePad audio is not played on the host, so the settings are stored and the processing is a
// pass-through: BotW's DRC final-mix callback calls DRCVS_Process(in[4], out[2], 144) with the
// output pointers aliasing input channels 0/1, so leaving the buffers untouched keeps front L/R.
// Setters always report success (AX_RESULT_SUCCESS) so the game's DRC setup proceeds.
#include "ax.h"
#include "ax_internal.h"

namespace snd_core
{
	struct
	{
		uint32 mode;
		uint32 lc;
		uint32 surroundLevelGain;
		uint32 outputMode;
		float downmixBalance[2];
		float outputGain[2];
		float surroundDepth[2];
		float speakerPosition[2][2];
	}__AXDRCVS;

	sint32 AXSetDRCVSMode(uint32 mode)
	{
		__AXDRCVS.mode = mode;
		return 0;
	}

	sint32 AXSetDRCVSDownmixBalance(uint32 output, float balance)
	{
		if (output < 2)
			__AXDRCVS.downmixBalance[output] = balance;
		return 0;
	}

	sint32 AXSetDRCVSLC(uint32 lc)
	{
		__AXDRCVS.lc = lc;
		return 0;
	}

	sint32 AXSetDRCVSOutputGain(uint32 output, float gain)
	{
		if (output < 2)
			__AXDRCVS.outputGain[output] = gain;
		return 0;
	}

	sint32 AXSetDRCVSSpeakerPosition(uint32 output, uint32 position, float angle)
	{
		if (output < 2 && position < 2)
			__AXDRCVS.speakerPosition[output][position] = angle;
		return 0;
	}

	sint32 AXSetDRCVSSurroundDepth(uint32 output, float depth)
	{
		if (output < 2)
			__AXDRCVS.surroundDepth[output] = depth;
		return 0;
	}

	sint32 AXSetDRCVSSurroundLevelGain(uint32 gain)
	{
		__AXDRCVS.surroundLevelGain = gain;
		return 0;
	}

	void DRCVS_Process(MEMPTR<sint32be>* input, MEMPTR<sint32be>* output, uint32 sampleCount)
	{
		// pass-through (see above)
	}

	sint32 DRCVS_SetOutputMode(uint32 mode)
	{
		__AXDRCVS.outputMode = mode;
		return 0;
	}
}
