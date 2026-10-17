// Guest CPU model shared by generated code and the runtime.
//
// One 4 GB guest address space is mapped at g_mem; guest data is big-endian. Every recompiled
// function is `void f_XXXXXXXX(PPCContext* __restrict c)` and works on the context directly:
// __restrict lets clang keep registers in host registers between calls, and guest memory goes
// through a separate base pointer so stores to it can't alias the context.
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#define PPC_INLINE static inline __attribute__((always_inline))
#define PPC_NORETURN __attribute__((noreturn))

struct alignas(64) PPCContext {
    uint32_t r[32];
    uint32_t lr, ctr;
    uint8_t cr[32];  // one byte per CR bit, IBM order: cr[4*f+0]=LT, +1 GT, +2 EQ, +3 SO
    uint8_t ca, so, ov;
    uint8_t xer_bc;  // byte count for lswx/stswx
    uint32_t fpscr;
    double f[32];    // ps0 (the scalar FPR)
    double ps1[32];  // paired-single slot 1
    uint32_t gqr[8];
    uint32_t res_addr, res_val;  // lwarx/stwcx. reservation (res_val in guest byte order)
    // runtime bookkeeping
    void* thread;     // host-side guest thread object
    uint32_t core;    // current core id (0..2)
    uint32_t pad_;
};

typedef void (*GuestFn)(PPCContext*);

extern "C" uint8_t* g_mem;  // base of the guest address space

extern "C" {
void rt_call(PPCContext* c, uint32_t target);  // indirect call through a guest code pointer
PPC_NORETURN void rt_trap(PPCContext* c, uint32_t addr, const char* why);
void rt_bad_switch(PPCContext* c, uint32_t addr, uint32_t target);
uint64_t rt_timebase(void);
void rt_probe(PPCContext* c, uint32_t addr);
}

// ---- guest memory (big-endian) ---------------------------------------------------------
PPC_INLINE uint8_t ld8_(uint8_t* M, uint32_t a) { return M[a]; }
PPC_INLINE uint16_t ld16_(uint8_t* M, uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return __builtin_bswap16(v); }
PPC_INLINE uint32_t ld32_(uint8_t* M, uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return __builtin_bswap32(v); }
PPC_INLINE uint64_t ld64_(uint8_t* M, uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return __builtin_bswap64(v); }
PPC_INLINE void st8_(uint8_t* M, uint32_t a, uint8_t v) { M[a] = v; }
PPC_INLINE void st16_(uint8_t* M, uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(M + a, &v, 2); }
PPC_INLINE void st32_(uint8_t* M, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(M + a, &v, 4); }
PPC_INLINE void st64_(uint8_t* M, uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(M + a, &v, 8); }
// byte-reversed (i.e. little-endian) forms: lhbrx/lwbrx/sthbrx/stwbrx
PPC_INLINE uint16_t ld16r_(uint8_t* M, uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
PPC_INLINE uint32_t ld32r_(uint8_t* M, uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
PPC_INLINE void st16r_(uint8_t* M, uint32_t a, uint16_t v) { memcpy(M + a, &v, 2); }
PPC_INLINE void st32r_(uint8_t* M, uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }

#define LD8(a) ld8_(M, (uint32_t)(a))
#define LD16(a) ld16_(M, (uint32_t)(a))
#define LD32(a) ld32_(M, (uint32_t)(a))
#define LD64(a) ld64_(M, (uint32_t)(a))
#define ST8(a, v) st8_(M, (uint32_t)(a), (uint8_t)(v))
#define ST16(a, v) st16_(M, (uint32_t)(a), (uint16_t)(v))
#define ST32(a, v) st32_(M, (uint32_t)(a), (uint32_t)(v))
#define ST64(a, v) st64_(M, (uint32_t)(a), (uint64_t)(v))

// ---- integer helpers ------------------------------------------------------------------------
PPC_INLINE uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

PPC_INLINE void set_cr_s(PPCContext* c, int f, int32_t a, int32_t b) {
    c->cr[4 * f + 0] = a < b;
    c->cr[4 * f + 1] = a > b;
    c->cr[4 * f + 2] = a == b;
    c->cr[4 * f + 3] = c->so;
}
PPC_INLINE void set_cr_u(PPCContext* c, int f, uint32_t a, uint32_t b) {
    c->cr[4 * f + 0] = a < b;
    c->cr[4 * f + 1] = a > b;
    c->cr[4 * f + 2] = a == b;
    c->cr[4 * f + 3] = c->so;
}
#define CR0(v) set_cr_s(c, 0, (int32_t)(v), 0)

PPC_INLINE uint32_t ppc_divw(int32_t a, int32_t b) {
    if (b == 0) return a < 0 ? 0xFFFFFFFFu : 0;  // Espresso: sign-dependent garbage; Cemu matches this
    if (a == INT32_MIN && b == -1) return 0xFFFFFFFFu;
    return (uint32_t)(a / b);
}
PPC_INLINE uint32_t ppc_divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }

// overflow-recording add/sub (the 'o' forms)
PPC_INLINE uint32_t add_ov(PPCContext* c, uint32_t a, uint32_t b) {
    int32_t r;
    c->ov = __builtin_add_overflow((int32_t)a, (int32_t)b, &r);
    c->so |= c->ov;
    return (uint32_t)r;
}
PPC_INLINE uint32_t sub_ov(PPCContext* c, uint32_t a, uint32_t b) {  // a - b
    int32_t r;
    c->ov = __builtin_sub_overflow((int32_t)a, (int32_t)b, &r);
    c->so |= c->ov;
    return (uint32_t)r;
}
// a + b + carry with carry out
PPC_INLINE uint32_t adde_(PPCContext* c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t s = (uint64_t)a + b + cin;
    c->ca = (uint8_t)(s >> 32);
    return (uint32_t)s;
}

PPC_INLINE uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    uint32_t m1 = 0xFFFFFFFFu >> mb;
    uint32_t m2 = me >= 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (m1 & m2) : (m1 | m2);
}

// ---- floating point ---------------------------------------------------------------------------
PPC_INLINE uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
PPC_INLINE double bitsd(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

// Bit-exact single <-> double conversions as the FPU does them for lfs/stfs (keeps NaN payloads,
// expands denormals). Same algorithms as Cemu's ConvertToDoubleNoFTZ/ConvertToSingleNoFTZ.
PPC_INLINE double f32_to_f64(uint32_t v) {
    uint64_t x = v;
    uint64_t exp = (x >> 23) & 0xFF;
    uint64_t frac = x & 0x007FFFFF;
    if (exp > 0 && exp < 255) {
        uint64_t y = !(exp >> 7);
        uint64_t z = y << 61 | y << 60 | y << 59;
        return bitsd(((x & 0xC0000000) << 32) | z | ((x & 0x3FFFFFFF) << 29));
    } else if (exp == 0 && frac != 0) {
        exp = 1023 - 126;
        do {
            frac <<= 1;
            exp -= 1;
        } while ((frac & 0x00800000) == 0);
        return bitsd(((x & 0x80000000) << 32) | (exp << 52) | ((frac & 0x007FFFFF) << 29));
    }
    // zero, inf, NaN
    uint64_t y = exp >> 7;
    uint64_t z = y << 61 | y << 60 | y << 59;
    return bitsd(((x & 0xC0000000) << 32) | z | ((x & 0x3FFFFFFF) << 29));
}
PPC_INLINE uint32_t f64_to_f32(double d) {
    uint64_t x = dbits(d);
    uint32_t exp = (x >> 52) & 0x7FF;
    if (exp > 896 || (x & ~0x8000000000000000ULL) == 0)
        return (uint32_t)(((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF));
    if (exp >= 874) {
        uint32_t t = (uint32_t)(0x80000000 | ((x & 0x000FFFFFFFFFFFFFULL) >> 21));
        t = t >> (905 - exp);
        t |= (uint32_t)((x >> 32) & 0x80000000);
        return t;
    }
    return (uint32_t)(((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF));
}
PPC_INLINE double round_single(double d) { return (double)(float)d; }
// Paired-single multiplies round frC to 25 bits of mantissa first (Gekko/Espresso quirk).
PPC_INLINE double round25(double d) {
    uint64_t v = dbits(d);
    v = (v & 0xFFFFFFFFF8000000ULL) + (v & 0x8000000ULL);
    return bitsd(v);
}
PPC_INLINE double ftz_single(double d) {
    float f = (float)d;
    if (fpclassify(f) == FP_SUBNORMAL) f = copysignf(0.0f, f);
    return f;
}

extern "C" double ppc_fres(double x);
extern "C" double ppc_frsqrte(double x);

PPC_INLINE void set_cr_f(PPCContext* c, int f, double a, double b) {
    uint8_t lt = 0, gt = 0, eq = 0, un = 0;
    if (isnan(a) || isnan(b)) un = 1;
    else if (a < b) lt = 1;
    else if (a > b) gt = 1;
    else eq = 1;
    c->cr[4 * f + 0] = lt;
    c->cr[4 * f + 1] = gt;
    c->cr[4 * f + 2] = eq;
    c->cr[4 * f + 3] = un;
    c->fpscr = (c->fpscr & 0xFFFF0FFFu) | ((uint32_t)(lt << 3 | gt << 2 | eq << 1 | un) << 12);
}

PPC_INLINE uint64_t ppc_fctiwz(double b) {
    uint64_t v;
    if (b > (double)0x7FFFFFFF) v = 0x7FFFFFFF;
    else if (b < -(double)0x80000000u) v = 0x80000000u;
    else if (isnan(b)) v = 0x80000000u;
    else v = (uint32_t)(int32_t)b;
    uint64_t r = 0xFFF8000000000000ULL | v;
    if (v == 0 && (dbits(b) >> 63)) r |= 0x100000000ULL;
    return r;
}
PPC_INLINE uint64_t ppc_fctiw(double b) {
    uint64_t v;
    if (b > (double)0x7FFFFFFF) v = 0x7FFFFFFF;
    else if (b < -(double)0x80000000u) v = 0x80000000u;
    else if (isnan(b)) v = 0x80000000u;
    else v = (uint32_t)(int32_t)nearbyint(b);  // round to nearest even (FPSCR RN=0)
    uint64_t r = 0xFFF8000000000000ULL | v;
    if (v == 0 && (dbits(b) >> 63)) r |= 0x100000000ULL;
    return r;
}

// ---- paired-single quantized loads/stores -------------------------------------------------------
extern "C" const float g_dequant_scale[64];
extern "C" const float g_quant_scale[64];

PPC_INLINE double psq_ld1(uint8_t* M, uint32_t ea, uint32_t type, uint32_t scale) {
    switch (type) {
        case 4: return (double)((float)LD8(ea) * g_dequant_scale[scale]);
        case 5: return (double)((float)LD16(ea) * g_dequant_scale[scale]);
        case 6: return (double)((float)(int8_t)LD8(ea) * g_dequant_scale[scale]);
        case 7: return (double)((float)(int16_t)LD16(ea) * g_dequant_scale[scale]);
        default: return f32_to_f64(LD32(ea));
    }
}
PPC_INLINE uint32_t psq_size(uint32_t type) { return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4; }

PPC_INLINE void psq_load(PPCContext* c, uint8_t* M, uint32_t ea, int w, uint32_t gqr, int frd) {
    uint32_t type = (gqr >> 16) & 7, scale = (gqr >> 24) & 0x3F;
    c->f[frd] = psq_ld1(M, ea, type, scale);
    c->ps1[frd] = w ? 1.0 : psq_ld1(M, ea + psq_size(type), type, scale);
}

PPC_INLINE void psq_st1(uint8_t* M, uint32_t ea, double v, uint32_t type, uint32_t scale) {
    if (type == 0 || type > 7 || (type >= 1 && type <= 3)) {
        ST32(ea, f64_to_f32(v));
        return;
    }
    float f = (float)v * g_quant_scale[scale];
    switch (type) {
        case 4: ST8(ea, (uint8_t)(f <= 0 ? 0 : f >= 255.f ? 255 : (uint32_t)f)); break;
        case 5: ST16(ea, (uint16_t)(f <= 0 ? 0 : f >= 65535.f ? 65535 : (uint32_t)f)); break;
        case 6: ST8(ea, (uint8_t)(int8_t)(f <= -128.f ? -128 : f >= 127.f ? 127 : (int32_t)f)); break;
        case 7: ST16(ea, (uint16_t)(int16_t)(f <= -32768.f ? -32768 : f >= 32767.f ? 32767 : (int32_t)f)); break;
    }
}
PPC_INLINE void psq_store(PPCContext* c, uint8_t* M, uint32_t ea, int w, uint32_t gqr, int frs) {
    uint32_t type = gqr & 7, scale = (gqr >> 8) & 0x3F;
    psq_st1(M, ea, c->f[frs], type, scale);
    if (!w) psq_st1(M, ea + psq_size(type), c->ps1[frs], type, scale);
}

// ---- atomics (lwarx/stwcx.) ------------------------------------------------------------------
PPC_INLINE uint32_t ppc_lwarx(PPCContext* c, uint8_t* M, uint32_t ea) {
    uint32_t raw = __atomic_load_n((uint32_t*)(M + ea), __ATOMIC_ACQUIRE);
    c->res_addr = ea;
    c->res_val = raw;
    return __builtin_bswap32(raw);
}
PPC_INLINE void ppc_stwcx(PPCContext* c, uint8_t* M, uint32_t ea, uint32_t v) {
    uint8_t ok = 0;
    if (c->res_addr == ea) {
        uint32_t expect = c->res_val;
        ok = __atomic_compare_exchange_n((uint32_t*)(M + ea), &expect, __builtin_bswap32(v), false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    c->res_addr = 0xFFFFFFFFu;
    c->cr[0] = 0;
    c->cr[1] = 0;
    c->cr[2] = ok;
    c->cr[3] = c->so;
}
