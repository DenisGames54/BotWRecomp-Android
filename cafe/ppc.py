"""Espresso (PowerPC 750CL family + paired singles) instruction decoder.

decode(word, addr) -> Insn. Field names follow the IBM manuals: rD/rS/rA/rB, frD/frA/frB/frC,
crfD/crfS, BO/BI, SIMM/UIMM, SH/MB/ME, spr, CRM, FM, Rc, OE, LK, AA. Unknown encodings decode as
op "illegal".
"""
from dataclasses import dataclass


def bits(w: int, a: int, b: int) -> int:
    """IBM bit numbering: bit 0 is the MSB."""
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def sext(v: int, n: int) -> int:
    return v - (1 << n) if v & (1 << (n - 1)) else v


@dataclass(slots=True)
class Insn:
    addr: int
    word: int
    op: str
    rD: int = 0  # also rS, frD, frS, crbD, crfD (see op)
    rA: int = 0
    rB: int = 0
    rC: int = 0  # frC / MB etc. where noted
    imm: int = 0  # SIMM (sign-extended), UIMM, d, branch target, spr, SH...
    mb: int = 0
    me: int = 0
    Rc: bool = False
    OE: bool = False
    LK: bool = False
    AA: bool = False
    BO: int = 0
    BI: int = 0
    W: int = 0  # psq W bit
    I: int = 0  # psq GQR index

    @property
    def target(self) -> int:
        return self.imm


# opcode 31 extended (bits 21-30) without OE variants
X31 = {
    0: "cmp", 4: "tw", 8: "subfc", 10: "addc", 11: "mulhwu", 19: "mfcr", 20: "lwarx", 23: "lwzx",
    24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl", 40: "subf", 54: "dcbst", 55: "lwzux", 60: "andc",
    75: "mulhw", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 104: "neg", 119: "lbzux", 124: "nor",
    136: "subfe", 138: "adde", 144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx", 183: "stwux",
    200: "subfze", 202: "addze", 210: "mtsr", 215: "stbx", 232: "subfme", 234: "addme", 235: "mullw",
    242: "mtsrin", 246: "dcbtst", 247: "stbux", 266: "add", 278: "dcbt", 279: "lhzx", 284: "eqv",
    306: "tlbie", 310: "eciwx", 311: "lhzux", 316: "xor", 339: "mfspr", 343: "lhax", 370: "tlbia",
    371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc", 438: "ecowx", 439: "sthux", 444: "or",
    459: "divwu", 467: "mtspr", 470: "dcbi", 476: "nand", 491: "divw", 512: "mcrxr", 533: "lswx",
    534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync", 567: "lfsux", 595: "mfsr", 597: "lswi",
    598: "sync", 599: "lfdx", 631: "lfdux", 659: "mfsrin", 661: "stswx", 662: "stwbrx", 663: "stfsx",
    695: "stfsux", 725: "stswi", 727: "stfdx", 759: "stfdux", 790: "lhbrx", 792: "sraw", 824: "srawi",
    854: "eieio", 918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi", 983: "stfiwx", 1014: "dcbz",
}
# XO-form arithmetic: these can carry OE (bit 21) -> xo | 0x200
OE_OPS = {8, 10, 11, 40, 75, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491}

X19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
       225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}

A59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls", 28: "fmsubs",
       29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
A63 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
       28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
X63 = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
       64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs",
       711: "mtfsf"}
PS10 = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr", 96: "ps_cmpo1",
        136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
        624: "ps_merge11", 1014: "dcbz_l"}
PS5 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1",
       18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul",
       26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
PS6 = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}

D_OPS = {
    7: "mulli", 8: "subfic", 12: "addic", 13: "addic.", 14: "addi", 15: "addis",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
}
U_OPS = {24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis."}


def decode(w: int, addr: int) -> Insn:
    po = w >> 26
    rD, rA, rB = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20)
    Rc = bool(w & 1)
    if po in D_OPS:
        return Insn(addr, w, D_OPS[po], rD, rA, imm=sext(w & 0xFFFF, 16))
    if po in U_OPS:
        return Insn(addr, w, U_OPS[po], rD, rA, imm=w & 0xFFFF)
    if po == 18:
        li = sext(w & 0x03FFFFFC, 26)
        aa, lk = bool(w & 2), bool(w & 1)
        t = (li if aa else addr + li) & 0xFFFFFFFF
        return Insn(addr, w, "b", imm=t, AA=aa, LK=lk)
    if po == 16:
        bd = sext(w & 0xFFFC, 16)
        aa, lk = bool(w & 2), bool(w & 1)
        t = (bd if aa else addr + bd) & 0xFFFFFFFF
        return Insn(addr, w, "bc", imm=t, AA=aa, LK=lk, BO=rD, BI=rA)
    if po == 10:
        return Insn(addr, w, "cmpli", rD >> 2, rA, imm=w & 0xFFFF)
    if po == 11:
        return Insn(addr, w, "cmpi", rD >> 2, rA, imm=sext(w & 0xFFFF, 16))
    if po == 3:
        return Insn(addr, w, "twi", rD, rA, imm=sext(w & 0xFFFF, 16))
    if po == 17:
        return Insn(addr, w, "sc")
    if po in (20, 21, 23):
        op = {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[po]
        return Insn(addr, w, op, rD, rA, rB, imm=rB, mb=bits(w, 21, 25), me=bits(w, 26, 30), Rc=Rc)
    if po == 19:
        xo = bits(w, 21, 30)
        op = X19.get(xo)
        if op is None:
            return Insn(addr, w, "illegal")
        if op in ("bclr", "bcctr"):
            return Insn(addr, w, op, LK=bool(w & 1), BO=rD, BI=rA)
        if op == "mcrf":
            return Insn(addr, w, op, rD >> 2, rA >> 2)
        return Insn(addr, w, op, rD, rA, rB)
    if po == 31:
        xo = bits(w, 21, 30)
        oe = False
        op = X31.get(xo)
        if op is None and (xo & 0x1FF) in OE_OPS and xo & 0x200:
            op = X31.get(xo & 0x1FF)
            oe = True
        if op is None:
            return Insn(addr, w, "illegal")
        i = Insn(addr, w, op, rD, rA, rB, Rc=Rc, OE=oe)
        if op in ("cmp", "cmpl"):
            i.rD = rD >> 2
        elif op in ("mfspr", "mtspr", "mftb"):
            i.imm = (rB << 5) | rA  # split SPR field
        elif op == "mtcrf":
            i.imm = bits(w, 12, 19)
        elif op == "srawi":
            i.imm = rB
        elif op in ("lswi", "stswi"):
            i.imm = rB if rB else 32
        elif op == "mcrxr":
            i.rD = rD >> 2
        return i
    if po == 59:
        xo = bits(w, 26, 30)
        op = A59.get(xo)
        if op is None:
            return Insn(addr, w, "illegal")
        return Insn(addr, w, op, rD, rA, rB, bits(w, 21, 25), Rc=Rc)
    if po == 63:
        xo5 = bits(w, 26, 30)
        if xo5 in A63 and (xo5 >= 18):
            # A-form ops occupy XO 18..31 in bits 26-30
            if xo5 in A63:
                return Insn(addr, w, A63[xo5], rD, rA, rB, bits(w, 21, 25), Rc=Rc)
        xo = bits(w, 21, 30)
        op = X63.get(xo)
        if op is None:
            return Insn(addr, w, "illegal")
        i = Insn(addr, w, op, rD, rA, rB, Rc=Rc)
        if op in ("fcmpu", "fcmpo", "mcrfs"):
            i.rD = rD >> 2
            if op == "mcrfs":
                i.rA = rA >> 2
        elif op == "mtfsf":
            i.imm = bits(w, 7, 14)  # FM
        elif op == "mtfsfi":
            i.rD = rD >> 2
            i.imm = bits(w, 16, 19)
        return i
    if po == 4:
        xo10 = bits(w, 21, 30)
        if xo10 in PS10:
            op = PS10[xo10]
            i = Insn(addr, w, op, rD, rA, rB, Rc=Rc)
            if op.startswith("ps_cmp"):
                i.rD = rD >> 2
            return i
        xo6 = bits(w, 25, 30)
        if xo6 in PS6:
            return Insn(addr, w, PS6[xo6], rD, rA, rB, W=bits(w, 21, 21), I=bits(w, 22, 24))
        xo5 = bits(w, 26, 30)
        if xo5 in PS5:
            return Insn(addr, w, PS5[xo5], rD, rA, rB, bits(w, 21, 25), Rc=Rc)
        return Insn(addr, w, "illegal")
    if po in (56, 57, 60, 61):
        op = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}[po]
        return Insn(addr, w, op, rD, rA, imm=sext(w & 0xFFF, 12), W=bits(w, 16, 16), I=bits(w, 17, 19))
    return Insn(addr, w, "illegal")


SPR_NAMES = {1: "xer", 8: "lr", 9: "ctr", 268: "tbl", 269: "tbu", 912: "gqr0", 913: "gqr1",
             914: "gqr2", 915: "gqr3", 916: "gqr4", 917: "gqr5", 918: "gqr6", 919: "gqr7"}


def fmt(i: Insn) -> str:
    """Short disassembly for comments and tools."""
    o = i.op
    if o in ("b",):
        return f"b{'l' if i.LK else ''}{'a' if i.AA else ''} 0x{i.imm:08x}"
    if o == "bc":
        return f"bc{'l' if i.LK else ''} {i.BO},{i.BI},0x{i.imm:08x}"
    if o in ("bclr", "bcctr"):
        if i.BO & 0x14 == 0x14:
            return ("blr" if o == "bclr" else "bctr") + ("l" if i.LK else "")
        return f"{o}{'l' if i.LK else ''} {i.BO},{i.BI}"
    if o in D_OPS.values() and o not in ("addi", "addis", "mulli", "subfic", "addic", "addic."):
        reg = "f" if o.startswith(("lf", "stf")) else "r"
        return f"{o} {reg}{i.rD},{i.imm}(r{i.rA})"
    if o in ("addi", "addis", "mulli", "subfic", "addic", "addic."):
        return f"{o} r{i.rD},r{i.rA},{i.imm}"
    if o in U_OPS.values():
        return f"{o} r{i.rA},r{i.rD},0x{i.imm:x}"
    if o in ("cmpi", "cmpli"):
        return f"{o} cr{i.rD},r{i.rA},{i.imm}"
    if o in ("cmp", "cmpl"):
        return f"{o} cr{i.rD},r{i.rA},r{i.rB}"
    if o in ("rlwinm", "rlwimi"):
        return f"{o}{'.' if i.Rc else ''} r{i.rA},r{i.rD},{i.imm},{i.mb},{i.me}"
    if o == "rlwnm":
        return f"{o}{'.' if i.Rc else ''} r{i.rA},r{i.rD},r{i.rB},{i.mb},{i.me}"
    if o in ("mfspr", "mtspr"):
        return f"{o} {SPR_NAMES.get(i.imm, i.imm)},r{i.rD}"
    if o.startswith("psq_"):
        if o in ("psq_l", "psq_lu", "psq_st", "psq_stu"):
            return f"{o} f{i.rD},{i.imm}(r{i.rA}),{i.W},qr{i.I}"
        return f"{o} f{i.rD},r{i.rA},r{i.rB},{i.W},qr{i.I}"
    if o.startswith(("f", "ps_")) and o not in ("fcmpu", "fcmpo"):
        return f"{o}{'.' if i.Rc else ''} f{i.rD},f{i.rA},f{i.rB},f{i.rC}"
    return f"{o}{'o' if i.OE else ''}{'.' if i.Rc else ''} {i.rD},{i.rA},{i.rB}"
