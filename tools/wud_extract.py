"""Extract the game partition (GM) of a Wii U disc image (.wud) to a folder.

usage: WIIU_COMMON_KEY=<hex or file> uv run python -I tools/wud_extract.py <image.wud|.wux> <disc key: hex string or .key file> <out dir>

Layout (see Cemu src/Cafe/Filesystem/FST/FST.cpp, which this follows):
  0x18000          partition table, AES-128-CBC with the disc key, IV 0
  partition start  partition header (magic CC93A4F5), FST at fstSector * 0x8000
  SI partition     FST encrypted with the disc key; holds <NN>/title.tik for partition NN
  GM partition     FST + contents encrypted with the title key from that ticket
Clusters are either raw (one CBC stream per cluster, IV = cluster index) or hashed
(0x10000 blocks: 0x400 bytes of hashes, IV 0, then 0xFC00 data bytes, IV = H0[block % 16]).
"""
import hashlib
import os
import struct
import sys
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

SECTOR = 0x8000
HBLOCK = 0x10000
HHASH = 0x400
HDATA = 0xFC00
# The Wii U common key (decrypts ticket title keys) is not included: pass it in WIIU_COMMON_KEY as 32 hex
# digits, or the path of a file holding them (or the 16 raw bytes). Checked against this SHA-256.
COMMON_KEY_SHA256 = "3b4011e584ae694f59f49df13aa5f471a4230ff7c2c7078361b7d624b8eeaa9b"


def common_key() -> bytes:
    v = os.environ.get("WIIU_COMMON_KEY", "").strip()
    if os.path.isfile(v):
        raw = open(v, "rb").read()
        v = raw.hex() if len(raw) == 16 else raw.decode(errors="replace").strip()
    try:
        key = bytes.fromhex(v)
    except ValueError:
        key = b""
    if hashlib.sha256(key).hexdigest() != COMMON_KEY_SHA256:
        raise SystemExit("set WIIU_COMMON_KEY to the Wii U common key (32 hex digits, or a file containing it)")
    return key


class DiscImage:
    """Plain .wud or WUX-compressed (.wux: deduplicated sectors + an index table) image."""

    def __init__(self, path: str):
        self.f = open(path, "rb")
        hdr = self.f.read(32)
        m0, m1, ssize, _pad, usize = struct.unpack_from("<IIIIQ", hdr, 0)
        self.wux = m0 == 0x30585557 and m1 == 0x1099D02E
        self.pos = 0
        if self.wux:
            self.ssize, self.size = ssize, usize
            n = (usize + ssize - 1) // ssize
            self.index = struct.unpack(f"<{n}I", self.f.read(4 * n))
            arr = 32 + 4 * n
            self.sectors = (arr + ssize - 1) // ssize * ssize
        else:
            self.f.seek(0, 2)
            self.size = self.f.tell()

    def seek(self, pos: int) -> None:
        self.pos = pos

    def read(self, n: int) -> bytes:
        if not self.wux:
            self.f.seek(self.pos)
            b = self.f.read(n)
            self.pos += len(b)
            return b
        out = []
        while n > 0 and self.pos < self.size:
            si, so = divmod(self.pos, self.ssize)
            k = min(n, self.ssize - so)
            self.f.seek(self.sectors + self.index[si] * self.ssize + so)
            out.append(self.f.read(k))
            self.pos += k
            n -= k
        return b"".join(out)


def aes_dec(key: bytes, iv: bytes, data: bytes) -> bytes:
    d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    return d.update(data) + d.finalize()


class Volume:
    """One FST-described partition."""

    def __init__(self, f, base: int, fst_off: int, fst_size: int, key: bytes):
        self.f, self.base, self.key = f, base, key
        padded = (fst_size + 15) & ~15
        fst = aes_dec(key, bytes(16), self.read_raw(fst_off, padded))[:fst_size]
        magic, self.offset_factor, ncluster = struct.unpack_from(">III", fst, 0)
        if magic != 0x46535400:
            raise SystemExit(f"bad FST magic {magic:08x} (wrong key?)")
        self.clusters = []
        for i in range(ncluster):
            off, size, _owner, _grp, hmode = struct.unpack_from(">IIQIB", fst, 0x20 + i * 0x20)
            self.clusters.append((off, size, hmode))
        ft = 0x20 + ncluster * 0x20
        nent = struct.unpack_from(">I", fst, ft + 8)[0]
        names = fst[ft + nent * 16:]
        self.entries = []  # (path, is_dir, file_offset, size, cluster)
        stack = [(0, nent, "")]
        for i in range(nent):
            tn, off, size, _flags, cl = struct.unpack_from(">IIIHH", fst, ft + i * 16)
            while i >= stack[-1][1]:
                stack.pop()
            nend = names.index(b"\0", tn & 0xFFFFFF)
            name = names[tn & 0xFFFFFF:nend].decode("utf-8", "replace")
            if i == 0:
                continue
            path = stack[-1][2] + name
            if tn >> 24 & 1:
                self.entries.append((path, True, 0, 0, 0))
                stack.append((i, size, path + "/"))
            else:
                self.entries.append((path, False, off * self.offset_factor, size, cl))

    def read_raw(self, off: int, size: int) -> bytes:
        self.f.seek(self.base + off)
        b = self.f.read(size)
        if len(b) != size:
            raise SystemExit(f"short read at {self.base + off:#x}")
        return b

    def read_file(self, file_off: int, size: int, cl: int, out) -> int:
        coff, _csize, hmode = self.clusters[cl]
        cbase = coff * SECTOR
        bad = 0
        if hmode in (0, 1):  # raw: a single CBC stream per cluster
            first = file_off // SECTOR
            pos = first * SECTOR
            end = file_off + size
            if first == 0:
                iv = struct.pack(">H", cl) + bytes(14)
            else:
                iv = self.read_raw(cbase + pos - 16, 16)
            dec = Cipher(algorithms.AES(self.key), modes.CBC(iv)).decryptor()
            while pos < end:
                n = min(8 * 1024 * 1024, ((end - pos) + SECTOR - 1) // SECTOR * SECTOR)
                plain = dec.update(self.read_raw(cbase + pos, n))
                lo = max(file_off - pos, 0)
                hi = min(end - pos, n)
                out.write(plain[lo:hi])
                pos += n
            return 0
        if hmode != 2:
            raise SystemExit(f"cluster {cl}: unknown hash mode {hmode}")
        blk = file_off // HDATA
        within = file_off % HDATA
        left = size
        while left > 0:
            raw = self.read_raw(cbase + blk * HBLOCK, HBLOCK)
            hashes = aes_dec(self.key, bytes(16), raw[:HHASH])
            h0 = hashes[(blk % 16) * 20:(blk % 16) * 20 + 20]
            data = aes_dec(self.key, h0[:16], raw[HHASH:])
            if hashlib.sha1(data).digest() != h0:
                bad += 1
            n = min(left, HDATA - within)
            out.write(data[within:within + n])
            left -= n
            within = 0
            blk += 1
        return bad

    def read_bytes(self, path: str) -> bytes:
        import io
        for p, is_dir, off, size, cl in self.entries:
            if p == path and not is_dir:
                buf = io.BytesIO()
                self.read_file(off, size, cl, buf)
                return buf.getvalue()
        raise KeyError(path)


def main() -> None:
    img, keyarg, out = sys.argv[1], sys.argv[2], sys.argv[3]
    ckey = common_key()
    disc_key = open(keyarg, "rb").read(16) if os.path.exists(keyarg) else bytes.fromhex(keyarg)
    f = DiscImage(img)
    f.seek(0x10000)
    if struct.unpack(">I", f.read(4))[0] != 0xCC549EB9:
        raise SystemExit("not a WUD image (header B magic mismatch)")
    f.seek(3 * SECTOR)
    pt = aes_dec(disc_key, bytes(16), f.read(SECTOR))
    magic, bsize = struct.unpack_from(">II", pt, 0)
    if magic != 0xCCA6E67B:
        raise SystemExit("partition table did not decrypt - wrong disc key")
    nparts = struct.unpack_from(">I", pt, 0x1C)[0]
    parts = []
    for i in range(nparts):
        e = pt[0x800 + i * 0x80:0x800 + (i + 1) * 0x80]
        name = e[:31].split(b"\0")[0].decode()
        addr = struct.unpack_from(">I", e, 0x20)[0]
        parts.append((name, addr * SECTOR))
    print("partitions:", ", ".join(f"{n}@{a:#x}" for n, a in parts))

    def header(base):
        f.seek(base)
        h = f.read(0x40)
        fst_size, fst_sector = struct.unpack_from(">II", h, 0x14)
        return fst_sector * SECTOR, fst_size

    si_index = next(i for i, (n, _) in enumerate(parts) if n.startswith("SI"))
    gm_index = next(i for i, (n, _) in enumerate(parts) if n.startswith("GM"))
    if os.environ.get("WUD_PARTITION"):  # pick another partition by index (e.g. a second GM partition)
        gm_index = int(os.environ["WUD_PARTITION"])
    si_base = parts[si_index][1]
    si = Volume(f, si_base, *header(si_base), disc_key)
    tik = si.read_bytes(f"{gm_index:02x}/title.tik")
    tmd = si.read_bytes(f"{gm_index:02x}/title.tmd")
    title_id = tik[0x1DC:0x1E4]
    title_key = aes_dec(ckey, title_id + bytes(8), tik[0x1BF:0x1CF])
    print(f"GM partition {parts[gm_index][0]}  title {title_id.hex()}  version {struct.unpack_from('>H', tmd, 0x1DC)[0]}")

    gm_base = parts[gm_index][1]
    gm = Volume(f, gm_base, *header(gm_base), title_key)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "title.tik"), "wb") as w:
        w.write(tik)
    with open(os.path.join(out, "title.tmd"), "wb") as w:
        w.write(tmd)
    files = [e for e in gm.entries if not e[1]]
    if os.environ.get("WUD_LIST"):
        for path, is_dir, off, size, cl in gm.entries:
            if not is_dir:
                print(f"{size:12} {path}")
        return
    total = sum(e[3] for e in files)
    done = 0
    bad = 0
    t0 = time.time()
    last = t0
    for path, is_dir, off, size, cl in gm.entries:
        dst = os.path.join(out, *path.split("/"))
        if is_dir:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as w:
            bad += gm.read_file(off, size, cl, w)
        done += size
        if time.time() - last > 15:
            last = time.time()
            print(f"  {done / total * 100:5.1f}%  {done / 2**30:.2f}/{total / 2**30:.2f} GiB  "
                  f"{done / (last - t0) / 2**20:.0f} MiB/s", flush=True)
    print(f"extracted {len(files)} files, {total / 2**30:.2f} GiB, {bad} hash mismatches, {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
