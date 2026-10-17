"""Static scan of U-King.rpx for how the game drives the GPU (native renderer design evidence).

  uv run python -I tools/shaders/analysis/gx2_static_scan.py ../game/code/U-King.rpx

Reports
  * call sites per GX2 import (bl / b to the import stub),
  * PM4 type-3 packet headers synthesised in game code (lis 0xC0nn + ori/addi 0xXX00 with a
    known IT_* opcode), i.e. command words written without going through GX2,
  * PM4-looking big-endian words in the data sections (pre-baked command streams).
"""
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
from cafe import ppc  # noqa: E402
from cafe.image import Image  # noqa: E402

IT = {
    0x10: "NOP", 0x20: "SET_PREDICATION", 0x22: "COND_EXEC", 0x23: "PRED_EXEC", 0x27: "DRAW_INDEX_2",
    0x28: "CONTEXT_CONTROL", 0x2A: "INDEX_TYPE", 0x2D: "DRAW_INDEX_AUTO", 0x2E: "DRAW_INDEX_IMMD",
    0x2F: "NUM_INSTANCES", 0x32: "INDIRECT_BUFFER_PRIV", 0x34: "STRMOUT_BUFFER_UPDATE", 0x39: "MEM_SEMAPHORE",
    0x3C: "WAIT_REG_MEM", 0x3D: "MEM_WRITE", 0x3F: "INDIRECT_BUFFER", 0x43: "SURFACE_SYNC", 0x46: "EVENT_WRITE",
    0x47: "EVENT_WRITE_EOP", 0x60: "LOAD_CONFIG_REG", 0x61: "LOAD_CONTEXT_REG", 0x62: "LOAD_ALU_CONST",
    0x63: "LOAD_BOOL_CONST", 0x64: "LOAD_LOOP_CONST", 0x65: "LOAD_RESOURCE", 0x66: "LOAD_SAMPLER",
    0x68: "SET_CONFIG_REG", 0x69: "SET_CONTEXT_REG", 0x6A: "SET_ALU_CONST", 0x6B: "SET_BOOL_CONST",
    0x6C: "SET_LOOP_CONST", 0x6D: "SET_RESOURCE", 0x6E: "SET_SAMPLER", 0x6F: "SET_CTL_CONST",
    0x72: "STRMOUT_BASE_UPDATE", 0x74: "SET_ALL_CONTEXTS",
}


def main():
    img = Image(sys.argv[1])
    r = img.rpx
    calls = Counter()
    callers = defaultdict(set)
    pm4_code = []
    lis = {}  # reg -> (addr, hi)
    a = img.text_lo
    while a < img.text_hi:
        w = img.u32(a)
        i = ppc.decode(w, a)
        if i.op == "b":
            imp = img.reloc_import(a)
            if imp is not None and imp.lib == "gx2":
                calls[imp.name] += 1
                callers[imp.name].add(a)
        elif i.op == "addis" and i.rA == 0:
            hi = i.imm & 0xFFFF
            if (hi >> 14) == 3 and (hi & 0x3FFF) < 0x400:
                lis[i.rD] = (a, hi)
            else:
                lis.pop(i.rD, None)
        elif i.op in ("ori", "addi") and i.rA in lis:
            la, hi = lis[i.rA]
            lo = i.imm & 0xFFFF
            if a - la <= 64 and (lo & 0xFF) in (0, 1) and (lo >> 8) in IT:
                word = (hi << 16) | lo
                pm4_code.append((la, a, word))
        a += 4

    print("== GX2 call sites (static) ==")
    print(f"{sum(calls.values())} call sites to {len(calls)} GX2 functions")
    for name, n in calls.most_common():
        print(f"  {n:6}  {name}")

    print("\n== PM4 type-3 headers built in game code (lis 0xC0nn + ori/addi IT<<8) ==")
    print(f"{len(pm4_code)} candidates")
    for la, ia, word in pm4_code[:60]:
        print(f"  {la:08x}/{ia:08x}: {word:08x}  IT_{IT[(word >> 8) & 0xFF]} count={((word >> 16) & 0x3FFF) + 1}")

    print("\n== PM4-looking words in data sections ==")
    hits = Counter()
    examples = defaultdict(list)
    for s in r.sections:
        if not s.addr or img.is_text(s.addr) or s.name in (".text",) or not s.size:
            continue
        if s.name.startswith((".fimport", ".dimport", ".rela", ".symtab", ".strtab", ".shstrtab")):
            continue
        for off in range(0, s.size - 4, 4):
            w = img.u32(s.addr + off)
            if w is None:
                break
            if (w >> 30) == 3 and (w & 0xFE) == 0 and ((w >> 8) & 0xFF) in IT and ((w >> 16) & 0x3FFF) < 0x40:
                # require a plausible following register offset for SET_* packets
                op = (w >> 8) & 0xFF
                nxt = img.u32(s.addr + off + 4) or 0
                if 0x68 <= op <= 0x6F and nxt > 0x1000:
                    continue
                hits[(s.name, IT[op])] += 1
                if len(examples[s.name]) < 8:
                    examples[s.name].append(f"{s.addr + off:08x}:{w:08x} {nxt:08x}")
    for (sec, op), n in sorted(hits.items(), key=lambda x: -x[1]):
        print(f"  {sec:12} IT_{op:22} {n}")
    for sec, ex in examples.items():
        print(f"  e.g. {sec}: {' | '.join(ex)}")


if __name__ == "__main__":
    main()
