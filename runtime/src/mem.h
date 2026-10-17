// Guest address space: 4 GB mapped at g_mem (big-endian data). Layout follows Cafe OS / Cemu:
//   0x02000000 text, 0x0E000000 runtime area (HLE thunks + sys heap), 0x10000000 MEM2 (1 GB),
//   0xC0000000 RPX import stubs, 0xE0000000 foreground bucket, 0xF4000000 MEM1 (32 MB),
//   0xFFC00000 locked caches.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ppc.h"

namespace mem {
constexpr uint32_t kText = 0x02000000;
constexpr uint32_t kDataImports = 0x0DF00000;  // fixed 4 KB slots for OS-exported variables
constexpr uint32_t kThunkBase = 0x0E000000, kThunkSize = 0x00100000;   // 4 bytes per HLE thunk
constexpr uint32_t kSysBase = 0x0E100000, kSysEnd = 0x10000000;          // runtime-owned guest memory
constexpr uint32_t kMem2 = 0x10000000, kMem2Size = 0x40000000;
constexpr uint32_t kFgBucket = 0xE0000000, kFgBucketSize = 0x04000000;
constexpr uint32_t kMem1 = 0xF4000000, kMem1Size = 0x02000000;
constexpr uint32_t kLockedCache = 0xFFC00000;

void init();
// Runtime-owned guest allocations (OS structures, thread stacks, strings handed to the game).
uint32_t sys_alloc(uint32_t size, uint32_t align = 16);
void sys_free(uint32_t addr);
uint32_t sys_strdup(const char* s);

inline uint8_t* host(uint32_t a) { return g_mem + a; }
inline uint32_t guest(const void* p) { return (uint32_t)((const uint8_t*)p - g_mem); }

inline uint8_t r8(uint32_t a) { return g_mem[a]; }
inline uint16_t r16(uint32_t a) { return ld16_(g_mem, a); }
inline uint32_t r32(uint32_t a) { return ld32_(g_mem, a); }
inline uint64_t r64(uint32_t a) { return ld64_(g_mem, a); }
inline void w8(uint32_t a, uint8_t v) { g_mem[a] = v; }
inline void w16(uint32_t a, uint16_t v) { st16_(g_mem, a, v); }
inline void w32(uint32_t a, uint32_t v) { st32_(g_mem, a, v); }
inline void w64(uint32_t a, uint64_t v) { st64_(g_mem, a, v); }
inline float rf32(uint32_t a) { uint32_t v = r32(a); float f; memcpy(&f, &v, 4); return f; }
inline void wf32(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); w32(a, v); }
inline const char* str(uint32_t a) { return a ? (const char*)(g_mem + a) : nullptr; }
}  // namespace mem
