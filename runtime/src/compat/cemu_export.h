// Typed HLE exports for code ported from Cemu: unpacks PPC SysV arguments (r3..r10, f1..f8,
// 64-bit values in odd/even GPR pairs) into a C++ function's parameters and stores the result.
//
//   CAFE_EXPORT(gx2, GX2SetViewport, GX2::GX2SetViewport)
// defines hle_gx2_GX2SetViewport (the symbol recompiled code calls) and registers it by name.
#pragma once
#include <tuple>
#include <type_traits>

#include "cemu.h"

namespace cemu_export {
struct ArgState {
    PPCContext* c;
    int gpr = 3;
    int fpr = 1;
    uint32 stack = 0;  // overflow area (r1 + 8)
    uint32 next32() {
        if (gpr <= 10) return c->r[gpr++];
        uint32 v = mem::r32(stack);
        stack += 4;
        return v;
    }
    uint64 next64() {
        if (gpr % 2 == 0) gpr++;
        if (gpr <= 9) {
            uint64 v = ((uint64)c->r[gpr] << 32) | c->r[gpr + 1];
            gpr += 2;
            return v;
        }
        gpr = 11;
        stack = (stack + 7) & ~7u;
        uint64 v = mem::r64(stack);
        stack += 8;
        return v;
    }
    double nextf() {
        if (fpr <= 8) return c->f[fpr++];
        stack = (stack + 7) & ~7u;
        double v = bitsd(mem::r64(stack));
        stack += 8;
        return v;
    }
};

template <typename T>
struct is_memptr : std::false_type {};
template <typename T>
struct is_memptr<MEMPTR<T>> : std::true_type {};
template <typename T>
struct is_betype : std::false_type {};
template <typename T>
struct is_betype<betype<T>> : std::true_type {};

template <typename T>
T get_arg(ArgState& s) {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_floating_point_v<U>) return (U)s.nextf();
    else if constexpr (std::is_pointer_v<U>) {
        uint32 a = s.next32();
        return a ? (U)(g_mem + a) : nullptr;
    } else if constexpr (is_memptr<U>::value) return U(s.next32());
    else if constexpr (is_betype<U>::value) return U(s.next32());
    else if constexpr (std::is_same_v<U, bool>) return s.next32() != 0;
    else if constexpr (std::is_enum_v<U>) {
        if constexpr (sizeof(U) == 8) return (U)s.next64();
        else return (U)s.next32();
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (sizeof(U) == 8) return (U)s.next64();
        else return (U)s.next32();
    } else {
        static_assert(sizeof(U) == 0, "unsupported HLE argument type");
    }
}

template <typename R>
void set_ret(PPCContext* c, R v) {
    if constexpr (std::is_floating_point_v<R>) c->f[1] = (double)v;
    else if constexpr (std::is_pointer_v<R>) c->r[3] = memory_getVirtualOffsetFromPointer((const void*)v);
    else if constexpr (is_memptr<R>::value) c->r[3] = v.GetMPTR();
    else if constexpr (is_betype<R>::value) c->r[3] = (uint32)v.value();
    else if constexpr (sizeof(R) == 8) {
        c->r[3] = (uint32)((uint64)v >> 32);
        c->r[4] = (uint32)(uint64)v;
    } else c->r[3] = (uint32)v;
}

template <typename F>
struct Adapter;
template <typename R, typename... A>
struct Adapter<R (*)(A...)> {
    template <R (*F)(A...)>
    static void call(PPCContext* c) {
        ArgState s{c, 3, 1, c->r[1] + 8};
        std::tuple<std::remove_cv_t<std::remove_reference_t<A>>...> args{get_arg<A>(s)...};  // braced: left to right
        if constexpr (std::is_void_v<R>) std::apply(F, args);
        else set_ret<R>(c, std::apply(F, args));
    }
};
}  // namespace cemu_export

#define CAFE_EXPORT(lib, name, fn)                                                                        \
    extern "C" void hle_##lib##_##name(PPCContext* c) {                                                  \
        cemu_export::Adapter<decltype(&fn)>::template call<&fn>(c);                                      \
    }                                                                                                    \
    static HleRegistration hle_reg_##lib##_##name(#lib, #name, hle_##lib##_##name)
