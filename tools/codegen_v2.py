#!/usr/bin/env python3
"""Smaller guest code. Run from the repo root. usage: codegen_v2.py <step>...   steps: ea call hle

ea    D-form loads/stores and lmw/stmw/lfs/lfd/stfs/stfd compute rA+d in 64 bits (no 32-bit wrap), so the
      host folds the displacement into its addressing mode. mem.cpp keeps guard pages on both sides.
      (does not change the control-flow graph, so the PGO profile stays valid)
call  rt_call (indirect calls) hits the text table directly instead of going through dispatch::lookup.
hle   HLE_CALL/HLE_TAIL lose the inline g_trace_hle branch (-DBOTW_TRACE_HLE brings it back).
      (changes the CFG of functions with HLE calls: re-collect the PGO profile after this one)
Steps are independent and idempotent. Regenerate generated/ afterwards (tools/recompile.py).
"""
import re, sys

PENDING = {}
def rd(p): return PENDING[p] if p in PENDING else open(p).read()
def wr(p, s): PENDING[p] = s
def rep(s, old, new, what):
    assert s.count(old) == 1, f"{what}: pattern not found exactly once ({s.count(old)})"
    return s.replace(old, new)


def step_ea():
    if "def ea_d(" in rd("cafe/codegen.py"):
        print("ea: already applied"); return
    s = rd("cafe/codegen.py")
    helper = '''def ea_d(ra: int, imm: int) -> str:
    """Effective address of a D-form access without update (rA|0 + d).

    For rA != 0 the sum is done in 64 bits instead of wrapping at 32: the host can then fold the
    displacement into the load/store addressing mode and share one base across consecutive accesses
    (stmw/lmw, struct fields). A guest address that would wrap past 0 or 4 GB is a guest bug anyway;
    mem.cpp keeps guard pages on both sides of the 4 GB window."""
    if ra == 0:
        return f"(uint32_t){imm}"
    return f"((uint64_t){R(ra)} + (int64_t){imm})"


'''
    s = rep(s, "def sanitize(s: str) -> str:", helper + "def sanitize(s: str) -> str:", "helper")
    s = rep(s, 'ea = f"({RA0(ra)} + (uint32_t){imm})" if not upd else f"({R(ra)} + (uint32_t){imm})"',
            'ea = ea_d(ra, imm) if not upd else f"({R(ra)} + (uint32_t){imm})"', "dform")
    s = rep(s, '''            ea = f"({R(ra) if upd else RA0(ra)} + (uint32_t){imm})"
            L.append(f"{{ uint32_t ea_ = {ea};")
            L += self.fp_mem(op[:4] if op.startswith("stf") else op[:3], d)''',
            '''            if upd:
                L.append(f"{{ uint32_t ea_ = ({R(ra)} + (uint32_t){imm});")
            else:
                L.append(f"{{ uint64_t ea_ = {ea_d(ra, imm)};")
            L += self.fp_mem(op[:4] if op.startswith("stf") else op[:3], d)''', "fp dform")
    s = rep(s, '''            L.append(f"{{ uint32_t ea_ = {RA0(ra)} + (uint32_t){imm};")
            for k in range(d, 32):
                L.append(f"{R(k)} = LD32(ea_ + {4 * (k - d)}u);")''',
            '''            L.append(f"{{ uint64_t ea_ = {ea_d(ra, imm)};")
            for k in range(d, 32):
                L.append(f"{R(k)} = LD32(ea_ + {4 * (k - d)}u);")''', "lmw")
    s = rep(s, '''            L.append(f"{{ uint32_t ea_ = {RA0(ra)} + (uint32_t){imm};")
            for k in range(d, 32):
                L.append(f"ST32(ea_ + {4 * (k - d)}u, {R(k)});")''',
            '''            L.append(f"{{ uint64_t ea_ = {ea_d(ra, imm)};")
            for k in range(d, 32):
                L.append(f"ST32(ea_ + {4 * (k - d)}u, {R(k)});")''', "stmw")
    wr("cafe/codegen.py", s)

    s = rd("runtime/include/ppc.h")
    for fn in ["ld8_", "ld16_", "ld32_", "ld64_", "st8_", "st16_", "st32_", "st64_", "ld16r_", "ld32r_", "st16r_", "st32r_"]:
        s, k = re.subn(r"(PPC_INLINE [^\n]*? " + re.escape(fn) + r"\(uint8_t\* M, )uint32_t a", r"\1uint64_t a", s)
        assert k == 1, fn
    assert s.count("(M, (uint32_t)(a)") == 8
    s = s.replace("(M, (uint32_t)(a)", "(M, (uint64_t)(a)")
    wr("runtime/include/ppc.h", s)

    s = rd("runtime/src/mem.cpp")
    s = rep(s, "(1ull << 32) + 0x10000 + (2u << 20);", "(1ull << 32) + 0x10000 + (4u << 20);", "mem size")
    s = rep(s, "g_mem = (uint8_t*)(((uintptr_t)p + (2u << 20) - 1) & ~(uintptr_t)((2u << 20) - 1));",
            "g_mem = (uint8_t*)(((uintptr_t)p + 0x10000 + (2u << 20) - 1) & ~(uintptr_t)((2u << 20) - 1));", "mem align")
    s = rep(s, "    mprotect(g_mem, 0x10000, PROT_NONE);  // catch guest null-pointer accesses",
            "    mprotect(g_mem, 0x10000, PROT_NONE);  // catch guest null-pointer accesses\n"
            "    mprotect(g_mem - 0x10000, 0x10000, PROT_NONE);  // rA+d is no longer wrapped at 32 bits: a negative sum lands here", "mem guard")
    wr("runtime/src/mem.cpp", s)
    print("ea: applied")


def step_call():
    s = rd("runtime/src/dispatch.cpp")
    if "hot path: recompiled function" in s:
        print("call: already applied"); return
    s = rep(s, '''extern "C" void rt_call(PPCContext* c, uint32_t target) {
    GuestFn fn = dispatch::lookup(target);''',
            '''extern "C" void rt_call(PPCContext* c, uint32_t target) {
    // hot path: recompiled function in the text table (one range check, one load)
    uint32_t off = target - mem::kText;
    GuestFn fn;
    if (__builtin_expect(off < g_text_end - mem::kText, 1) && (fn = g_text_table[off >> 2])) {
        fn(c);
        return;
    }
    fn = dispatch::lookup(target);''', "rt_call")
    wr("runtime/src/dispatch.cpp", s)
    print("call: applied")


def step_hle():
    s = rd("tools/recompile.py")
    if "BOTW_TRACE_HLE" in s:
        print("hle: already applied"); return
    old_call = '#define HLE_CALL(f, n) do { if (__builtin_expect(g_trace_hle, 0)) { rt_trace_hle(c, n, 0); f(c); rt_trace_hle(c, n, 1); } else f(c); } while (0)\n'
    old_tail = '#define HLE_TAIL(f, n) do { if (__builtin_expect(g_trace_hle, 0)) { rt_trace_hle(c, n, 0); f(c); rt_trace_hle(c, n, 1); return; } [[clang::musttail]] return f(c); } while (0)\n'
    new = ('#ifdef BOTW_TRACE_HLE\n' + old_call + old_tail +
           '#else\n#define HLE_CALL(f, n) f(c)\n#define HLE_TAIL(f, n) do { [[clang::musttail]] return f(c); } while (0)\n#endif\n')
    s = rep(s, old_call + old_tail, new, "hle macros")
    wr("tools/recompile.py", s)
    print("hle: applied")


steps = sys.argv[1:]
if not steps or any(x not in ("ea", "call", "hle") for x in steps):
    sys.exit(__doc__)
for x in steps:
    {"ea": step_ea, "call": step_call, "hle": step_hle}[x]()
for p, t in PENDING.items():
    open(p, "w").write(t)
print("written:", ", ".join(sorted(PENDING)) or "nothing")
