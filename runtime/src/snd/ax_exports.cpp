// snd_core (AX) init/quit, misc exports and the export table for the snd_core functions BotW imports.
// Vendored/ported from Cemu (src/Cafe/OS/libs/snd_core/ax_exports.cpp), MPL-2.0.
// Host changes: exports are typed functions registered with CAFE_EXPORT; AXInit also opens the host
// audio device; AXGetDeviceFinalOutput copies the big-endian DMA samples as-is (Cemu byte-swaps them
// a second time on the way out).
#include "../compat/cemu_export.h"
#include "ax.h"
#include "ax_internal.h"

namespace snd_core
{
	sndGeneric_t sndGeneric;

	void AXResetToDefaultState()
	{
		sndGeneric = {};
		resetNumProcessedFrames();
		AXVBP_Reset();
	}

	bool AXIsInit()
	{
		return sndGeneric.isInitialized;
	}

	void __AXInit(bool isSoundCore2, uint32 frameLength, uint32 rendererFreq, uint32 pipelineMode)
	{
		sndGeneric.isSoundCore2 = isSoundCore2;
		sndGeneric.initParam.frameLength = frameLength;
		sndGeneric.initParam.rendererFreq = rendererFreq;
		sndGeneric.initParam.pipelineMode = pipelineMode;
		// init submodules
		AXIst_Init();
		AXOut_Init();
		AXVPB_Init();
		AXAux_Init();
		AXMix_Init();
		AXMultiVoice_Init();
		AXOut_init(); // host audio device (Cemu opens it at title start)
		sndGeneric.isInitialized = true;
		AXIst_InitThread();
		LOG("snd_core: AXInit (%s, %u Hz renderer)", isSoundCore2 ? "sndcore2" : "snd_core", rendererFreq == AX_RENDERER_FREQ_48KHZ ? 48000 : 32000);
	}

	void sndcore1_AXInit()
	{
		if (sndGeneric.isInitialized)
			return;
		__AXInit(false, AX_FRAMELENGTH_3MS, AX_RENDERER_FREQ_32KHZ, AX_PIPELINE_SINGLE);
	}

	void AXQuit()
	{
		AXResetCallbacks();
		// todo - should we wait to make sure any active callbacks are finished with execution before we exit AXQuit?
		// request worker thread stop and wait until complete
		AXIst_StopThread();
		// clean up subsystems
		AXVBP_Reset();
		sndGeneric.isInitialized = false;
	}

	sint32 AXGetMaxVoices()
	{
		return sndGeneric.isInitialized ? AX_MAX_VOICES : 0;
	}

	// AXGetDeviceMode(device, u32* mode) -> 0
	sint32 export_AXGetDeviceMode(sint32 device, uint32be* mode)
	{
		*mode = AXGetDeviceMode(device);
		return 0;
	}

	struct AXGetDeviceFinalOutput_t
	{
		/* +0x00 */ uint32be channelCount;
		/* +0x04 */ uint32be uknValue;
		/* +0x08 */ uint32be ukn08;
		/* +0x0C */ uint32be ukn0C;
		/* +0x10 */ uint32be size;
		// struct might be bigger?
	};

	sint32 AXGetDeviceFinalOutput(uint32 device, sint16be* sampleBufferOutput, uint32 bufferSize, AXGetDeviceFinalOutput_t* output)
	{
		if (device != AX_DEV_TV && device != AX_DEV_DRC)
			return -1;
		sint32 channelCount = AIGetChannelCount(device);
		sint32 samplesPerChannel = AIGetSamplesPerChannel(device);
		sint32 samplesToCopy = samplesPerChannel * channelCount;

		if (bufferSize < (samplesToCopy * sizeof(sint16be)))
			return -11; // buffer not large enough

		// copy samples to buffer (DMA buffers already hold big-endian samples)
		sint16* samplesBuffer = AIGetCurrentDMABuffer(device);
		memcpy(sampleBufferOutput, samplesBuffer, samplesToCopy * sizeof(sint16be));

		// set output struct
		output->size = samplesToCopy * sizeof(sint16be);
		output->channelCount = channelCount;
		output->uknValue = 1; // always set to 1/true?

		return 0;
	}

	bool isInitialized()
	{
		return sndGeneric.isInitialized;
	}

	bool IsSndCore2()
	{
		return sndGeneric.isSoundCore2;
	}
}

// ---- snd_core exports imported by BotW ----
// init / callbacks / devices
CAFE_EXPORT(snd_core, AXInit, snd_core::sndcore1_AXInit);
CAFE_EXPORT(snd_core, AXQuit, snd_core::AXQuit);
CAFE_EXPORT(snd_core, AXSetDefaultMixerSelect, snd_core::AXSetDefaultMixerSelect);
CAFE_EXPORT(snd_core, AXGetDeviceFinalMixCallback, snd_core::AXGetDeviceFinalMixCallback);
CAFE_EXPORT(snd_core, AXRegisterDeviceFinalMixCallback, snd_core::AXRegisterDeviceFinalMixCallback);
CAFE_EXPORT(snd_core, AXRegisterAppFrameCallback, snd_core::AXRegisterAppFrameCallback);
CAFE_EXPORT(snd_core, AXDeregisterAppFrameCallback, snd_core::AXDeregisterAppFrameCallback);
CAFE_EXPORT(snd_core, AXRegisterAuxCallback, snd_core::AXRegisterAuxCallback);
CAFE_EXPORT(snd_core, AXGetAuxCallback, snd_core::AXGetAuxCallback);
CAFE_EXPORT(snd_core, AXSetAuxReturnVolume, snd_core::AXSetAuxReturnVolume);
CAFE_EXPORT(snd_core, AXGetDeviceMode, snd_core::export_AXGetDeviceMode);
CAFE_EXPORT(snd_core, AXGetDeviceUpsampleStage, snd_core::AXGetDeviceUpsampleStage);
CAFE_EXPORT(snd_core, AXGetDeviceFinalOutput, snd_core::AXGetDeviceFinalOutput);
CAFE_EXPORT(snd_core, AXSetDeviceLinearUpsampler, snd_core::AXSetDeviceLinearUpsampler);
CAFE_EXPORT(snd_core, AXSetDeviceCompressor, snd_core::AXSetDeviceCompressor);
CAFE_EXPORT(snd_core, AXRegisterExceedCallback, snd_core::AXRegisterExceedCallback);
CAFE_EXPORT(snd_core, AXInitProfile, snd_core::AXInitProfile);
CAFE_EXPORT(snd_core, AXGetSwapProfile, snd_core::AXGetSwapProfile);
CAFE_EXPORT(snd_core, AXUserIsProtected, snd_core::AXUserIsProtected);

// voices
CAFE_EXPORT(snd_core, AXAcquireVoiceEx, snd_core::AXAcquireVoiceEx);
CAFE_EXPORT(snd_core, AXFreeVoice, snd_core::AXFreeVoice);
CAFE_EXPORT(snd_core, AXSetVoicePriority, snd_core::AXSetVoicePriority);
CAFE_EXPORT(snd_core, AXSetVoiceDeviceMix, snd_core::AXSetVoiceDeviceMix);
CAFE_EXPORT(snd_core, AXComputeLpfCoefs, snd_core::AXComputeLpfCoefs);
CAFE_EXPORT(snd_core, AXSetVoiceState, snd_core::AXSetVoiceState);
CAFE_EXPORT(snd_core, AXSetVoiceType, snd_core::AXSetVoiceType);
CAFE_EXPORT(snd_core, AXSetVoiceAdpcm, snd_core::AXSetVoiceAdpcm);
CAFE_EXPORT(snd_core, AXSetVoiceAdpcmLoop, snd_core::AXSetVoiceAdpcmLoop);
CAFE_EXPORT(snd_core, AXSetVoiceSrc, snd_core::AXSetVoiceSrc);
CAFE_EXPORT(snd_core, AXSetVoiceSrcType, snd_core::AXSetVoiceSrcType);
CAFE_EXPORT(snd_core, AXSetVoiceSrcRatio, snd_core::AXSetVoiceSrcRatio);
CAFE_EXPORT(snd_core, AXSetVoiceVe, snd_core::AXSetVoiceVe);
CAFE_EXPORT(snd_core, AXSetVoiceLpf, snd_core::AXSetVoiceLpf);
CAFE_EXPORT(snd_core, AXSetVoiceLpfCoefs, snd_core::AXSetVoiceLpfCoefs);
CAFE_EXPORT(snd_core, AXSetVoiceBiquad, snd_core::AXSetVoiceBiquad);
CAFE_EXPORT(snd_core, AXSetVoiceBiquadCoefs, snd_core::AXSetVoiceBiquadCoefs);
CAFE_EXPORT(snd_core, AXSetVoiceOffsets, snd_core::AXSetVoiceOffsets);
CAFE_EXPORT(snd_core, AXGetVoiceOffsets, snd_core::AXGetVoiceOffsets);
CAFE_EXPORT(snd_core, AXSetVoiceCurrentOffset, snd_core::AXSetVoiceCurrentOffset);
CAFE_EXPORT(snd_core, AXSetVoiceLoopOffsetEx, snd_core::AXSetVoiceLoopOffsetEx);
CAFE_EXPORT(snd_core, AXSetVoiceEndOffsetEx, snd_core::AXSetVoiceEndOffsetEx);
CAFE_EXPORT(snd_core, AXGetVoiceCurrentOffsetEx, snd_core::AXGetVoiceCurrentOffsetEx);
CAFE_EXPORT(snd_core, AXSetVoiceLoop, snd_core::AXSetVoiceLoop);
CAFE_EXPORT(snd_core, AXGetVoiceLoopCount, snd_core::AXGetVoiceLoopCount);
CAFE_EXPORT(snd_core, AXSetVoiceMixerSelect, snd_core::AXSetVoiceMixerSelect);
CAFE_EXPORT(snd_core, AXSetVoiceRmtOn, snd_core::AXSetVoiceRmtOn);
CAFE_EXPORT(snd_core, AXSetVoiceRmtIIR, snd_core::AXSetVoiceRmtIIR);
CAFE_EXPORT(snd_core, AXSetVoiceRmtIIRCoefs, snd_core::AXSetVoiceRmtIIRCoefs);
CAFE_EXPORT(snd_core, AXDecodeAdpcmData, snd_core::AXDecodeAdpcmData);

// remote speaker
CAFE_EXPORT(snd_core, AXRmtGetSamplesLeft, snd_core::AXRmtGetSamplesLeft);
CAFE_EXPORT(snd_core, AXRmtGetSamples, snd_core::AXRmtGetSamples);
CAFE_EXPORT(snd_core, AXRmtAdvancePtr, snd_core::AXRmtAdvancePtr);

// DRC virtual surround
CAFE_EXPORT(snd_core, AXSetDRCVSMode, snd_core::AXSetDRCVSMode);
CAFE_EXPORT(snd_core, AXSetDRCVSDownmixBalance, snd_core::AXSetDRCVSDownmixBalance);
CAFE_EXPORT(snd_core, AXSetDRCVSLC, snd_core::AXSetDRCVSLC);
CAFE_EXPORT(snd_core, AXSetDRCVSOutputGain, snd_core::AXSetDRCVSOutputGain);
CAFE_EXPORT(snd_core, AXSetDRCVSSpeakerPosition, snd_core::AXSetDRCVSSpeakerPosition);
CAFE_EXPORT(snd_core, AXSetDRCVSSurroundDepth, snd_core::AXSetDRCVSSurroundDepth);
CAFE_EXPORT(snd_core, AXSetDRCVSSurroundLevelGain, snd_core::AXSetDRCVSSurroundLevelGain);
CAFE_EXPORT(snd_core, DRCVS_Process, snd_core::DRCVS_Process);
CAFE_EXPORT(snd_core, DRCVS_SetOutputMode, snd_core::DRCVS_SetOutputMode);
