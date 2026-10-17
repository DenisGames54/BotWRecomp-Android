// snd_core (AX) voices: voice lists, acquire/free/drop, user/voice protection and the AXSetVoice* API.
// Vendored/ported from Cemu (src/Cafe/OS/libs/snd_core/ax_voice.cpp), MPL-2.0.
// Host changes: the list spinlock is a recursive mutex and setters additionally take
// __AXVoiceParamLock (see ax.h). AXSetVoicePriority/MixerSelect/Rmt* are not in Cemu.
#include "ax.h"
#include "ax_internal.h"

namespace snd_core
{
	std::recursive_mutex __AXVoiceListSpinlock;
	std::recursive_mutex __AXVoiceParamLock;

#define AX_VOICE_PARAM_LOCK() std::lock_guard<std::recursive_mutex> _axParamLock(__AXVoiceParamLock)

	inline void AXSetSyncFlag(AXVPB* vpb, uint32 flags)
	{
		vpb->sync = (uint32be)((uint32)vpb->sync | flags);
	}

	inline void AXResetSyncFlag(AXVPB* vpb, uint32 flags)
	{
		vpb->sync = (uint32be)((uint32)vpb->sync & ~flags);
	}

	/* voice lists */
	std::vector<AXVPB*> __AXVoicesPerPriority[AX_PRIORITY_MAX];
	std::vector<AXVPB*> __AXFreeVoices;

	void AXVoiceList_AddFreeVoice(AXVPB* vpb)
	{
		__AXFreeVoices.push_back(vpb);
		vpb->prev = nullptr;
		vpb->next = nullptr;
	}

	AXVPB* AXVoiceList_GetFreeVoice()
	{
		if (__AXFreeVoices.empty())
			return nullptr;
		AXVPB* vpb = __AXFreeVoices.back();
		__AXFreeVoices.pop_back();
		return vpb;
	}

	std::vector<AXVPB*>& AXVoiceList_GetFreeVoices()
	{
		return __AXFreeVoices;
	}

	void AXVoiceList_AddVoice(AXVPB* vpb, sint32 priority)
	{
		__AXVoicesPerPriority[priority].push_back(vpb);
		vpb->next = nullptr;
		vpb->prev = nullptr;
		vpb->priority = priority;
	}

	void AXVoiceList_RemoveVoice(AXVPB* vpb)
	{
		uint32 priority = (uint32)vpb->priority;
		if (priority == AX_PRIORITY_FREE || priority >= AX_PRIORITY_MAX)
			return;
		vectorRemoveByValue(__AXVoicesPerPriority[priority], vpb);
	}

	AXVPB* AXVoiceList_GetLeastRecentVoiceByPriority(uint32 priority)
	{
		if (__AXVoicesPerPriority[priority].empty())
			return nullptr;
		return __AXVoicesPerPriority[priority].front();
	}

	std::vector<AXVPB*>& AXVoiceList_GetListByPriority(uint32 priority)
	{
		return __AXVoicesPerPriority[priority];
	}

	void AXVoiceList_Reset()
	{
		__AXFreeVoices.clear();
		for (uint32 i = 0; i < AX_PRIORITY_MAX; i++)
			__AXVoicesPerPriority[i].clear();
	}

	SysAllocator<AXVPBInternal_t, AX_MAX_VOICES> _buffer__AXVPBInternalVoiceArray;
	AXVPBInternal_t* __AXVPBInternalVoiceArray;

	SysAllocator<AXVPBInternal_t, AX_MAX_VOICES> _buffer__AXVPBInternalVoiceShadowCopyArray;
	AXVPBInternal_t* __AXVPBInternalVoiceShadowCopyArrayPtr; // this is the array used by audio mixing (it's synced at the beginning of every audio frame with __AXVPBInternalVoiceArray)

	SysAllocator<AXVPBItd, AX_MAX_VOICES> _buffer__AXVPBItdArray;
	AXVPBItd* __AXVPBItdArrayPtr;

	SysAllocator<AXVPB, AX_MAX_VOICES> _buffer__AXVPBArray;
	AXVPB* __AXVPBArrayPtr;

	struct AXUSERPROTECTION
	{
		MPTR threadMPTR;
		uint32 count;
	};

	uint32 __AXUserProtectionArraySize = 0;
	AXUSERPROTECTION __AXUserProtectionArray[AX_MAX_VOICES] = {};
	AXUSERPROTECTION __AXVoiceProtection[AX_MAX_VOICES] = {};

	bool AXUserIsProtected()
	{
		return __AXUserProtectionArraySize != 0;
	}

	sint32 AXUserBegin()
	{
		AX_VOICE_PARAM_LOCK();
		if (AXIst_IsFrameBeingProcessed())
		{
			return -2;
		}
		MPTR currentThreadMPTR = AXGetCurrentThreadMPTR();
		for (sint32 i = __AXUserProtectionArraySize - 1; i >= 0; i--)
		{
			if (__AXUserProtectionArray[i].threadMPTR == currentThreadMPTR)
			{
				sint32 newCount = __AXUserProtectionArray[i].count + 1;
				__AXUserProtectionArray[i].count = newCount;
				return newCount;
			}
		}
		// no matching entry found
		if (__AXUserProtectionArraySize >= AX_MAX_VOICES)
		{
			// no entry available
			return -4;
		}
		// create new entry
		sint32 entryIndex = __AXUserProtectionArraySize;
		__AXUserProtectionArray[entryIndex].threadMPTR = currentThreadMPTR;
		__AXUserProtectionArray[entryIndex].count = 1;
		__AXUserProtectionArraySize++;
		return 1;
	}

	sint32 AXUserEnd()
	{
		AX_VOICE_PARAM_LOCK();
		if (AXIst_IsFrameBeingProcessed())
			return -2;
		MPTR currentThreadMPTR = AXGetCurrentThreadMPTR();
		for (sint32 i = __AXUserProtectionArraySize - 1; i >= 0; i--)
		{
			if (__AXUserProtectionArray[i].threadMPTR == currentThreadMPTR)
			{
				sint32 newCount = __AXUserProtectionArray[i].count - 1;
				__AXUserProtectionArray[i].count = newCount;
				if (__AXUserProtectionArray[i].count == 0)
				{
					// count == 0 -> remove entry
					if (i >= (sint32)(__AXUserProtectionArraySize - 1))
					{
						// entry is last in array, can just decrease array size
						__AXUserProtectionArraySize--;
						__AXUserProtectionArray[i].threadMPTR = MPTR_NULL;
					}
					else
					{
						// Cemu leaves this case unimplemented (entry stays in the array)
						cemu_assert_debug(false);
					}
					// remove entries associated with the current thread from __AXVoiceProtection[] if the count is zero
					for (sint32 f = 0; f < AX_MAX_VOICES; f++)
					{
						if (__AXVoiceProtection[f].threadMPTR == currentThreadMPTR && __AXVoiceProtection[f].count == 0)
						{
							__AXVoiceProtection[f].threadMPTR = MPTR_NULL;
						}
					}
				}
				return newCount;
			}
		}
		cemu_assert_debug(false); // voice not found in list, did the game not call AXUserBegin()?
		return -3;
	}

	bool AXVoiceProtection_IsProtectedByAnyThread(AXVPB* vpb)
	{
		sint32 index = vpb->index;
		return __AXVoiceProtection[index].threadMPTR != MPTR_NULL;
	}

	bool AXVoiceProtection_IsProtectedByCurrentThread(AXVPB* vpb)
	{
		sint32 index = vpb->index;
		bool isProtected = false;
		if (AXIst_IsFrameBeingProcessed())
			isProtected = __AXVoiceProtection[index].threadMPTR != MPTR_NULL;
		else
			isProtected = __AXVoiceProtection[index].threadMPTR != AXGetCurrentThreadMPTR();
		return isProtected;
	}

	void AXVoiceProtection_Acquire(AXVPB* vpb)
	{
		sint32 index = vpb->index;
		if (AXUserIsProtected() == false)
			return;
		if (AXIst_IsFrameBeingProcessed())
			return;
		if (__AXVoiceProtection[index].threadMPTR == MPTR_NULL)
		{
			__AXVoiceProtection[index].threadMPTR = AXGetCurrentThreadMPTR();
			// does not set count?
		}
	}

	void AXVoiceProtection_Release(AXVPB* vpb)
	{
		sint32 index = vpb->index;
		__AXVoiceProtection[index].threadMPTR = MPTR_NULL;
		__AXVoiceProtection[index].count = 0;
	}

	sint32 AXVoiceBegin(AXVPB* voice)
	{
		if (voice == nullptr)
			return -1;
		uint32 index = (uint32)voice->index;
		if (index >= AX_MAX_VOICES)
			return -1;
		AX_VOICE_PARAM_LOCK();
		if (AXIst_IsFrameBeingProcessed())
			return -2;
		MPTR currentThreadMPTR = AXGetCurrentThreadMPTR();
		if (__AXVoiceProtection[index].threadMPTR == MPTR_NULL)
		{
			__AXVoiceProtection[index].threadMPTR = currentThreadMPTR;
			__AXVoiceProtection[index].count = 1;
			return 1;
		}
		else if (__AXVoiceProtection[index].threadMPTR == currentThreadMPTR)
		{
			__AXVoiceProtection[index].count++;
			return __AXVoiceProtection[index].count;
		}
		return -1;
	}

	sint32 __GetThreadProtection(MPTR threadMPTR)
	{
		for (sint32 i = __AXUserProtectionArraySize - 1; i >= 0; i--)
		{
			if (__AXUserProtectionArray[i].threadMPTR == threadMPTR)
				return i;
		}
		return -1;
	}

	sint32 AXVoiceEnd(AXVPB* voice)
	{
		if (voice == nullptr)
			return -1;
		uint32 index = (uint32)voice->index;
		if (index >= AX_MAX_VOICES)
			return -1;
		AX_VOICE_PARAM_LOCK();
		if (AXIst_IsFrameBeingProcessed())
			return -2;
		MPTR currentThreadMPTR = AXGetCurrentThreadMPTR();
		if (__AXVoiceProtection[index].threadMPTR == currentThreadMPTR)
		{
			if (__AXVoiceProtection[index].count > 0)
				__AXVoiceProtection[index].count--;
			if (__AXVoiceProtection[index].count == 0)
			{
				if (__GetThreadProtection(currentThreadMPTR) == -1)
				{
					__AXVoiceProtection[index].threadMPTR = 0;
				}
			}
			sint32 count = __AXVoiceProtection[index].count;
			return count;
		}
		else
		{
			if (__AXVoiceProtection[index].threadMPTR == MPTR_NULL)
			{
				return -3;
			}
			else
			{
				return -1;
			}
		}
	}

	void AXVPB_SetVoiceDefault(AXVPB* vpb)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = GetInternalVoice(vpb);
		vpb->playbackState = 0;
		vpb->sync = 0;
		AXSetSyncFlag(vpb, AX_SYNCFLAG_PLAYBACKSTATE);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_SRCDATA);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_LPFDATA);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_BIQUADDATA);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_VOICEREMOTEON);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_ITD20);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_8000000);
		AXSetSyncFlag(vpb, 0x10000000);
		internal->reserved282_rmtIIRGuessed = 0;
		internal->reserved16C = 0;
		internal->reserved148_voiceRmtOn = 0;
		internal->biquad.on = 0;
		internal->lpf.on = 0;
		internal->playbackState = 0;
		uint32 defaultMixer = AXGetDefaultMixerSelect();
		internal->mixerSelect = defaultMixer;
		vpb->mixerSelect = defaultMixer;
		AXResetVoiceLoopCount(vpb);
		internal->reserved280 = 0;
		internal->src.currentFrac = 0;
		internal->src.historySamples[0] = 0;
		internal->src.historySamples[1] = 0;
		internal->src.historySamples[2] = 0;
		internal->src.historySamples[3] = 0;
		internal->reserved278 = 0;
		internal->reserved27A = 0;
		internal->reserved27C = 0;
		internal->reserved27E = 0;
		internal->srcFilterMode = 0;
		memset(&internal->deviceMixMaskTV, 0, 8);
		memset(&internal->deviceMixMaskDRC, 0, 16);
		memset(&internal->deviceMixMaskRMT, 0, 0x20);
		memset(&internal->reserved1E8, 0, 0x30);
		memset(&internal->reserved218, 0, 0x40);
		memset(&internal->reserved258, 0, 0x20);
	}

	AXVPB* AXVPB_DropVoice(sint32 priority)
	{
		for (sint32 i = 1; i < priority; i++)
		{
			AXVPB* voiceItr = AXVoiceList_GetLeastRecentVoiceByPriority(i);
			if (voiceItr)
			{
				// get last voice in chain
				while (voiceItr->next)
					voiceItr = voiceItr->next.GetPtr();
				// drop voice
				if (voiceItr->playbackState != 0)
				{
					voiceItr->depop = 1;
				}
				// do drop callback
				if (voiceItr->callback)
				{
					PPCCoreCallback((MPTR)voiceItr->callback, voiceItr);
				}
				voiceItr->ukn4C_dropReason = 0; // probably drop reason?
				if (voiceItr->callbackEx)
				{
					PPCCoreCallback((MPTR)voiceItr->callbackEx, voiceItr, (MPTR)voiceItr->userParam, (uint32)voiceItr->ukn4C_dropReason);
				}
				// move voice to new stack
				AXVoiceList_RemoveVoice(voiceItr);
				AXVoiceList_AddVoice(voiceItr, priority);
				return voiceItr;
			}
		}
		return nullptr;
	}

	AXVPB* AXAcquireVoiceEx(uint32 priority, MPTR callbackEx, MPTR userParam)
	{
		if (priority == AX_PRIORITY_FREE || priority >= AX_PRIORITY_MAX)
			return nullptr; // Cemu asserts here
		std::lock_guard<std::recursive_mutex> listLock(__AXVoiceListSpinlock);
		AXVPB* vpb = AXVoiceList_GetFreeVoice();
		if (vpb != nullptr)
		{
			AXVoiceList_AddVoice(vpb, priority);
		}
		else
		{
			// no free voice available, try to drop a voice with lower priority
			vpb = AXVPB_DropVoice(priority);
			if (!vpb)
			{
				// no voice available
				return nullptr;
			}
		}
		vpb->userParam = userParam;
		vpb->callback = MPTR_NULL;
		vpb->callbackEx = callbackEx;
		AXVPB_SetVoiceDefault(vpb);
		return vpb;
	}

	void AXFreeVoice(AXVPB* vpb)
	{
		if (vpb == nullptr)
			return;
		std::lock_guard<std::recursive_mutex> listLock(__AXVoiceListSpinlock);
		if (vpb->priority == (uint32be)AX_PRIORITY_FREE)
		{
			// AXFreeVoice() called on free voice
			return;
		}
		AX_VOICE_PARAM_LOCK();
		AXVoiceProtection_Release(vpb);
		AXVoiceList_RemoveVoice(vpb);
		if (vpb->playbackState != (uint32be)0)
		{
			vpb->depop = (uint32be)1;
		}
		AXVPB_SetVoiceDefault(vpb);
		vpb->callback = MPTR_NULL;
		vpb->callbackEx = MPTR_NULL;
		AXVoiceList_AddFreeVoice(vpb);
	}

	void __AXVPBResetVoices()
	{
		__AXVPBInternalVoiceArray = _buffer__AXVPBInternalVoiceArray.GetPtr();
		__AXVPBInternalVoiceShadowCopyArrayPtr = _buffer__AXVPBInternalVoiceShadowCopyArray.GetPtr();
		__AXVPBArrayPtr = _buffer__AXVPBArray.GetPtr();
		__AXVPBItdArrayPtr = _buffer__AXVPBItdArray.GetPtr();

		memset(__AXVPBInternalVoiceShadowCopyArrayPtr, 0, sizeof(AXVPBInternal_t)*AX_MAX_VOICES);
		memset(__AXVPBInternalVoiceArray, 0, sizeof(AXVPBInternal_t)*AX_MAX_VOICES);
		memset(__AXVPBItdArrayPtr, 0, sizeof(AXVPBItd)*AX_MAX_VOICES);
		memset(__AXVPBArrayPtr, 0, sizeof(AXVPB)*AX_MAX_VOICES);
	}

	void AXVPBInit()
	{
		__AXVPBResetVoices();
		for (sint32 i = 0; i < AX_MAX_VOICES; i++)
		{
			AXVPBItd* itd = __AXVPBItdArrayPtr + i;
			AXVPBInternal_t* internalShadowCopy = __AXVPBInternalVoiceShadowCopyArrayPtr + i;
			AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + i;
			AXVPB* vpb = __AXVPBArrayPtr + i;

			MPTR internalShadowCopyPhys = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(internalShadowCopy));
			MPTR itdPhys = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(itd));

			vpb->callbackEx = MPTR_NULL;
			vpb->itd = itd;
			vpb->callback = MPTR_NULL;
			vpb->index = i;
			AXVPB_SetVoiceDefault(vpb);

			if (i == (AX_MAX_VOICES - 1))
			{
				internal->nextAddrHigh = 0;
				internal->nextAddrLow = 0;
				internalShadowCopy->nextAddrHigh = 0;
				internalShadowCopy->nextAddrLow = 0;
			}
			else
			{
				MPTR nextShadowCopyPhys = internalShadowCopyPhys + sizeof(AXVPBInternal_t);
				internalShadowCopy->nextAddrHigh = internal->nextAddrHigh = (nextShadowCopyPhys >> 16);
				internalShadowCopy->nextAddrLow = internal->nextAddrLow = (nextShadowCopyPhys & 0xFFFF);
			}
			internalShadowCopy->index = internal->index = i;
			internalShadowCopy->selfAddrHigh = internal->selfAddrHigh = (internalShadowCopyPhys >> 16);
			internalShadowCopy->selfAddrLow = internal->selfAddrLow = (internalShadowCopyPhys & 0xFFFF);
			internalShadowCopy->itdAddrHigh = internal->itdAddrHigh = (itdPhys >> 16);
			internalShadowCopy->itdAddrLow = internal->itdAddrLow = (itdPhys & 0xFFFF);
			vpb->priority = 1;
			AXVoiceList_AddFreeVoice(vpb);
		}
	}

	void AXVPB_Init()
	{
		std::lock_guard<std::recursive_mutex> listLock(__AXVoiceListSpinlock);
		AXVoiceList_Reset();
		__AXVPBResetVoices();
		AXVPBInit();
	}

	void AXVBP_Reset()
	{
		std::lock_guard<std::recursive_mutex> listLock(__AXVoiceListSpinlock);
		AXVoiceList_Reset();
		__AXVPBResetVoices();
	}

	sint32 AXIsValidDevice(sint32 device, sint32 deviceIndex)
	{
		if (device == AX_DEV_TV)
		{
			if (deviceIndex != 0)
				return -2;
		}
		else if (device == AX_DEV_DRC)
		{
			if (deviceIndex != 0 && deviceIndex != 1)
				return -2;
		}
		else if (device == AX_DEV_TV) // sic (Cemu): RMT falls through to -1
		{
			if (deviceIndex < 0 || deviceIndex >= 4)
				return -2;
		}
		else
			return -1;
		return 0;
	}

	void __AXSetVoiceChannelMix(AXCHMIX_DEPR* mixOut, AXCHMIX_DEPR* mixIn, sint16* mixMask)
	{
		for (sint32 i = 0; i < AX_BUS_COUNT; i++)
		{
			mixOut[i].vol = mixIn[i].vol;
			mixOut[i].delta = mixIn[i].delta;
			if (mixIn[i].delta)
				mixMask[i] = 3;
			else if (mixIn[i].vol)
				mixMask[i] = 1;
			else
				mixMask[i] = 0;
		}
	}

	sint32 AXSetVoiceDeviceMix(AXVPB* vpb, sint32 device, sint32 deviceIndex, AXCHMIX_DEPR* mix)
	{
		if (vpb == nullptr)
			return -4;
		if (mix == nullptr)
			return -3;
		sint32 r = AXIsValidDevice(device, deviceIndex);
		if (r)
			return r;
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		sint32 channelCount;

		uint16* deviceMixMask;
		AXCHMIX_DEPR* voiceMix;
		if (device == AX_DEV_TV)
		{
			channelCount = AX_TV_CHANNEL_COUNT;
			voiceMix = internal->deviceMixTV + deviceIndex * 0x60 / 4;
			deviceMixMask = internal->deviceMixMaskTV;
		}
		else if (device == AX_DEV_DRC)
		{
			channelCount = AX_DRC_CHANNEL_COUNT;
			voiceMix = internal->deviceMixDRC + deviceIndex * 16;
			deviceMixMask = internal->deviceMixMaskDRC;
		}
		else
		{
			// AX_DEV_RMT: unreachable, AXIsValidDevice() rejects it (Cemu asserts here)
			return -1;
		}
		sint16 updatedMixMask[AX_BUS_COUNT];
		for (sint32 i = 0; i < AX_BUS_COUNT; i++)
		{
			updatedMixMask[i] = 0;
		}
		sint16 channelMixMask[AX_BUS_COUNT];
		for (sint32 c = 0; c < channelCount; c++)
		{
			__AXSetVoiceChannelMix(voiceMix, mix, channelMixMask);
			for (sint32 i = 0; i < AX_BUS_COUNT; i++)
			{
				updatedMixMask[i] |= (channelMixMask[i] << (c * 2));
			}
			// next channel
			voiceMix += AX_BUS_COUNT;
			mix += AX_BUS_COUNT;
		}
		for (sint32 i = 0; i < AX_BUS_COUNT; i++)
		{
			deviceMixMask[i] = _swapEndianU16(updatedMixMask[i]);
		}
		vpb->sync = (uint32)vpb->sync | (AX_SYNCFLAG_DEVICEMIXMASK | AX_SYNCFLAG_DEVICEMIX);
		AXVoiceProtection_Acquire(vpb);
		return 0;
	}

	void AXSetVoiceState(AXVPB* vpb, sint32 voiceState)
	{
		AX_VOICE_PARAM_LOCK();
		if (vpb->playbackState != (uint32be)voiceState)
		{
			vpb->playbackState = voiceState;
			AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
			internal->playbackState = _swapEndianU16(voiceState);
			AXSetSyncFlag(vpb, AX_SYNCFLAG_PLAYBACKSTATE);
			AXVoiceProtection_Acquire(vpb);
			if (voiceState == 0)
			{
				vpb->depop = (uint32be)1;
			}
		}
	}

	sint32 AXIsVoiceRunning(AXVPB* vpb)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		return (_swapEndianU16(internal->playbackState) == 1) ? 1 : 0;
	}

	void AXSetVoiceType(AXVPB* vpb, uint16 voiceType)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->voiceType = _swapEndianU16(voiceType);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_VOICETYPE);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceAdpcm(AXVPB* vpb, AXPBADPCM_t* adpcm)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)(vpb->index);
		for (sint32 i = 0; i < 16; i++)
			internal->adpcmData.coef[i] = adpcm->a[i];
		internal->adpcmData.gain = adpcm->gain;
		internal->adpcmData.scale = adpcm->scale;
		AXSetSyncFlag(vpb, AX_SYNCFLAG_ADPCMDATA);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceAdpcmLoop(AXVPB* vpb, AXPBADPCMLOOP_t* adpcmLoop)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->adpcmLoop.loopScale = adpcmLoop->loopScale;
		if (internal->voiceType == 0)
		{
			internal->adpcmLoop.loopYn1 = adpcmLoop->loopYn1;
			internal->adpcmLoop.loopYn2 = adpcmLoop->loopYn2;
		}
		AXSetSyncFlag(vpb, AX_SYNCFLAG_ADPCMLOOP);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceSrc(AXVPB* vpb, AXPBSRC_t* src)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->src.ratioHigh = src->ratioHigh;
		internal->src.ratioLow = src->ratioLow;
		internal->src.currentFrac = src->currentFrac;
		internal->src.historySamples[0] = src->historySamples[0];
		internal->src.historySamples[1] = src->historySamples[1];
		internal->src.historySamples[2] = src->historySamples[2];
		internal->src.historySamples[3] = src->historySamples[3];
		AXResetSyncFlag(vpb, AX_SYNCFLAG_SRCRATIO);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_SRCDATA);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceSrcType(AXVPB* vpb, uint32 srcType)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		if (srcType == AX_SRC_TYPE_NONE)
		{
			internal->srcFilterMode = _swapEndianU16(AX_FILTER_MODE_NONE);
		}
		else if (srcType == AX_SRC_TYPE_LINEAR)
		{
			internal->srcFilterMode = _swapEndianU16(AX_FILTER_MODE_LINEAR);
		}
		else if (srcType == AX_SRC_TYPE_LOWPASS1)
		{
			internal->srcFilterMode = _swapEndianU16(AX_FILTER_MODE_TAP);
			internal->srcTapFilter = _swapEndianU16(AX_FILTER_LOWPASS_8K);
		}
		else if (srcType == AX_SRC_TYPE_LOWPASS2)
		{
			internal->srcFilterMode = _swapEndianU16(AX_FILTER_MODE_TAP);
			internal->srcTapFilter = _swapEndianU16(AX_FILTER_LOWPASS_12K);
		}
		else if (srcType == AX_SRC_TYPE_LOWPASS3)
		{
			internal->srcFilterMode = _swapEndianU16(AX_FILTER_MODE_TAP);
			internal->srcTapFilter = _swapEndianU16(AX_FILTER_LOWPASS_16K);
		}
		else
		{
			LOG("AXSetVoiceSrcType(): Unsupported src type %u", srcType);
		}
		AXSetSyncFlag(vpb, AX_SYNCFLAG_SRCFILTER);
		AXVoiceProtection_Acquire(vpb);
	}

	sint32 AXSetVoiceSrcRatio(AXVPB* vpb, float ratio)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		ratio *= 65536.0f;
		sint32 ratioI = (sint32)ratio;
		if (ratioI < 0)
			ratioI = 0;
		else if (ratioI > 0x80000)
			ratioI = 0x80000;

		uint16 ratioHigh = (uint16)(ratioI >> 16);
		uint16 ratioLow = (uint16)(ratioI & 0xFFFF);

		ratioHigh = _swapEndianU16(ratioHigh);
		ratioLow = _swapEndianU16(ratioLow);

		if (internal->src.ratioHigh != ratioHigh || internal->src.ratioLow != ratioLow)
		{
			internal->src.ratioHigh = ratioHigh;
			internal->src.ratioLow = ratioLow;
			AXSetSyncFlag(vpb, AX_SYNCFLAG_SRCRATIO);
			AXVoiceProtection_Acquire(vpb);
		}
		return 0;
	}

	void AXSetVoiceVe(AXVPB* vpb, AXPBVE* ve)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->veVolume = ve->currentVolume;
		internal->veDelta = ve->currentDelta;
		AXSetSyncFlag(vpb, AX_SYNCFLAG_VE);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXComputeLpfCoefs(uint32 freq, uint16be* a0, uint16be* b0)
	{
		// todo - verify algorithm
		float t1 = cos((float)freq / 32000.0f * 6.2831855f);
		float t2 = 2.0f - t1;
		t1 = (float)sqrt(t2 * t2 - 1.0f);
		t1 = t1 - t2;
		t1 = t1 * 32768.0f;
		t1 = -t1;
		uint32 r = (uint16)t1;
		*a0 = 0x7FFF - r;
		*b0 = r;
	}

	void AXSetVoiceLpf(AXVPB* vpb, AXPBLPF_t* lpf)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->lpf.on = lpf->on;
		internal->lpf.yn1 = lpf->yn1;
		internal->lpf.a0 = lpf->a0;
		internal->lpf.b0 = lpf->b0;
		AXSetSyncFlag(vpb, AX_SYNCFLAG_LPFDATA);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceLpfCoefs(AXVPB* vpb, uint16 a0, uint16 b0)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->lpf.a0 = _swapEndianU16(a0);
		internal->lpf.b0 = _swapEndianU16(b0);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_LPFCOEF);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceBiquad(AXVPB* vpb, AXPBBIQUAD_t* biquad)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->biquad.on = biquad->on;
		internal->biquad.xn1 = biquad->xn1;
		internal->biquad.xn2 = biquad->xn2;
		internal->biquad.yn1 = biquad->yn1;
		internal->biquad.yn2 = biquad->yn2;
		internal->biquad.b0 = biquad->b0;
		internal->biquad.b1 = biquad->b1;
		internal->biquad.b2 = biquad->b2;
		internal->biquad.a1 = biquad->a1;
		internal->biquad.a2 = biquad->a2;
		AXSetSyncFlag(vpb, AX_SYNCFLAG_BIQUADDATA);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceBiquadCoefs(AXVPB* vpb, uint16 b0, uint16 b1, uint16 b2, uint16 a1, uint16 a2)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;
		internal->biquad.b0 = _swapEndianU16(b0);
		internal->biquad.b1 = _swapEndianU16(b1);
		internal->biquad.b2 = _swapEndianU16(b2);
		internal->biquad.a1 = _swapEndianU16(a1);
		internal->biquad.a2 = _swapEndianU16(a2);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_BIQUADCOEF);
		AXVoiceProtection_Acquire(vpb);
	}

	void __AXSetVoiceAddr(AXVPB* vpb, axOffsetsInternal_t* voiceAddr)
	{
		sint32 voiceIndex = (sint32)(vpb->index);
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + voiceIndex;
		memcpy(&internal->internalOffsets, voiceAddr, sizeof(axOffsetsInternal_t));
		uint16 format = _swapEndianU16(voiceAddr->format);
		if (format == AX_FORMAT_PCM8)
		{
			memset(&internal->adpcmData, 0x00, sizeof(axADPCMInternal_t));
			internal->adpcmData.gain = 0x100;
			AXSetSyncFlag(vpb, AX_SYNCFLAG_ADPCMDATA);
			AXResetSyncFlag(vpb, AX_SYNCFLAG_LOOPFLAG | AX_SYNCFLAG_LOOPOFFSET | AX_SYNCFLAG_ENDOFFSET | AX_SYNCFLAG_CURRENTOFFSET);
			AXSetSyncFlag(vpb, AX_SYNCFLAG_OFFSETS);
			AXVoiceProtection_Acquire(vpb);
		}
		else if (format == AX_FORMAT_PCM16)
		{
			memset(&internal->adpcmData, 0x00, sizeof(axADPCMInternal_t));
			internal->adpcmData.gain = 0x800;
			AXSetSyncFlag(vpb, AX_SYNCFLAG_ADPCMDATA);
			AXResetSyncFlag(vpb, AX_SYNCFLAG_LOOPFLAG | AX_SYNCFLAG_LOOPOFFSET | AX_SYNCFLAG_ENDOFFSET | AX_SYNCFLAG_CURRENTOFFSET);
			AXSetSyncFlag(vpb, AX_SYNCFLAG_OFFSETS);
			AXVoiceProtection_Acquire(vpb);
		}
		else if (format == AX_FORMAT_ADPCM)
		{
			// .adpcmData is left intact
			AXResetSyncFlag(vpb, AX_SYNCFLAG_LOOPFLAG | AX_SYNCFLAG_LOOPOFFSET | AX_SYNCFLAG_ENDOFFSET | AX_SYNCFLAG_CURRENTOFFSET);
			AXSetSyncFlag(vpb, AX_SYNCFLAG_OFFSETS);
			AXVoiceProtection_Acquire(vpb);
		}
		else
		{
			cemu_assert_debug(false);
		}
	}

	void AXSetVoiceOffsets(AXVPB* vpb, AXPBOFFSET_t* pbOffset)
	{
		MPTR sampleBase = _swapEndianU32(pbOffset->samples);
		if (sampleBase == MPTR_NULL)
		{
			LOG("AXSetVoiceOffsets(): Invalid sample address");
			return;
		}
		AX_VOICE_PARAM_LOCK();
		memcpy(&vpb->offsets, pbOffset, sizeof(AXPBOFFSET_t));
		sampleBase = memory_virtualToPhysical(sampleBase);
		uint16 format = _swapEndianU16(pbOffset->format);

		axOffsetsInternal_t voiceAddr;
		if (format == AX_FORMAT_PCM8)
		{
			uint32 loopOffsetPtr = sampleBase + _swapEndianU32(pbOffset->loopOffset);
			uint32 endOffsetPtr = sampleBase + _swapEndianU32(pbOffset->endOffset);
			uint32 currentOffsetPtr = sampleBase + _swapEndianU32(pbOffset->currentOffset);
			voiceAddr.format = _swapEndianU16(pbOffset->format);
			voiceAddr.loopFlag = _swapEndianU16(pbOffset->loopFlag);
			voiceAddr.loopOffsetPtrHigh = (loopOffsetPtr >> 16) & 0x1FFF;
			voiceAddr.loopOffsetPtrLow = loopOffsetPtr & 0xFFFF;
			voiceAddr.endOffsetPtrHigh = (endOffsetPtr >> 16) & 0x1FFF;
			voiceAddr.endOffsetPtrLow = endOffsetPtr & 0xFFFF;
			voiceAddr.currentOffsetPtrHigh = (currentOffsetPtr >> 16) & 0x1FFF;
			voiceAddr.currentOffsetPtrLow = currentOffsetPtr & 0xFFFF;
			voiceAddr.ptrHighExtension = (currentOffsetPtr >> 29);
			// convert to big endian
			voiceAddr.format = _swapEndianU16(voiceAddr.format);
			voiceAddr.loopFlag = _swapEndianU16(voiceAddr.loopFlag);
			voiceAddr.loopOffsetPtrHigh = _swapEndianU16(voiceAddr.loopOffsetPtrHigh);
			voiceAddr.loopOffsetPtrLow = _swapEndianU16(voiceAddr.loopOffsetPtrLow);
			voiceAddr.endOffsetPtrHigh = _swapEndianU16(voiceAddr.endOffsetPtrHigh);
			voiceAddr.endOffsetPtrLow = _swapEndianU16(voiceAddr.endOffsetPtrLow);
			voiceAddr.currentOffsetPtrHigh = _swapEndianU16(voiceAddr.currentOffsetPtrHigh);
			voiceAddr.currentOffsetPtrLow = _swapEndianU16(voiceAddr.currentOffsetPtrLow);
			voiceAddr.ptrHighExtension = _swapEndianU16(voiceAddr.ptrHighExtension);
			__AXSetVoiceAddr(vpb, &voiceAddr);
		}
		else if (format == AX_FORMAT_PCM16)
		{
			uint32 loopOffsetPtr = sampleBase / 2 + _swapEndianU32(pbOffset->loopOffset);
			uint32 endOffsetPtr = sampleBase / 2 + _swapEndianU32(pbOffset->endOffset);
			uint32 currentOffset = _swapEndianU32(pbOffset->currentOffset);
			uint32 currentOffsetPtr = sampleBase / 2 + currentOffset;
			voiceAddr.format = _swapEndianU16(pbOffset->format);
			voiceAddr.loopFlag = _swapEndianU16(pbOffset->loopFlag);
			voiceAddr.loopOffsetPtrHigh = (loopOffsetPtr >> 16) & 0x0FFF;
			voiceAddr.loopOffsetPtrLow = loopOffsetPtr & 0xFFFF;
			voiceAddr.endOffsetPtrHigh = (endOffsetPtr >> 16) & 0x0FFF;
			voiceAddr.endOffsetPtrLow = endOffsetPtr & 0xFFFF;
			voiceAddr.currentOffsetPtrHigh = (currentOffsetPtr >> 16) & 0x0FFF;
			voiceAddr.currentOffsetPtrLow = currentOffsetPtr & 0xFFFF;
			voiceAddr.ptrHighExtension = ((sampleBase + currentOffset * 2) >> 29);
			// convert to big endian
			voiceAddr.format = _swapEndianU16(voiceAddr.format);
			voiceAddr.loopFlag = _swapEndianU16(voiceAddr.loopFlag);
			voiceAddr.loopOffsetPtrHigh = _swapEndianU16(voiceAddr.loopOffsetPtrHigh);
			voiceAddr.loopOffsetPtrLow = _swapEndianU16(voiceAddr.loopOffsetPtrLow);
			voiceAddr.endOffsetPtrHigh = _swapEndianU16(voiceAddr.endOffsetPtrHigh);
			voiceAddr.endOffsetPtrLow = _swapEndianU16(voiceAddr.endOffsetPtrLow);
			voiceAddr.currentOffsetPtrHigh = _swapEndianU16(voiceAddr.currentOffsetPtrHigh);
			voiceAddr.currentOffsetPtrLow = _swapEndianU16(voiceAddr.currentOffsetPtrLow);
			voiceAddr.ptrHighExtension = _swapEndianU16(voiceAddr.ptrHighExtension);
			__AXSetVoiceAddr(vpb, &voiceAddr);
		}
		else if (format == AX_FORMAT_ADPCM)
		{
			uint32 loopOffsetPtr = sampleBase * 2 + _swapEndianU32(pbOffset->loopOffset);
			uint32 endOffsetPtr = sampleBase * 2 + _swapEndianU32(pbOffset->endOffset);
			uint32 currentOffset = _swapEndianU32(pbOffset->currentOffset);
			uint32 currentOffsetPtr = sampleBase * 2 + currentOffset;
			voiceAddr.format = _swapEndianU16(pbOffset->format);
			voiceAddr.loopFlag = _swapEndianU16(pbOffset->loopFlag);
			voiceAddr.loopOffsetPtrHigh = (loopOffsetPtr >> 16) & 0x3FFF;
			voiceAddr.loopOffsetPtrLow = loopOffsetPtr & 0xFFFF;
			voiceAddr.endOffsetPtrHigh = (endOffsetPtr >> 16) & 0x3FFF;
			voiceAddr.endOffsetPtrLow = endOffsetPtr & 0xFFFF;
			voiceAddr.currentOffsetPtrHigh = (currentOffsetPtr >> 16) & 0x3FFF;
			voiceAddr.currentOffsetPtrLow = currentOffsetPtr & 0xFFFF;
			voiceAddr.ptrHighExtension = ((sampleBase + currentOffset / 2) >> 29);
			// convert to big endian
			voiceAddr.format = _swapEndianU16(voiceAddr.format);
			voiceAddr.loopFlag = _swapEndianU16(voiceAddr.loopFlag);
			voiceAddr.loopOffsetPtrHigh = _swapEndianU16(voiceAddr.loopOffsetPtrHigh);
			voiceAddr.loopOffsetPtrLow = _swapEndianU16(voiceAddr.loopOffsetPtrLow);
			voiceAddr.endOffsetPtrHigh = _swapEndianU16(voiceAddr.endOffsetPtrHigh);
			voiceAddr.endOffsetPtrLow = _swapEndianU16(voiceAddr.endOffsetPtrLow);
			voiceAddr.currentOffsetPtrHigh = _swapEndianU16(voiceAddr.currentOffsetPtrHigh);
			voiceAddr.currentOffsetPtrLow = _swapEndianU16(voiceAddr.currentOffsetPtrLow);
			voiceAddr.ptrHighExtension = _swapEndianU16(voiceAddr.ptrHighExtension);
			__AXSetVoiceAddr(vpb, &voiceAddr);
		}
		else
		{
			cemu_assert_debug(false);
		}
	}

	void AXSetVoiceOffsetsEx(AXVPB* vpb, AXPBOFFSET_t* pbOffset, void* sampleBase)
	{
		// used by F1 Racing
		if (vpb == nullptr || sampleBase == nullptr)
			return;
		AXPBOFFSET_t tmpOffsets = *pbOffset;
		tmpOffsets.samples = _swapEndianU32(memory_getVirtualOffsetFromPointer(sampleBase));
		AXSetVoiceOffsets(vpb, &tmpOffsets);
	}

	void AXSetVoiceSamplesAddr(AXVPB* vpb, void* sampleBase)
	{
		AX_VOICE_PARAM_LOCK();
		vpb->offsets.samples = _swapEndianU32(memory_getVirtualOffsetFromPointer(sampleBase));
		AXGetVoiceOffsets(vpb, &vpb->offsets);
	}

	void AXGetVoiceOffsets(AXVPB* vpb, AXPBOFFSET_t* pbOffset)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)vpb->index;

		memcpy(pbOffset, &vpb->offsets, sizeof(AXPBOFFSET_t));
		MPTR sampleBase = _swapEndianU32(vpb->offsets.samples);
		sampleBase = memory_virtualToPhysical(sampleBase);
		uint16 format = _swapEndianU16(pbOffset->format);
		uint32 loopOffsetPtrLow = _swapEndianU16(internal->internalOffsets.loopOffsetPtrLow);
		uint32 loopOffsetPtrHigh = _swapEndianU16(internal->internalOffsets.loopOffsetPtrHigh);
		uint32 endOffsetPtrLow = _swapEndianU16(internal->internalOffsets.endOffsetPtrLow);
		uint32 endOffsetPtrHigh = _swapEndianU16(internal->internalOffsets.endOffsetPtrHigh);
		uint32 currentOffsetPtrLow = _swapEndianU16(internal->internalOffsets.currentOffsetPtrLow);
		uint32 currentOffsetPtrHigh = _swapEndianU16(internal->internalOffsets.currentOffsetPtrHigh);
		uint32 ptrHighExtension = _swapEndianU16(internal->internalOffsets.ptrHighExtension);

		if (format == AX_FORMAT_PCM8)
		{
			uint32 loopOffset = (loopOffsetPtrLow | (loopOffsetPtrHigh << 16) | (ptrHighExtension << 29)) - sampleBase;
			uint32 endOffset = (endOffsetPtrLow | (endOffsetPtrHigh << 16) | (ptrHighExtension << 29)) - sampleBase;
			uint32 currentOffset = (currentOffsetPtrLow | (currentOffsetPtrHigh << 16) | (ptrHighExtension << 29)) - sampleBase;

			pbOffset->loopOffset = _swapEndianU32(loopOffset);
			pbOffset->endOffset = _swapEndianU32(endOffset);
			pbOffset->currentOffset = _swapEndianU32(currentOffset);
		}
		else if (format == AX_FORMAT_PCM16)
		{
			uint32 loopOffset = (loopOffsetPtrLow | (loopOffsetPtrHigh << 16) | ((ptrHighExtension << 29) / 2)) - sampleBase / 2;
			uint32 endOffset = (endOffsetPtrLow | (endOffsetPtrHigh << 16) | ((ptrHighExtension << 29) / 2)) - sampleBase / 2;
			uint32 currentOffset = (currentOffsetPtrLow | (currentOffsetPtrHigh << 16) | ((ptrHighExtension << 29) / 2)) - sampleBase / 2;

			pbOffset->loopOffset = _swapEndianU32(loopOffset);
			pbOffset->endOffset = _swapEndianU32(endOffset);
			pbOffset->currentOffset = _swapEndianU32(currentOffset);
		}
		else if (format == AX_FORMAT_ADPCM)
		{
			uint32 loopOffset = (loopOffsetPtrLow | (loopOffsetPtrHigh << 16) | ((ptrHighExtension << 29) * 2)) - sampleBase * 2;
			uint32 endOffset = (endOffsetPtrLow | (endOffsetPtrHigh << 16) | ((ptrHighExtension << 29) * 2)) - sampleBase * 2;
			uint32 currentOffset = (currentOffsetPtrLow | (currentOffsetPtrHigh << 16) | ((ptrHighExtension << 29) * 2)) - sampleBase * 2;

			pbOffset->loopOffset = _swapEndianU32(loopOffset);
			pbOffset->endOffset = _swapEndianU32(endOffset);
			pbOffset->currentOffset = _swapEndianU32(currentOffset);
		}
		else
		{
			cemu_assert_debug(false);
		}
	}

	void AXGetVoiceOffsetsEx(AXVPB* vpb, AXPBOFFSET_t* pbOffset, MPTR sampleBase)
	{
		if (vpb == nullptr || sampleBase == MPTR_NULL)
			return;
		AX_VOICE_PARAM_LOCK();
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		AXGetVoiceOffsets(vpb, pbOffset);
		memcpy(&vpb->offsets, pbOffset, sizeof(AXPBOFFSET_t));
	}

	void AXSetVoiceCurrentOffset(AXVPB* vpb, uint32 currentOffset)
	{
		// untested
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)(vpb->index);
		MPTR sampleBase = memory_virtualToPhysical(_swapEndianU32(vpb->offsets.samples));
		vpb->offsets.currentOffset = _swapEndianU32(currentOffset);
		uint16 voiceFormat = _swapEndianU16(internal->internalOffsets.format);
		uint32 currentOffsetPtr = 0;
		sampleBase &= 0x1FFFFFFF;
		if (voiceFormat == AX_FORMAT_PCM8)
		{
			// calculate offset (in bytes)
			currentOffsetPtr = sampleBase + currentOffset;
		}
		else if (voiceFormat == AX_FORMAT_PCM16)
		{
			// calculate offset (in shorts)
			currentOffsetPtr = sampleBase / 2 + currentOffset;
		}
		else if (voiceFormat == AX_FORMAT_ADPCM)
		{
			// calculate offset (in nibbles)
			currentOffsetPtr = sampleBase * 2 + currentOffset;
		}
		else
		{
			cemu_assert_debug(false);
		}
		internal->internalOffsets.currentOffsetPtrHigh = _swapEndianU16(currentOffsetPtr >> 16);
		internal->internalOffsets.currentOffsetPtrLow = _swapEndianU16(currentOffsetPtr & 0xFFFF);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_CURRENTOFFSET);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceLoopOffset(AXVPB* vpb, uint32 loopOffset)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)(vpb->index);
		MPTR sampleBase = memory_virtualToPhysical(_swapEndianU32(vpb->offsets.samples));
		vpb->offsets.loopOffset = _swapEndianU32(loopOffset);
		uint16 voiceFormat = _swapEndianU16(internal->internalOffsets.format);
		uint32 loopOffsetPtr = 0;
		sampleBase &= 0x1FFFFFFF;
		if (voiceFormat == AX_FORMAT_PCM8)
		{
			// calculate offset (in bytes)
			loopOffsetPtr = sampleBase + loopOffset;
		}
		else if (voiceFormat == AX_FORMAT_PCM16)
		{
			// calculate offset (in shorts)
			loopOffsetPtr = sampleBase / 2 + loopOffset;
		}
		else if (voiceFormat == AX_FORMAT_ADPCM)
		{
			// calculate offset (in nibbles)
			loopOffsetPtr = sampleBase * 2 + loopOffset;
		}
		else
		{
			cemu_assert_debug(false);
		}
		internal->internalOffsets.loopOffsetPtrHigh = _swapEndianU16(loopOffsetPtr >> 16);
		internal->internalOffsets.loopOffsetPtrLow = _swapEndianU16(loopOffsetPtr & 0xFFFF);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_LOOPOFFSET);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceEndOffset(AXVPB* vpb, uint32 endOffset)
	{
		// untested
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)(vpb->index);
		MPTR sampleBase = memory_virtualToPhysical(_swapEndianU32(vpb->offsets.samples));
		vpb->offsets.endOffset = _swapEndianU32(endOffset);
		uint16 voiceFormat = _swapEndianU16(internal->internalOffsets.format);
		uint32 endOffsetPtr = 0;
		sampleBase &= 0x1FFFFFFF;
		if (voiceFormat == AX_FORMAT_PCM8)
		{
			// calculate offset (in bytes)
			endOffsetPtr = sampleBase + endOffset;
		}
		else if (voiceFormat == AX_FORMAT_PCM16)
		{
			// calculate offset (in shorts)
			endOffsetPtr = sampleBase / 2 + endOffset;
		}
		else if (voiceFormat == AX_FORMAT_ADPCM)
		{
			// calculate offset (in nibbles)
			endOffsetPtr = sampleBase * 2 + endOffset;
		}
		else
		{
			cemu_assert_debug(false);
		}
		internal->internalOffsets.endOffsetPtrHigh = _swapEndianU16(endOffsetPtr >> 16);
		internal->internalOffsets.endOffsetPtrLow = _swapEndianU16(endOffsetPtr & 0xFFFF);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_ENDOFFSET);
		AXVoiceProtection_Acquire(vpb);
	}

	void AXSetVoiceCurrentOffsetEx(AXVPB* vpb, uint32 currentOffset, MPTR sampleBase)
	{
		if (vpb == nullptr || sampleBase == MPTR_NULL)
			return;
		AX_VOICE_PARAM_LOCK();
		AXPBOFFSET_t pbOffset;
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		AXGetVoiceOffsets(vpb, &pbOffset);
		AXSetVoiceCurrentOffset(vpb, currentOffset);
	}

	void AXSetVoiceLoopOffsetEx(AXVPB* vpb, uint32 loopOffset, MPTR sampleBase)
	{
		if (vpb == nullptr || sampleBase == MPTR_NULL)
			return;
		AX_VOICE_PARAM_LOCK();
		AXPBOFFSET_t pbOffset;
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		AXGetVoiceOffsets(vpb, &pbOffset);
		AXSetVoiceLoopOffset(vpb, loopOffset);
	}

	void AXSetVoiceEndOffsetEx(AXVPB* vpb, uint32 endOffset, MPTR sampleBase)
	{
		if (vpb == nullptr || sampleBase == MPTR_NULL)
			return;
		AX_VOICE_PARAM_LOCK();
		AXPBOFFSET_t pbOffset;
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		AXGetVoiceOffsets(vpb, &pbOffset);
		AXSetVoiceEndOffset(vpb, endOffset);
	}

	uint32 AXGetVoiceCurrentOffsetEx(AXVPB* vpb, MPTR sampleBase)
	{
		if (vpb == nullptr || sampleBase == MPTR_NULL)
			return 0;
		AXPBOFFSET_t pbOffset;
		AXGetVoiceOffsetsEx(vpb, &pbOffset, sampleBase);
		return _swapEndianU32(pbOffset.currentOffset);
	}

	void AXSetVoiceLoop(AXVPB* vpb, uint16 loopState)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)(vpb->index);
		vpb->offsets.loopFlag = _swapEndianU16(loopState);
		internal->internalOffsets.loopFlag = _swapEndianU16(loopState);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_LOOPFLAG);
		AXVoiceProtection_Acquire(vpb);
	}

	uint32 vpbLoopTracker_loopCount[AX_MAX_VOICES];
	uint32 vpbLoopTracker_prevCurrentOffset[AX_MAX_VOICES];

	void AXResetVoiceLoopCount(AXVPB* vpb)
	{
		if (!vpb)
			return;
		uint32 voiceIndex = vpb->index;
		vpbLoopTracker_loopCount[voiceIndex] = 0;
		vpbLoopTracker_prevCurrentOffset[voiceIndex] = 0;
	}

	sint32 AXGetVoiceLoopCount(AXVPB* vpb)
	{
		if (!vpb)
			return 0;
		AX_VOICE_PARAM_LOCK();
		uint32 voiceIndex = vpb->index;
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + voiceIndex;

		uint32 loopOffset = internal->internalOffsets.GetLoopOffset32();
		uint32 endOffset = internal->internalOffsets.GetEndOffset32();
		uint32 currentOffset = internal->internalOffsets.GetCurrentOffset32();

		uint32 srcRatio;
		if (internal->srcFilterMode == AX_FILTER_MODE_NONE)
			srcRatio = 0x10000;
		else
			srcRatio = internal->src.GetSrcRatio32();

		uint32 loopLength = 0;
		if (srcRatio != 0)
			loopLength = ((endOffset - loopOffset) * 0x10000) / srcRatio;

		uint32 prevCurrentOffset = vpbLoopTracker_prevCurrentOffset[voiceIndex];
		uint32 voiceSamplesPerFrame = AXGetInputSamplesPerFrame();

		if (loopOffset < endOffset && srcRatio != 0 && loopLength <= voiceSamplesPerFrame)
		{
			// handle loops shorter than one frame
			uint32 loopsInFrame = loopLength ? ((prevCurrentOffset - loopOffset) + voiceSamplesPerFrame) / loopLength : 0;
			vpbLoopTracker_loopCount[voiceIndex] += loopsInFrame;
		}
		else // loopOffset >= endOffset
		{
			if (prevCurrentOffset <= endOffset) // loop could only happen if playback cursor was before end
			{
				if (loopOffset > endOffset && currentOffset >= loopOffset)
					vpbLoopTracker_loopCount[voiceIndex]++;
				else if (currentOffset < prevCurrentOffset && currentOffset >= loopOffset)
					vpbLoopTracker_loopCount[voiceIndex]++;
			}
		}
		vpbLoopTracker_prevCurrentOffset[voiceIndex] = currentOffset;
		return vpbLoopTracker_loopCount[voiceIndex];
	}

	// ---- exports BotW imports that Cemu does not implement ----

	// Moves the voice to the list of another priority (the stack AXAcquireVoiceEx drops voices from).
	void AXSetVoicePriority(AXVPB* vpb, uint32 priority)
	{
		if (vpb == nullptr || priority == AX_PRIORITY_FREE || priority >= AX_PRIORITY_MAX)
			return;
		std::lock_guard<std::recursive_mutex> listLock(__AXVoiceListSpinlock);
		if (vpb->priority == (uint32be)AX_PRIORITY_FREE)
			return;
		AXVoiceList_RemoveVoice(vpb);
		AXVoiceList_AddVoice(vpb, priority);
	}

	// The per-frame sync copies vpb->mixerSelect into the internal voice (AXIst_SyncVPB). All voices
	// are mixed on the "PPC" path in this implementation, as in Cemu. Returns the previous value.
	uint32 AXSetVoiceMixerSelect(AXVPB* vpb, uint32 mixerSelect)
	{
		AX_VOICE_PARAM_LOCK();
		uint32 prev = vpb->mixerSelect;
		vpb->mixerSelect = mixerSelect;
		return prev;
	}

	// Remote speaker routing. Stored and synced like Cemu's AX_SYNCFLAG_VOICEREMOTEON guess; the
	// remote mix itself is not produced (see AXRmt* in ax_out.cpp).
	void AXSetVoiceRmtOn(AXVPB* vpb, uint16 on)
	{
		AX_VOICE_PARAM_LOCK();
		AXVPBInternal_t* internal = __AXVPBInternalVoiceArray + (sint32)(vpb->index);
		internal->reserved148_voiceRmtOn = _swapEndianU16(on);
		AXSetSyncFlag(vpb, AX_SYNCFLAG_VOICEREMOTEON);
		AXVoiceProtection_Acquire(vpb);
	}

	// Remote speaker IIR filter: only affects the (not produced) remote mix.
	void AXSetVoiceRmtIIR(AXVPB* vpb, void* iir)
	{
	}

	// void AXSetVoiceRmtIIRCoefs(AXVPB*, u16 type, ...): coefficients are varargs; ignored like the IIR itself.
	void AXSetVoiceRmtIIRCoefs(AXVPB* vpb, uint16 type)
	{
	}
}
