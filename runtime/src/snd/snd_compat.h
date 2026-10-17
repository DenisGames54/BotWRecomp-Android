// Small helpers the snd_core (AX) port needs on top of compat/cemu.h.
#pragma once
#include <algorithm>
#include <mutex>
#include <vector>

#include "../compat/cemu.h"
#include "../thread.h"

#ifndef FORCE_INLINE
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace snd_core {
// Cemu's vectorRemoveByValue (Common/precompiled.h)
template <typename T>
void vectorRemoveByValue(std::vector<T>& vec, const T& val) {
    vec.erase(std::remove(vec.begin(), vec.end(), val), vec.end());
}

// coreinit::OSGetCurrentThread() as a guest pointer (0 on plain host threads)
inline MPTR AXGetCurrentThreadMPTR() {
    threads::GuestThread* t = threads::current();
    return t ? t->os : MPTR_NULL;
}
}  // namespace snd_core
