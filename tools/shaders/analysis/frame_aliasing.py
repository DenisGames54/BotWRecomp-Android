"""Format reinterpretation in a --gpu-dump-frame log: surfaces rendered (RT/depth) in one format
and later sampled as a texture in another, and addresses reused for different surfaces.

  uv run python -I tools/shaders/analysis/frame_aliasing.py gpu_frame_N.txt
"""
import re
import sys
from collections import Counter, defaultdict

DEPTH = {1: "D16", 3: "D24S8", 5: "D24S8F", 6: "D32F", 7: "D32S8"}
rt_re = re.compile(r"RT\d (\w+) fmt (\w+)/(\d) tile (\d+) (\d+)x(\d+)")
z_re = re.compile(r"Z (\w+) fmt (\d) test (\d) write (\d)")
t_re = re.compile(r"t\d+ (\w+) (\d+)x(\d+) dim (\d) fmt (\w+) tile (\d+)")
written = {}  # addr -> (kind, fmt, w, h)
rt_formats = defaultdict(set)
reinterp = Counter()
same = Counter()
for line in open(sys.argv[1]):
    if not line.startswith("draw"):
        continue
    state, _, tex = line.partition(" | VS ")
    for m in t_re.finditer(line.split(" | ")[-1]):
        a, w, h, dim, fmt, tile = m.groups()
        a = f"{int(a, 16) & ~0xFFF:08x}"  # drop the macro-tile swizzle bits (8..10)
        if a in written:
            k, f0, w0, h0 = written[a]
            key = f"{k} {f0} {w0}x{h0} -> sampled as fmt {fmt} {w}x{h} dim {dim}"
            if (k == "RT" and int(f0, 16) == int(fmt, 16)):
                same[f"RT fmt {fmt}"] += 1
            else:
                reinterp[key] += 1
    for m in rt_re.finditer(state):
        a, fmt, num, tile, w, h = m.groups()
        a = f"{int(a, 16) & ~0xFFF:08x}"
        written[a] = ("RT", fmt, w, h)
        rt_formats[a].add((fmt, num, w, h))
    m = z_re.search(state)
    if m and m.group(4) == "1":
        a, f = f"{int(m.group(1), 16) & ~0xFFF:08x}", int(m.group(2))
        written[a] = ("Z", DEPTH.get(f, f), "?", "?")
print("textures sampled after being written this frame with the SAME format:", sum(same.values()))
print("sampled in a DIFFERENT format than written (reinterpretation), by kind:")
for k, n in reinterp.most_common(20):
    print(f"  {n:5}  {k}")
multi = {a: v for a, v in rt_formats.items() if len(v) > 1}
print(f"render-target addresses used with more than one format/size in the frame: {len(multi)}")
for a, v in list(multi.items())[:12]:
    print("  ", a, sorted(v))
