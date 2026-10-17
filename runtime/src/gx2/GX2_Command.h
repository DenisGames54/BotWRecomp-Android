// GX2 command writing (Cemu-compatible API, own implementation in gx2_core.cpp).
// Commands go to the calling thread's current write target: a chunk of the GX2 command pool
// or, between GX2BeginDisplayList/GX2EndDisplayList, the game's display list buffer.
#pragma once
#include "../compat/cemu.h"
#include "../latte/isa/LatteReg.h"
#include "../latte/latte.h"

#define pm4HeaderType3(__itCode, __dataDWordCount) (0xC0000000 | ((uint32)(__itCode) << 8) | ((uint32)((__dataDWordCount)-1) << 16))
#define pm4HeaderType2Filler() (0x80000000)
using namespace latte;  // IT_* opcodes

namespace GX2 {
struct GX2PerCoreCBState {
    uint32be* bufferPtr = nullptr;
    uint32 bufferSizeInU32s = 0;
    uint32be* currentWritePtr = nullptr;
    bool isDisplayList = false;
};
GX2PerCoreCBState& cb();  // this thread's write target

void GX2Command_Flush(uint32 numU32sForNextBuffer, bool triggerMarkerInterrupt = true);
void GX2ReserveCmdSpace(uint32 reservedFreeSpaceInU32);
uint64 GX2GetLastSubmittedTimeStamp();
uint64 GX2GetRetiredTimeStamp();
bool GX2WaitTimeStamp(uint64 tsWait);
void GX2BeginDisplayList(MEMPTR<void> displayListAddr, uint32 size);
void GX2BeginDisplayListEx(MEMPTR<void> displayListAddr, uint32 size, bool profiling);
uint32 GX2EndDisplayList(MEMPTR<void> displayListAddr);
void GX2CallDisplayList(MPTR addr, uint32 size);
void GX2DirectCallDisplayList(void* addr, uint32 size);
bool GX2GetDisplayListWriteStatus();
bool GX2DrawDone();
void GX2Flush();
extern uint32 sGX2MainCoreIndex;
}  // namespace GX2

void gx2WriteGather_submitU32AsBE(uint32 v);
void gx2WriteGather_submitU32AsLE(uint32 v);
void gx2WriteGather_submitU32AsLEArray(uint32* v, uint32 numValues);

template <typename... Targs>
inline void gx2WriteGather_submit_(uint32be* writePtr) {
    GX2::cb().currentWritePtr = writePtr;
}
template <typename T, typename... Targs>
inline void gx2WriteGather_submit_(uint32be* writePtr, const betype<T>& arg, Targs... args) {
    static_assert(sizeof(betype<T>) == sizeof(uint32be));
    *(betype<T>*)writePtr = arg;
    gx2WriteGather_submit_(writePtr + 1, args...);
}
template <typename T, typename... Targs>
    requires std::is_floating_point_v<T>
inline void gx2WriteGather_submit_(uint32be* writePtr, const T& arg, Targs... args) {
    static_assert(sizeof(T) == sizeof(uint32));
    uint32 u;
    memcpy(&u, &arg, 4);
    *writePtr = u;
    gx2WriteGather_submit_(writePtr + 1, args...);
}
template <typename T, typename... Targs>
    requires std::is_base_of_v<Latte::LATTEREG, T>
inline void gx2WriteGather_submit_(uint32be* writePtr, const T& arg, Targs... args) {
    *writePtr = arg.getRawValue();
    gx2WriteGather_submit_(writePtr + 1, args...);
}
template <typename T, typename... Targs>
    requires(!std::is_base_of_v<Latte::LATTEREG, T>) && (!std::is_floating_point_v<T>)
inline void gx2WriteGather_submit_(uint32be* writePtr, const T& arg, Targs... args) {
    *writePtr = (uint32)arg;
    gx2WriteGather_submit_(writePtr + 1, args...);
}
template <typename... Targs>
inline void gx2WriteGather_submit(Targs... args) {
    uint32be* p = GX2::cb().currentWritePtr;
    if (!p) return;
    gx2WriteGather_submit_(p, std::forward<Targs>(args)...);
}
