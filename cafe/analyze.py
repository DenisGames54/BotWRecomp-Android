"""Function discovery and control flow for the main RPX.

Entries come from: the ELF entry point, every direct `bl` target, relocated code pointers
(ADDR16_LO / ADDR32 relocations whose S+A lands in .text, minus switch-table bases), and finally
gap filling: any decodable .text not reached by an earlier function becomes a function of its
own, so every instruction gets compiled. Control inside a function is followed through b/bc and
GHS branch-table switches; a branch to another function's entry is a tail call.
"""
from dataclasses import dataclass, field

from . import ppc
from .image import Image

BO_ALWAYS = 0x14


@dataclass
class Switch:
    table: int  # address of the first `b` slot
    count: int
    targets: list[int]


@dataclass
class Function:
    entry: int
    insns: dict[int, ppc.Insn] = field(default_factory=dict)  # address -> insn (reachable only)
    labels: set[int] = field(default_factory=set)  # intra-function branch targets
    switches: dict[int, Switch] = field(default_factory=dict)  # bctr address -> switch
    tail_targets: set[int] = field(default_factory=set)


def bo_always(bo: int) -> bool:
    return (bo & 0x14) == 0x14


class Analyzer:
    def __init__(self, img: Image):
        self.img = img
        self.decoded: dict[int, ppc.Insn] = {}
        self.entries: set[int] = set()
        self.switch_bases: set[int] = set()
        self.functions: dict[int, Function] = {}
        self.problems: list[str] = []

    def insn(self, a: int) -> ppc.Insn:
        i = self.decoded.get(a)
        if i is None:
            i = ppc.decode(self.img.u32(a), a)
            self.decoded[a] = i
        return i

    # ---- switch tables -------------------------------------------------------------------
    def find_switch(self, at: int) -> Switch | None:
        """GHS branch table: addis/addic(addi) rX = table; rlwinm rI,rI,2; add; mtctr; bctr; b...
        The table sits right after the bctr; the bound comes from a preceding cmpli."""
        img = self.img
        table = None
        bound = None
        for k in range(1, 16):
            a = at - 4 * k
            if not img.is_text(a):
                break
            i = self.insn(a)
            if i.op in ("addic", "addi") and table is None:
                t = img.reloc_target(a + 2)
                if t is not None and img.is_text(t):
                    table = t
            if i.op == "cmpli" and bound is None:
                # cmpli rI,N then bgt default -> N+1 slots; then bge default -> N slots
                j = self.insn(a + 4)
                bound = i.imm
                if j.op == "bc" and j.BO & 0x1C == 0x04 and j.BI == 4 * i.rD:
                    bound = i.imm - 1
            if i.op in ("b", "bclr") or (i.op == "bcctr" and a != at):
                break
        if table is None or table != at + 4:
            return None  # GHS branch tables always follow the bctr; anything else is a far jump
        if bound is None:
            # no bound: take the run of plain `b` slots starting at the table
            n = 0
            while img.is_text(table + 4 * n) and self.insn(table + 4 * n).op == "b" and not self.insn(table + 4 * n).LK:
                n += 1
            bound = n - 1
        targets = []
        for k in range(bound + 1):
            j = self.insn(table + 4 * k)
            if j.op != "b" or j.LK:
                self.problems.append(f"switch at {at:08x}: slot {k} at {table + 4 * k:08x} is {j.op}")
                break
            targets.append(table + 4 * k)  # jump to the slot; the slot's own `b` continues
        return Switch(table, len(targets), targets)

    # ---- entry discovery -------------------------------------------------------------------
    def discover_entries(self) -> None:
        img = self.img
        lo, hi = img.text_lo, img.text_hi
        self.entries.add(img.rpx.entry)
        # switch tables first so their bases are not mistaken for functions
        for a in range(lo, hi, 4):
            i = self.insn(a)
            if i.op == "bcctr" and not i.LK:
                sw = self.find_switch(a)
                if sw:
                    self.switch_bases.add(sw.table)
        for a in range(lo, hi, 4):
            i = self.insn(a)
            if i.op == "b" and i.LK and not i.AA:
                if img.reloc_at.get(a) is not None:
                    continue  # REL24 to an import or a weak (null) symbol
                if img.is_text(i.target):
                    self.entries.add(i.target)
            elif i.op == "bc" and i.LK and i.target != a + 4 and img.is_text(i.target):
                self.entries.add(i.target)
        for r in img.rpx.relocs:
            if r.type not in (1, 4):  # ADDR32, ADDR16_LO
                continue
            t = img.reloc_target(r.offset)
            if t is not None and lo <= t < hi and t % 4 == 0 and t not in self.switch_bases:
                self.entries.add(t)

    # ---- per-function control flow ------------------------------------------------------------
    def walk(self, entry: int) -> Function:
        img = self.img
        fn = Function(entry)
        work = [entry]
        seen = set()
        while work:
            a = work.pop()
            while True:
                if a in seen:
                    break
                if not img.is_text(a):
                    self.problems.append(f"{entry:08x}: flow leaves .text at {a:08x}")
                    break
                if a != entry and a in self.entries:
                    # fell (or jumped) into another function: tail call
                    fn.tail_targets.add(a)
                    break
                seen.add(a)
                i = self.insn(a)
                fn.insns[a] = i
                op = i.op
                nxt = a + 4
                if op == "b":
                    if i.LK:
                        if img.reloc_at.get(a) is None and i.target == nxt:
                            pass  # bl $+4: reads PC into LR
                        a = nxt
                        continue
                    if img.reloc_at.get(a) is not None:
                        break  # REL24 tail call to an import (or a weak symbol)
                    t = i.target
                    if t in self.entries and t != entry:
                        fn.tail_targets.add(t)
                    else:
                        fn.labels.add(t)
                        work.append(t)
                    break
                if op == "bc":
                    t = i.target
                    if i.LK:
                        a = nxt
                        continue
                    if t in self.entries and t != entry:
                        fn.tail_targets.add(t)
                    else:
                        fn.labels.add(t)
                        work.append(t)
                    if bo_always(i.BO):
                        break
                    fn.labels.add(nxt)
                    a = nxt
                    continue
                if op == "bclr":
                    if bo_always(i.BO) and not i.LK:
                        break
                    fn.labels.add(nxt)
                    a = nxt
                    continue
                if op == "bcctr":
                    if i.LK:
                        a = nxt
                        continue
                    sw = self.find_switch(a) if bo_always(i.BO) else None
                    if sw:
                        fn.switches[a] = sw
                        for t in sw.targets:
                            fn.labels.add(t)
                            work.append(t)
                        break
                    if bo_always(i.BO):
                        break  # indirect tail call
                    fn.labels.add(nxt)
                    a = nxt
                    continue
                if op == "tw" and i.rD == 31:
                    break  # unconditional trap
                if op in ("illegal", "sc", "rfi"):
                    break
                a = nxt
        return fn

    def run(self) -> None:
        self.discover_entries()
        self.n_discovered = len(self.entries)
        for e in sorted(self.entries):
            self.functions[e] = self.walk(e)
        # Gap filling: code no function reaches becomes a function of its own. Nothing existing
        # flows into a gap (else it would be covered), so earlier functions stay valid.
        covered = set()
        for f in self.functions.values():
            covered.update(f.insns)
        a, hi = self.img.text_lo, self.img.text_hi
        self.gap_functions = 0
        while a < hi:
            if a not in covered and a not in self.switch_bases:
                i = self.insn(a)
                if self.img.u32(a) != 0 and i.op != "illegal":
                    self.entries.add(a)
                    f = self.walk(a)
                    self.functions[a] = f
                    covered.update(f.insns)
                    self.gap_functions += 1
            a += 4
