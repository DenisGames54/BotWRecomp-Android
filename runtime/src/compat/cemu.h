// Compatibility layer for code ported from Cemu (MPL-2.0): Cemu's integer types, big-endian
// wrappers, guest pointers and the handful of runtime services that code expects, mapped onto
// this runtime (g_mem, mem::sys_alloc, guest_call, LOG).
#pragma once
#include <concepts>
#include <mutex>
#include <span>

#include "../latte/isa/prelude.h"
#include "../log.h"
#include "../mem.h"
#include "../hle.h"

typedef uint32 MPTR;
typedef float float32;

extern uint8* memory_base;  // == g_mem (set by mem::init)

inline uint16 _swapEndianU16(uint16 v) { return __builtin_bswap16(v); }
inline sint16 _swapEndianS16(sint16 v) { return (sint16)__builtin_bswap16((uint16)v); }
inline sint32 _swapEndianS32(sint32 v) { return (sint32)__builtin_bswap32((uint32)v); }
inline uint64 _swapEndianU64(uint64 v) { return __builtin_bswap64(v); }
inline float _swapEndianF32(float v) {
    uint32 u;
    memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    memcpy(&v, &u, 4);
    return v;
}

#include "betype.h"
#include "enumFlags.h"
#include "MemPtr.h"

inline uint8* memory_getPointerFromVirtualOffset(MPTR a) { return g_mem + a; }
inline uint8* memory_getPointerFromVirtualOffsetAllowNull(MPTR a) { return a ? g_mem + a : nullptr; }
inline MPTR memory_getVirtualOffsetFromPointer(const void* p) { return p ? (MPTR)((const uint8*)p - g_mem) : 0; }
// The GPU side of this runtime works on guest virtual addresses, so "physical" is identity.
inline MPTR memory_virtualToPhysical(MPTR a) { return a; }
inline MPTR memory_physicalToVirtual(MPTR a) { return a; }
inline uint32 memory_readU32(MPTR a) { return mem::r32(a); }
inline uint16 memory_readU16(MPTR a) { return mem::r16(a); }
inline uint8 memory_readU8(MPTR a) { return mem::r8(a); }
inline float memory_readFloat(MPTR a) { return mem::rf32(a); }
inline void memory_writeU32(MPTR a, uint32 v) { mem::w32(a, v); }
inline void memory_writeU16(MPTR a, uint16 v) { mem::w16(a, v); }
inline void memory_writeU8(MPTR a, uint8 v) { mem::w8(a, v); }
inline void memory_writeFloat(MPTR a, float v) { mem::wf32(a, v); }
inline void memcpy_dwords(void* dst, const void* src, uint32 n) { memcpy(dst, src, n * 4); }

// Logging/asserts used throughout Cemu code
#define cemuLog_log(type, ...) ((void)0)
#define cemuLog_logDebug(type, ...) ((void)0)
#define cemuLog_logOnce(type, ...) ((void)0)
#define cemuLog_logDebugOnce(type, ...) ((void)0)
#define cemu_assert_suspicious() ((void)0)
#define cemu_assert_unimplemented() LOG("unimplemented path at %s:%d", __FILE__, __LINE__)
#define debug_printf(...) ((void)0)
#define assert_dbg() ((void)0)
#define debugBreakpoint() ((void)0)

// Cemu's generic HAS_FLAG (all bits of test_flag set). The per-enum overloads from DEFINE_ENUM_FLAG_OPERATORS
// (prelude.h) are non-templates and win for those types; this covers ENABLE_BITMASK_OPERATORS enums.
template <typename T1, typename T2>
constexpr bool HAS_FLAG(T1 flags, T2 test_flag) {
    return (flags & (T1)test_flag) == (T1)test_flag;
}

// Guest memory allocated on first use from the runtime's sys heap (Cemu's SysAllocator).
template <typename T, size_t count = 1, size_t alignment = 32>
class SysAllocator {
   public:
    T* GetPtr() {
        if (!m_addr) {
            std::lock_guard<std::mutex> l(m_lock);
            if (!m_addr) m_addr = mem::sys_alloc((uint32)(sizeof(T) * count), (uint32)alignment);
        }
        return (T*)(g_mem + m_addr);
    }
    MPTR GetMPTR() { GetPtr(); return m_addr; }
    T* operator->() { return GetPtr(); }
    T& operator*() { return *GetPtr(); }
    operator T*() { return GetPtr(); }
    T& operator[](size_t i) { return GetPtr()[i]; }
    operator MEMPTR<T>() { return MEMPTR<T>(GetPtr()); }

   private:
    uint32 m_addr = 0;
    std::mutex m_lock;
};

// Call guest code from host code on the current guest thread (Cemu's PPCCoreCallback).
namespace cemu_compat {
template <typename T>
uint32 to_u32(T v) {
    if constexpr (std::is_pointer_v<T>) return memory_getVirtualOffsetFromPointer((const void*)v);
    else if constexpr (std::is_enum_v<T>) return (uint32)v;
    else if constexpr (std::is_integral_v<T>) return (uint32)v;
    else return (uint32)v.GetMPTR();  // MEMPTR
}
}  // namespace cemu_compat
uint32 cemu_core_callback(MPTR fn, std::initializer_list<uint32> args);
template <typename F, typename... A>
uint32 PPCCoreCallback(F fn, A... args) {
    return cemu_core_callback(cemu_compat::to_u32(fn), {cemu_compat::to_u32(args)...});
}
