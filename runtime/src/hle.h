// HLE conventions. An HLE function has the guest ABI: args in r3..r10 / f1..f8, result in r3
// (r3:r4 for 64-bit), and is named hle_<lib>_<export>, matching tools/recompile.py.
//
//   HLE_EXPORT(coreinit, OSReport) { ... }   // also registered by name for OSDynLoad
#pragma once
#include <initializer_list>

#include "log.h"
#include "mem.h"
#include "ppc.h"

struct HleRegistration {
    HleRegistration(const char* lib, const char* name, GuestFn fn);
};

#define HLE_EXPORT(lib, name)                                                                   \
    extern "C" void hle_##lib##_##name(PPCContext* c);                                         \
    static HleRegistration hle_reg_##lib##_##name(#lib, #name, hle_##lib##_##name);            \
    extern "C" void hle_##lib##_##name(PPCContext* c)

#define ARG(n) (c->r[3 + (n)])
#define ARGF(n) (c->f[1 + (n)])
#define RET(v) (c->r[3] = (uint32_t)(v))
#define RET64(v) do { uint64_t v_ = (uint64_t)(v); c->r[3] = (uint32_t)(v_ >> 32); c->r[4] = (uint32_t)v_; } while (0)
#define RETF(v) (c->f[1] = (double)(v))
// 64-bit argument starting at GPR pair rN:rN+1 (pairs start at an odd register: r3, r5, r7, r9)
#define ARG64(n) (((uint64_t)c->r[3 + (n)] << 32) | c->r[4 + (n)])

namespace hle {
GuestFn find(const char* lib, const char* name);  // registered HLE export or nullptr
}

namespace dispatch {
void init();
GuestFn lookup(uint32_t addr);
// A guest-callable address for a host function (function pointers handed to the game).
uint32_t thunk(GuestFn fn, const char* name);
// Thunk for a registered export, or an "unimplemented" trap thunk carrying the name.
uint32_t export_thunk(const char* lib, const char* name);
const char* thunk_name(uint32_t addr);
void add_import_stub(uint32_t addr, GuestFn fn, const char* lib, const char* name);
}  // namespace dispatch

// Call guest code at `fn` with integer args in r3.. from HLE code; returns r3. The caller's
// registers are preserved (the guest callee may clobber volatiles).
uint32_t guest_call(PPCContext* c, uint32_t fn, std::initializer_list<uint32_t> args = {});
