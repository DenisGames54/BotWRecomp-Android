"""Guest image of an RPX at its link addresses, relocations applied.

Imports keep their link-time stub addresses in the .fimport_/.dimport_ sections (0xC000xxxx);
the runtime maps those addresses to HLE functions / variables.
"""
import bisect
import struct

from . import rpx

DATA_IMPORT_BASE = 0x0DF00000


class Image:
    def __init__(self, path: str):
        self.rpx = r = rpx.load(path)
        # Data imports get fixed 4 KB slots: recompiled code bakes their addresses in, and the
        # runtime places the exported variables at the same addresses (see guest_table.h).
        self.data_import_addr = {}
        k = 0
        for imp in sorted(r.imports, key=lambda i: i.addr):
            if imp.is_data and not imp.name.startswith("."):
                self.data_import_addr[imp.sym_index] = DATA_IMPORT_BASE + 0x1000 * k
                k += 1

        def resolve(i, s):
            return self.data_import_addr.get(i, s.value)

        self.secs = rpx.build_image(r, resolve)
        self.starts = sorted(self.secs)
        self.text = [s for s in r.text_sections() if s.name == ".text"][0]
        self.text_lo, self.text_hi = self.text.addr, self.text.addr + self.text.size
        self.import_at = {i.addr: i for i in r.imports}
        # relocation index: address being patched -> Reloc
        self.reloc_at = {x.offset: x for x in r.relocs}

    def _loc(self, addr: int):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None, 0
        b = self.starts[i]
        buf = self.secs[b]
        if addr - b >= len(buf):
            return None, 0
        return buf, addr - b

    def u32(self, addr: int) -> int | None:
        buf, o = self._loc(addr)
        if buf is None or o + 4 > len(buf):
            return None
        return struct.unpack_from(">I", buf, o)[0]

    def is_text(self, addr: int) -> bool:
        return self.text_lo <= addr < self.text_hi

    def reloc_target(self, at: int) -> int | None:
        """S + A of the relocation patching address `at` (any type)."""
        r = self.reloc_at.get(at)
        if r is None:
            return None
        sym = self.rpx.symbols[r.sym]
        return (self.data_import_addr.get(r.sym, sym.value) + r.addend) & 0xFFFFFFFF

    def reloc_import(self, at: int):
        r = self.reloc_at.get(at)
        if r is None:
            return None
        return self.import_at.get(self.rpx.symbols[r.sym].value)
