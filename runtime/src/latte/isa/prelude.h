// Minimal type prelude for the vendored Latte hardware definitions (from Cemu, MPL-2.0).
#pragma once
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;
typedef uint64_t uint64;
typedef int64_t sint64;
#ifndef cemu_assert_debug
#define cemu_assert_debug(x) ((void)0)
#define cemu_assert(x) ((void)0)
#endif
#include <stddef.h>
#include <type_traits>
#include <span>
inline uint32 _swapEndianU32(uint32 v) { return __builtin_bswap32(v); }
#define DEFINE_ENUM_FLAG_OPERATORS(T)                                                                                 \
    inline T operator|(T a, T b) { return (T)((std::underlying_type_t<T>)a | (std::underlying_type_t<T>)b); }         \
    inline T operator&(T a, T b) { return (T)((std::underlying_type_t<T>)a & (std::underlying_type_t<T>)b); }         \
    inline T operator^(T a, T b) { return (T)((std::underlying_type_t<T>)a ^ (std::underlying_type_t<T>)b); }         \
    inline T operator~(T a) { return (T)(~(std::underlying_type_t<T>)a); }                                            \
    inline T& operator|=(T& a, T b) { return a = a | b; }                                                             \
    inline T& operator&=(T& a, T b) { return a = a & b; }                                                             \
    inline bool HAS_FLAG(T a, T b) { return ((std::underlying_type_t<T>)a & (std::underlying_type_t<T>)b) != 0; }
