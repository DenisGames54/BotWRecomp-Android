"""Disassemble guest code. usage: uv run python -I tools/dis.py <rpx> <hex addr> [count]"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from cafe import ppc  # noqa: E402
from cafe.image import Image  # noqa: E402

img = Image(sys.argv[1])
a = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
for k in range(n):
    w = img.u32(a)
    if w is None:
        break
    i = ppc.decode(w, a)
    note = ""
    imp = img.reloc_import(a) or img.reloc_import(a + 2)
    if imp:
        note = f"   ; -> {imp.lib}.{imp.name}"
    else:
        t = img.reloc_target(a) or img.reloc_target(a + 2)
        if t is not None:
            note = f"   ; reloc -> 0x{t:08x}"
    print(f"{a:08x}: {w:08x}  {ppc.fmt(i)}{note}")
    a += 4
