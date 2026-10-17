// snd_core (AX) output: device modes, conversion of the 48 kHz final mix to 16-bit "DMA" frames and
// hand-off to the host audio device.
// Vendored/ported from Cemu (src/Cafe/OS/libs/snd_core/ax_out.cpp), MPL-2.0.
// Host changes: the TV frame goes to audio::push_tv every 3 ms frame (Cemu groups 4 frames for its
// IAudioAPI devices); GamePad (DRC) audio is computed but not played; the frame pacing that Cemu's
// AXOut_update does lives in the AX thread (ax_ist.cpp) and only asks this file for a device hint.
// The remote speaker API (AXRmt*) reports no samples.
#include "../audio/output.h"
#include "ax.h"
#include "ax_internal.h"

namespace snd_core
{
	uint32 numProcessedFrames = 0;

	void resetNumProcessedFrames()
	{
		numProcessedFrames = 0;
	}

	uint32 getNumProcessedFrames()
	{
		return numProcessedFrames;
	}

	sint32 __AXMode[AX_DEV_COUNT]; // audio mode (AX_MODE_*) per device

	// TV audio mode reported to the game (Cemu: GetConfig().tv_channels, default stereo).
	// 0 -> mono, 1/2 -> stereo, 3 -> surround (6ch, downmixed by audio::push_tv)
	constexpr uint32 kTVAudioMode = 2;

	bool AVMGetTVAudioMode(uint32be* tvAudioMode)
	{
		*tvAudioMode = kTVAudioMode;
		return true;
	}

	bool AVMGetDRCSystemAudioMode(uint32be* drcAudioMode)
	{
		*drcAudioMode = 1; // apparently the default is Stereo(?), MH3U exits if AXGetDeviceMode doesn't return 0 (DRCSystemAudioMode must return 1 to set DRC mode to 0)
		return true;
	}

	sint32 __AXOutTVOutputChannelCount;
	sint32 __AXOutDRCOutputChannelCount;

	void __AXSetTVMode(sint32 mode)
	{
		__AXMode[AX_DEV_TV] = mode;
	}

	void __AXSetDeviceMode(sint32 device, sint32 mode)
	{
		if (device == AX_DEV_TV)
			__AXMode[AX_DEV_TV] = mode;
		else if (device == AX_DEV_DRC)
			__AXMode[AX_DEV_DRC] = mode;
		else if (device == AX_DEV_RMT)
			__AXMode[AX_DEV_RMT] = mode;
	}

	sint32 AXGetDeviceMode(sint32 device)
	{
		if (device == AX_DEV_TV || device == AX_DEV_DRC || device == AX_DEV_RMT)
			return __AXMode[device];
		return 0;
	}

	void _AXOutInitDeviceModes()
	{
		// TV mode
		uint32be tvAudioMode;
		AVMGetTVAudioMode(&tvAudioMode);
		if (tvAudioMode == 0)
		{
			// mono
			__AXSetTVMode(AX_MODE_MONO);
			__AXOutTVOutputChannelCount = 1;
		}
		else if (tvAudioMode == 1 || tvAudioMode == 2)
		{
			// stereo
			__AXSetTVMode(AX_MODE_STEREO);
			__AXOutTVOutputChannelCount = 2;
		}
		else
		{
			// surround (6ch)
			__AXSetTVMode(AX_MODE_6CH);
			__AXOutTVOutputChannelCount = 6;
		}
		// DRC mode
		uint32be drcAudioMode;
		AVMGetDRCSystemAudioMode(&drcAudioMode);
		if (drcAudioMode == 0)
		{
			// mono
			__AXSetDeviceMode(1, AX_MODE_MONO);
			__AXOutDRCOutputChannelCount = 1;
		}
		else if (drcAudioMode == 2)
		{
			// surround
			__AXSetDeviceMode(1, AX_MODE_SURROUND);
			__AXOutDRCOutputChannelCount = 2; // output channel count still 2 for DRC 'surround'
		}
		else
		{
			// stereo
			__AXSetDeviceMode(1, AX_MODE_STEREO);
			__AXOutDRCOutputChannelCount = 2;
		}
	}

	void AXOut_Init()
	{
		_AXOutInitDeviceModes();
	}

	// DMA buffers hold big-endian samples, as on hardware (read back by AXGetDeviceFinalOutput)
	sint16 __buf_AXTVDMABuffers_0[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16 __buf_AXTVDMABuffers_1[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16 __buf_AXTVDMABuffers_2[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16* __AXTVDMABuffers[3] = {__buf_AXTVDMABuffers_0, __buf_AXTVDMABuffers_1, __buf_AXTVDMABuffers_2};

	sint16 __buf_AXDRCDMABuffers_0[AX_SAMPLES_MAX * 6];
	sint16 __buf_AXDRCDMABuffers_1[AX_SAMPLES_MAX * 6];
	sint16 __buf_AXDRCDMABuffers_2[AX_SAMPLES_MAX * 6];
	sint16* __AXDRCDMABuffers[3] = { __buf_AXDRCDMABuffers_0, __buf_AXDRCDMABuffers_1, __buf_AXDRCDMABuffers_2 };

	sint16 __AXTVHostFrame[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT]; // little-endian copy for the host device

	void AIInitDMA(sint16* sampleData, sint32 size)
	{
		sint32 sampleCount = size / sizeof(sint16); // sample count in total (summed up for all channels)
		sint32 channels = AIGetChannelCount(AX_DEV_TV);
		for (sint32 i = 0; i < sampleCount; ++i)
		{
			__AXTVHostFrame[i] = _swapEndianS16(sampleData[i]);
		}
		audio::push_tv(__AXTVHostFrame, sampleCount / channels, channels);
	}

	sint32 AIGetSamplesPerChannel(uint32 device)
	{
		// TV and DRC output the same number of samples
		return AX_SAMPLES_PER_3MS_48KHZ;
	}

	sint32 AIGetChannelCount(uint32 device)
	{
		if (__AXMode[device] == AX_MODE_6CH)
			return 6;
		if (__AXMode[device] == AX_MODE_STEREO)
			return 2;
		// default to mono
		return 1;
	}

	sint16* AIGetCurrentDMABuffer(uint32 device)
	{
		if (device == AX_DEV_TV)
			return __AXTVDMABuffers[0];
		else if (device == AX_DEV_DRC)
			return __AXDRCDMABuffers[0];
		return nullptr;
	}

	static inline sint16 AXOut_ClampToBE16(sint32be* sample)
	{
		return _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*(sint32*)sample), -32768), 32767));
	}

	void AXOut_SubmitTVFrame(sint32 frameIndex)
	{
		sint32 numSamples = AIGetSamplesPerChannel(AX_DEV_TV);
		sint32be* tvBuffer = (sint32be*)__AXTVBuffer48.GetPtr();
		if (__AXMode[AX_DEV_TV] == AX_MODE_6CH)
		{
			sint32be* inputChannel0 = tvBuffer + numSamples * 0;
			sint32be* inputChannel1 = tvBuffer + numSamples * 1;
			sint32be* inputChannel2 = tvBuffer + numSamples * 2;
			sint32be* inputChannel3 = tvBuffer + numSamples * 3;
			sint32be* inputChannel4 = tvBuffer + numSamples * 4;
			sint32be* inputChannel5 = tvBuffer + numSamples * 5;
			sint16* dmaOutputBuffer = __AXTVDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				// AX channel order L R Ls Rs C LFE -> device order FL FR FC LFE BL BR
				dmaOutputBuffer[0] = AXOut_ClampToBE16(inputChannel0);
				dmaOutputBuffer[1] = AXOut_ClampToBE16(inputChannel1);

				dmaOutputBuffer[4] = AXOut_ClampToBE16(inputChannel2);
				dmaOutputBuffer[5] = AXOut_ClampToBE16(inputChannel3);

				dmaOutputBuffer[2] = AXOut_ClampToBE16(inputChannel4);
				dmaOutputBuffer[3] = AXOut_ClampToBE16(inputChannel5);
				dmaOutputBuffer += 6;
				// next sample
				inputChannel0++;
				inputChannel1++;
				inputChannel2++;
				inputChannel3++;
				inputChannel4++;
				inputChannel5++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 6 * sizeof(sint16)); // 6ch output
		}
		else if (__AXMode[AX_DEV_TV] == AX_MODE_STEREO)
		{
			sint32be* inputChannel0 = tvBuffer + numSamples * 0;
			sint32be* inputChannel1 = tvBuffer + numSamples * 1;
			sint16* dmaOutputBuffer = __AXTVDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = AXOut_ClampToBE16(inputChannel0);
				dmaOutputBuffer[1] = AXOut_ClampToBE16(inputChannel1);
				dmaOutputBuffer += 2;
				// next sample
				inputChannel0++;
				inputChannel1++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 2 * sizeof(sint16)); // 2ch output
		}
		else
		{
			// mono
			sint32be* inputChannel0 = tvBuffer + numSamples * 0;
			sint16* dmaOutputBuffer = __AXTVDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = AXOut_ClampToBE16(inputChannel0);
				dmaOutputBuffer++;
				// next sample
				inputChannel0++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 1 * sizeof(sint16)); // 1ch
		}
	}

	void AXOut_SubmitDRCFrame(sint32 frameIndex)
	{
		// The GamePad speaker isn't played on the host; the DMA buffer is still filled for AXGetDeviceFinalOutput.
		sint32 numSamples = AIGetSamplesPerChannel(AX_DEV_DRC);
		sint32be* drcBuffer = (sint32be*)__AXDRCBuffer48.GetPtr();
		sint16* dmaOutputBuffer = __AXDRCDMABuffers[frameIndex];
		if (__AXMode[AX_DEV_DRC] == AX_MODE_6CH)
		{
			sint32be* inputChannel0 = drcBuffer + numSamples * 0;
			sint32be* inputChannel1 = drcBuffer + numSamples * 1;
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = AXOut_ClampToBE16(inputChannel0);
				dmaOutputBuffer[1] = AXOut_ClampToBE16(inputChannel1);
				dmaOutputBuffer[2] = 0;
				dmaOutputBuffer[3] = 0;
				dmaOutputBuffer[4] = 0;
				dmaOutputBuffer[5] = 0;
				dmaOutputBuffer += 6;
				inputChannel0++;
				inputChannel1++;
			}
		}
		else if (__AXMode[AX_DEV_DRC] == AX_MODE_MONO)
		{
			sint32be* inputChannel0 = drcBuffer + numSamples * 0;
			for (sint32 i = 0; i < numSamples; i++)
			{
				// write mono input as stereo output
				dmaOutputBuffer[1] = dmaOutputBuffer[0] = AXOut_ClampToBE16(inputChannel0);
				dmaOutputBuffer += 2;
				inputChannel0++;
			}
		}
		else
		{
			// stereo (and DRC "surround", which also outputs 2 channels)
			sint32be* inputChannel0 = drcBuffer + numSamples * 0;
			sint32be* inputChannel1 = drcBuffer + numSamples * 1;
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = AXOut_ClampToBE16(inputChannel0);
				dmaOutputBuffer[1] = AXOut_ClampToBE16(inputChannel1);
				dmaOutputBuffer += 2;
				inputChannel0++;
				inputChannel1++;
			}
		}
	}

	/* AX output */

	void AXOut_init()
	{
		audio::init();
	}

	// Frame pacing hint for the AX thread, mirroring Cemu's AXOut_update(): if the host device is
	// running low (Cemu: NeedAdditionalBlocks, i.e. less than the configured delay of 2 blocks of
	// 4 frames queued) frames are generated slightly faster (-1). Host addition: if the queue grows
	// past ~100 ms (device clock slower than the host clock) frames are generated slightly slower (+1).
	// Without a device (headless/muted runs) the host clock alone paces the frames (0).
	sint32 AXOut_GetFramePacingAdjustment()
	{
		constexpr int kLowWaterFrames = AX_SAMPLES_PER_3MS_48KHZ * 4 * 2;
		constexpr int kHighWaterFrames = 48000 / 10;
		static bool s_deviceActive = false;
		int queued = audio::queued_frames();
		if (queued > 0)
			s_deviceActive = true;
		if (!s_deviceActive)
			return 0;
		if (queued < kLowWaterFrames)
			return -1;
		if (queued > kHighWaterFrames)
			return 1;
		return 0;
	}

	// ---- remote (Wii remote speaker) output ----
	// The remote mix isn't produced (Cemu: "DRC1 mixing + RMT mixing todo"), so no samples are ever
	// available: BotW checks AXRmtGetSamplesLeft() >= 40 before pulling samples.
	sint32 AXRmtGetSamplesLeft()
	{
		return 0;
	}

	sint32 AXRmtGetSamples(sint32 channel, sint16be* buffer, sint32 samples)
	{
		if (buffer && samples > 0)
			memset(buffer, 0, samples * sizeof(sint16be));
		return 0;
	}

	sint32 AXRmtAdvancePtr(sint32 samples)
	{
		return 0;
	}
}
