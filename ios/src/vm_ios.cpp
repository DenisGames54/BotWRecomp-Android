// iOS virtual-address budget. Without com.apple.developer.kernel.extended-virtual-addressing (paid
// Apple Developer Program teams only) an app on a > 3 GB iPhone gets VA up to 0x4_5800_0000 (xnu
// ARM64_MAX_OFFSET_DEVICE_LARGE), i.e. ~7 GB usable around the shared cache. The 4 GB guest space takes
// most of it, so 64 MB host stacks for ~40 guest threads (2.5 GB) would not fit. Probe once: if a
// 32 GB reservation succeeds the process has the big ("jumbo") map and stacks stay as requested,
// otherwise they are capped at 16 MB. Used by runtime/src/thread.cpp and snd/ax_ist.cpp (TARGET_OS_IOS).
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cstddef>

#include "log.h"

size_t botw_ios_stack_size(size_t want) {
    static const bool big_va = [] {
        const size_t probe = 32ull << 30;
        void* p = mmap(nullptr, probe, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return false;
        munmap(p, probe);
        return true;
    }();
    size_t got = big_va ? want : std::min<size_t>(want, 16u << 20);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        LOG("ios: %s address space, guest thread host stacks %zu MB", big_va ? "extended" : "default (no extended VA)",
            got >> 20);
    return got;
}
