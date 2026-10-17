"""Summarise an RPX: sections, file info, imports per library, relocation types, symbols.

usage: uv run python -I tools/rpx_info.py <file.rpx> [--imports]
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from cafe import rpx  # noqa: E402

r = rpx.load(sys.argv[1])
print(f"entry 0x{r.entry:08x}")
for s in r.sections:
    if s.type == 0 and not s.size:
        continue
    print(f"  [{s.index:2}] {s.name:24} type {s.type:08x} flags {s.flags:08x} addr {s.addr:08x} size {s.size:9x}")
print("fileinfo:", {k: hex(v) for k, v in r.fileinfo.items()})
print(f"symbols {len(r.symbols)}, named non-import {sum(1 for s in r.symbols if s.name and s.shndx and s.shndx < len(r.sections) and r.sections[s.shndx].type != rpx.SHT_RPL_IMPORTS)}")
print(f"relocs {len(r.relocs)}:", {rpx.RELOC_NAMES.get(t, t): n for t, n in Counter(x.type for x in r.relocs).most_common()})
libs = Counter((i.lib, i.is_data) for i in r.imports)
print(f"imports {len(r.imports)}:")
for (lib, d), n in sorted(libs.items()):
    print(f"  {lib:16} {'data' if d else 'func'} {n}")
print(f"exports {len(r.exports)}: {list(r.exports.items())[:10]}")
if "--imports" in sys.argv:
    for i in sorted(r.imports, key=lambda i: (i.lib, i.name)):
        print(f"  {i.lib}.{i.name} @{i.addr:08x}{' (data)' if i.is_data else ''}")
