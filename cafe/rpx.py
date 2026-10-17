"""RPX/RPL loader (Cafe OS ELF: 32-bit big-endian PowerPC, zlib-compressed sections).

Section contents are kept at their link addresses (text 0x02000000, data 0x10000000, loader
sections 0xC0000000). `apply_relocations` patches the image the way the Cafe loader would,
with imports resolved to caller-supplied addresses.
"""
import struct
import zlib
from dataclasses import dataclass, field

SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS = 1, 2, 3, 4, 8
SHT_RPL_EXPORTS, SHT_RPL_IMPORTS, SHT_RPL_CRCS, SHT_RPL_FILEINFO = 0x80000001, 0x80000002, 0x80000003, 0x80000004
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
SHF_RPL_ZLIB = 0x08000000

R_PPC_NONE, R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 0, 1, 4, 5, 6
R_PPC_REL24, R_PPC_REL14 = 10, 11
R_PPC_REL32 = 26
R_PPC_DTPMOD32, R_PPC_DTPREL32 = 68, 78
R_PPC_EMB_SDA21 = 109
R_PPC_GHS_REL16_HA, R_PPC_GHS_REL16_HI, R_PPC_GHS_REL16_LO = 251, 252, 253

RELOC_NAMES = {0: "NONE", 1: "ADDR32", 4: "ADDR16_LO", 5: "ADDR16_HI", 6: "ADDR16_HA", 10: "REL24",
               11: "REL14", 26: "REL32", 68: "DTPMOD32", 78: "DTPREL32", 109: "EMB_SDA21",
               251: "GHS_REL16_HA", 252: "GHS_REL16_HI", 253: "GHS_REL16_LO"}


@dataclass
class Section:
    index: int
    name: str
    type: int
    flags: int
    addr: int
    size: int  # uncompressed size
    link: int
    info: int
    align: int
    entsize: int
    data: bytes = b""


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    info: int
    shndx: int

    @property
    def type(self) -> int:
        return self.info & 0xF

    @property
    def bind(self) -> int:
        return self.info >> 4


@dataclass
class Reloc:
    offset: int  # address being patched
    type: int
    sym: int
    addend: int
    target_section: int


@dataclass
class Import:
    lib: str
    name: str
    addr: int  # stub/variable address inside the .fimport_/.dimport_ section
    is_data: bool
    sym_index: int


@dataclass
class Rpx:
    path: str
    entry: int
    sections: list[Section]
    symbols: list[Symbol]
    relocs: list[Reloc]
    imports: list[Import]
    exports: dict[str, int]  # name -> address (functions and data)
    fileinfo: dict = field(default_factory=dict)

    def section_at(self, addr: int) -> Section | None:
        for s in self.sections:
            if s.flags & SHF_ALLOC and s.addr <= addr < s.addr + s.size and s.type != SHT_RPL_IMPORTS:
                return s
        return None

    def text_sections(self) -> list[Section]:
        return [s for s in self.sections if s.flags & SHF_EXECINSTR and s.flags & SHF_ALLOC and s.size]

    def is_code(self, addr: int) -> bool:
        return any(s.addr <= addr < s.addr + s.size for s in self.text_sections())


def load(path: str) -> Rpx:
    raw = open(path, "rb").read()
    if raw[:4] != b"\x7fELF" or raw[4] != 1 or raw[5] != 2:
        raise ValueError("not a 32-bit big-endian ELF")
    (e_type, e_machine, _ver, entry, _phoff, shoff, _flags, _ehsize, _phentsize, _phnum,
     shentsize, shnum, shstrndx) = struct.unpack_from(">HHIIIIIHHHHHH", raw, 16)
    if e_machine != 20:
        raise ValueError(f"not PowerPC (machine {e_machine})")
    secs: list[Section] = []
    for i in range(shnum):
        (name, typ, flags, addr, off, size, link, info, align, entsize) = struct.unpack_from(">10I", raw, shoff + i * shentsize)
        data = b""
        if typ != SHT_NOBITS and size:
            data = raw[off:off + size]
            if flags & SHF_RPL_ZLIB:
                usize = struct.unpack_from(">I", data, 0)[0]
                data = zlib.decompress(data[4:])
                assert len(data) == usize, (i, len(data), usize)
                size = usize
        secs.append(Section(i, str(name), typ, flags, addr, size, link, info, align, entsize, data))
    shstr = secs[shstrndx].data
    for s in secs:
        o = int(s.name)
        s.name = shstr[o:shstr.index(b"\0", o)].decode()

    symbols: list[Symbol] = []
    symtab = next((s for s in secs if s.type == SHT_SYMTAB), None)
    if symtab:
        strtab = secs[symtab.link].data
        for j in range(len(symtab.data) // 16):
            nm, value, size, info, _other, shndx = struct.unpack_from(">IIIBBH", symtab.data, j * 16)
            n = strtab[nm:strtab.index(b"\0", nm)].decode("utf-8", "replace") if nm < len(strtab) else ""
            symbols.append(Symbol(n, value, size, info, shndx))

    relocs: list[Reloc] = []
    for s in secs:
        if s.type == SHT_RELA:
            for j in range(len(s.data) // 12):
                off, info, addend = struct.unpack_from(">IIi", s.data, j * 12)
                relocs.append(Reloc(off, info & 0xFF, info >> 8, addend, s.info))

    imports: list[Import] = []
    for j, sym in enumerate(symbols):
        if 0 < sym.shndx < len(secs) and secs[sym.shndx].type == SHT_RPL_IMPORTS:
            sec = secs[sym.shndx]
            lib = sec.name.split("_", 1)[1] if "_" in sec.name else sec.name
            imports.append(Import(lib, sym.name, sym.value, sec.name.startswith(".dimport"), j))

    exports: dict[str, int] = {}
    for s in secs:
        if s.type == SHT_RPL_EXPORTS:
            count = struct.unpack_from(">I", s.data, 0)[0]
            for k in range(count):
                value, name_off = struct.unpack_from(">II", s.data, 8 + k * 8)
                name_off &= 0x7FFFFFFF  # top bit marks TLS exports
                nm = s.data[name_off:s.data.index(b"\0", name_off)].decode()
                exports[nm] = value

    fileinfo = {}
    fi = next((s for s in secs if s.type == SHT_RPL_FILEINFO), None)
    if fi:
        names = ["magic_version", "text_size", "text_align", "data_size", "data_align", "load_size",
                 "load_align", "temp_size", "tramp_adjust", "sda_base", "sda2_base", "stack_size",
                 "filename_offset", "flags", "heap_size", "tag_offset", "min_version",
                 "compression_level", "tramp_addition", "file_info_pad", "cafe_sdk_version",
                 "cafe_sdk_revision", "tls_module_index", "tls_align_shift", "runtime_file_info_size"]
        vals = struct.unpack_from(">" + "I" * min(len(names), len(fi.data) // 4), fi.data, 0)
        fileinfo = dict(zip(names, vals))
    return Rpx(path, entry, secs, symbols, relocs, imports, exports, fileinfo)


def reloc_value(rpx: Rpx, r: Reloc, resolve) -> int:
    """S + A for relocation r. `resolve(sym_index, symbol)` returns the symbol's run-time address."""
    sym = rpx.symbols[r.sym]
    return (resolve(r.sym, sym) + r.addend) & 0xFFFFFFFF


def build_image(rpx: Rpx, resolve) -> dict[int, bytearray]:
    """Returns {section start: patched bytes} for every allocated section, relocations applied."""
    img = {s.addr: bytearray(s.data if s.type != SHT_NOBITS else bytes(s.size))
           for s in rpx.sections if s.flags & SHF_ALLOC and s.size}
    starts = sorted(img)
    import bisect

    def locate(addr: int):
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return None, 0
        base = starts[i]
        if addr - base >= len(img[base]):
            return None, 0
        return img[base], addr - base

    for r in rpx.relocs:
        buf, o = locate(r.offset)
        if buf is None:
            continue
        v = reloc_value(rpx, r, resolve)
        t = r.type
        if t == R_PPC_ADDR32:
            struct.pack_into(">I", buf, o, v)
        elif t == R_PPC_ADDR16_LO:
            struct.pack_into(">H", buf, o, v & 0xFFFF)
        elif t == R_PPC_ADDR16_HI:
            struct.pack_into(">H", buf, o, v >> 16)
        elif t == R_PPC_ADDR16_HA:
            struct.pack_into(">H", buf, o, ((v + 0x8000) >> 16) & 0xFFFF)
        elif t == R_PPC_REL24:
            d = (v - r.offset) & 0xFFFFFFFF
            insn = struct.unpack_from(">I", buf, o)[0]
            struct.pack_into(">I", buf, o, (insn & 0xFC000003) | (d & 0x03FFFFFC))
        elif t == R_PPC_REL14:
            d = (v - r.offset) & 0xFFFFFFFF
            insn = struct.unpack_from(">I", buf, o)[0]
            struct.pack_into(">I", buf, o, (insn & 0xFFFF0003) | (d & 0xFFFC))
        elif t == R_PPC_REL32:
            struct.pack_into(">I", buf, o, (v - r.offset) & 0xFFFFFFFF)
        elif t == R_PPC_GHS_REL16_HA:
            d = (v - r.offset) & 0xFFFFFFFF
            struct.pack_into(">H", buf, o, ((d + 0x8000) >> 16) & 0xFFFF)
        elif t == R_PPC_GHS_REL16_HI:
            struct.pack_into(">H", buf, o, ((v - r.offset) >> 16) & 0xFFFF)
        elif t == R_PPC_GHS_REL16_LO:
            struct.pack_into(">H", buf, o, (v - r.offset) & 0xFFFF)
        # DTPMOD32/DTPREL32/EMB_SDA21: handled by the runtime loader if they occur
    return img
