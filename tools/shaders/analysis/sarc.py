"""Minimal SARC/Yaz0 reader for finding GX2 shader archives in the game's packs.

  uv run python -I tools/shaders/analysis/sarc.py <file.pack|.sarc|.s*> [--find bfsha] [--extract name outfile]
"""
import struct
import sys


def yaz0(data: bytes) -> bytes:
    if data[:4] != b"Yaz0":
        return data
    size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(size)
    src, dst = 16, 0
    while dst < size:
        code = data[src]
        src += 1
        for bit in range(8):
            if dst >= size:
                break
            if code & (0x80 >> bit):
                out[dst] = data[src]
                dst += 1
                src += 1
            else:
                b1, b2 = data[src], data[src + 1]
                src += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12
                    src += 1
                else:
                    n += 2
                for _ in range(n):
                    out[dst] = out[dst - dist]
                    dst += 1
    return bytes(out)


def sarc(data: bytes):
    data = yaz0(data)
    assert data[:4] == b"SARC", data[:4]
    bom = data[6:8]
    e = ">" if bom == b"\xfe\xff" else "<"
    hdr_len, = struct.unpack_from(e + "H", data, 4)
    data_off, = struct.unpack_from(e + "I", data, 0x0C)
    sfat = hdr_len
    assert data[sfat:sfat + 4] == b"SFAT"
    count, = struct.unpack_from(e + "H", data, sfat + 6)
    nodes = []
    for i in range(count):
        h, attr, s, end = struct.unpack_from(e + "IIII", data, sfat + 12 + i * 16)
        nodes.append((attr, s, end))
    sfnt = sfat + 12 + count * 16
    names_off = sfnt + 8
    for attr, s, end in nodes:
        name = ""
        if attr & 0x01000000:
            o = names_off + (attr & 0xFFFF) * 4
            name = data[o:data.index(b"\0", o)].decode()
        yield name, data[data_off + s:data_off + end]


if __name__ == "__main__":
    raw = open(sys.argv[1], "rb").read()
    find = sys.argv[sys.argv.index("--find") + 1] if "--find" in sys.argv else None
    ext = sys.argv[sys.argv.index("--extract") + 1:sys.argv.index("--extract") + 3] if "--extract" in sys.argv else None
    for name, blob in sarc(raw):
        if ext and name == ext[0]:
            open(ext[1], "wb").write(yaz0(blob))
        if find and find not in name:
            continue
        print(f"{len(blob):10}  {name}  {blob[:4]}")
