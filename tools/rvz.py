#!/usr/bin/env python3
"""Minimal GameCube RVZ/WIA reader and extractor (zstd / none only).

Format: Dolphin's docs/WiaAndRvz.md. Written from the format description; no
Dolphin code. Only what a GameCube disc needs (no Wii partitions).

Usage:
  python tools/rvz.py info  <disc.rvz>
  python tools/rvz.py extract <disc.rvz> <outdir>     # sys/ + files/ (FST tree)
  python tools/rvz.py iso <disc.rvz> <out.iso>        # full 1:1 ISO
"""
import ctypes
import os
import struct
import sys

# --- zstd via a local libzstd (msys64 ships one; override with RT_ZSTD_DLL) -------------
_ZSTD_CANDIDATES = [
    os.environ.get("RT_ZSTD_DLL", ""),
    r"C:\msys64\ucrt64\bin\libzstd.dll",
    r"C:\msys64\mingw64\bin\libzstd.dll",
    "libzstd.so.1",
    "libzstd.dylib",
]


def _load_zstd():
    for p in _ZSTD_CANDIDATES:
        if not p:
            continue
        try:
            if os.name == "nt" and os.path.isabs(p):
                os.add_dll_directory(os.path.dirname(p))
            lib = ctypes.CDLL(p)
            lib.ZSTD_decompress.restype = ctypes.c_size_t
            lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
            lib.ZSTD_isError.restype = ctypes.c_uint
            lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
            lib.ZSTD_getErrorName.restype = ctypes.c_char_p
            lib.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
            return lib
        except OSError:
            continue
    raise SystemExit("libzstd not found (set RT_ZSTD_DLL)")


_zstd = None


def zstd_decompress(src: bytes, capacity: int) -> bytes:
    global _zstd
    if _zstd is None:
        _zstd = _load_zstd()
    dst = ctypes.create_string_buffer(capacity)
    n = _zstd.ZSTD_decompress(dst, capacity, src, len(src))
    if _zstd.ZSTD_isError(n):
        raise ValueError("zstd: " + _zstd.ZSTD_getErrorName(n).decode())
    return dst.raw[:n]


# --- Lagged Fibonacci generator (junk data of GameCube/Wii discs) ----------------------
LFG_K, LFG_J, SEED_WORDS = 521, 32, 17


class LFG:
    def __init__(self, seed: bytes):
        b = list(struct.unpack(">17I", seed))
        for i in range(SEED_WORDS, LFG_K):
            b.append(((b[i - 17] << 23) & 0xFFFFFFFF) ^ (b[i - 16] >> 9) ^ b[i - 1])
        self.buf = b
        self.pos = 0  # byte position in the current 521-word block
        for _ in range(4):
            self._forward()
        self._bytes = None

    def _forward(self):
        b = self.buf
        for i in range(LFG_J):
            b[i] ^= b[i + LFG_K - LFG_J]
        for i in range(LFG_J, LFG_K):
            b[i] ^= b[i - LFG_J]
        self._bytes = None

    def _block(self):
        if self._bytes is None:
            self._bytes = struct.pack(">521I", *self.buf)
        return self._bytes

    def skip(self, n):
        self.pos += n
        while self.pos >= LFG_K * 4:
            self._forward()
            self.pos -= LFG_K * 4

    def get(self, n):
        out = bytearray()
        while n > 0:
            blk = self._block()
            take = min(n, LFG_K * 4 - self.pos)
            out += blk[self.pos:self.pos + take]
            self.pos += take
            n -= take
            if self.pos == LFG_K * 4:
                self._forward()
                self.pos = 0
        return bytes(out)


# --- RVZ container --------------------------------------------------------------------
GC_SECTOR = 0x8000


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0x48)
        self.magic, ver, _compat, disc_size = struct.unpack(">4sIII", h[:16])
        if self.magic not in (b"RVZ\x01", b"WIA\x01"):
            raise ValueError("not an RVZ/WIA file")
        self.rvz = self.magic == b"RVZ\x01"
        self.iso_size, _ = struct.unpack(">QQ", h[0x24:0x34])
        d = self.f.read(disc_size)
        self.disc_type, self.compr, _lvl, self.chunk = struct.unpack(">IIiI", d[:16])
        self.dhead = d[16:16 + 0x80]
        o = 16 + 0x80
        n_part, _pts, _poff = struct.unpack(">IIQ", d[o:o + 16])
        o += 16 + 20
        (n_raw, raw_off, raw_size, n_groups, group_off, group_size, _cdl) = struct.unpack(">IQIIQIB", d[o:o + 33])
        if self.disc_type != 1 or n_part:
            raise ValueError("only GameCube discs are supported")
        if self.compr not in (0, 5):
            raise ValueError(f"compression {self.compr} unsupported (zstd/none only)")
        raw = self._read_table(raw_off, raw_size, n_raw * 0x18)
        self.raw = [struct.unpack(">QQII", raw[i * 0x18:(i + 1) * 0x18]) for i in range(n_raw)]
        gt = self._read_table(group_off, group_size, n_groups * 12)
        self.groups = [struct.unpack(">III", gt[i * 12:(i + 1) * 12]) for i in range(n_groups)]
        self._cache = {}

    def _read_table(self, off, size, out_size):
        self.f.seek(off)
        data = self.f.read(size)
        if self.compr == 5:
            data = zstd_decompress(data, out_size)
        return data[:out_size]

    def _group(self, gi, data_start, group_bytes):
        """Decoded bytes of group gi, which covers disc offsets [data_start, +group_bytes)."""
        key = gi
        if key in self._cache:
            return self._cache[key]
        off4, size, packed = self.groups[gi]
        compressed = bool(size & 0x80000000) if self.rvz else self.compr != 0
        size &= 0x7FFFFFFF
        if size == 0:
            out = bytes(group_bytes)
        else:
            self.f.seek(off4 << 2)
            data = self.f.read(size)
            if compressed:
                data = zstd_decompress(data, max(packed, group_bytes) + 64)
            if self.rvz and packed:
                out = self._unpack(data, data_start, group_bytes)
            else:
                out = data[:group_bytes]
        if len(self._cache) > 64:
            self._cache.clear()
        self._cache[key] = out
        return out

    @staticmethod
    def _unpack(data, disc_off, want):
        out = bytearray()
        p = 0
        while len(out) < want and p < len(data):
            (n,) = struct.unpack(">I", data[p:p + 4])
            p += 4
            if n & 0x80000000:
                n &= 0x7FFFFFFF
                g = LFG(data[p:p + 68])
                p += 68
                g.skip((disc_off + len(out)) % GC_SECTOR)
                out += g.get(n)
            else:
                out += data[p:p + n]
                p += n
        return bytes(out[:want])

    def read(self, offset, size):
        out = bytearray()
        if offset < 0x80:
            take = min(size, 0x80 - offset)
            out += self.dhead[offset:offset + take]
            offset += take
            size -= take
        while size > 0:
            for (r_off, r_size, g_index, g_count) in self.raw:
                skipped = r_off % GC_SECTOR
                base = r_off - skipped
                end = r_off + r_size
                if base <= offset < end:
                    break
            else:
                out += bytes(size)  # outside every raw-data range: zeros
                break
            gi_local = (offset - base) // self.chunk
            g_start = base + gi_local * self.chunk
            g_bytes = min(self.chunk, end - g_start)
            blob = self._group(g_index + gi_local, g_start, g_bytes)
            within = offset - g_start
            take = min(size, g_bytes - within)
            out += blob[within:within + take]
            offset += take
            size -= take
        return bytes(out)


# --- GameCube filesystem --------------------------------------------------------------
def fst_entries(disc):
    hdr = disc.read(0, 0x440)
    fst_off, fst_size = struct.unpack(">II", hdr[0x424:0x42C])
    fst = disc.read(fst_off, fst_size)
    n = struct.unpack(">I", fst[8:12])[0]
    names = fst[n * 12:]

    def name_at(o):
        return names[o:names.index(b"\0", o)].decode("shift_jis", "replace")

    out = []  # (path, is_dir, offset, size)
    stack = [("", n)]
    i = 1
    while i < n:
        while stack and i >= stack[-1][1]:
            stack.pop()
        w0, w1, w2 = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        is_dir = w0 >> 24
        name = name_at(w0 & 0xFFFFFF)
        path = stack[-1][0] + name if stack else name
        if is_dir:
            out.append((path, True, 0, 0))
            stack.append((path + "/", w2))
        else:
            out.append((path, False, w1, w2))
        i += 1
    return out


def sys_files(disc):
    hdr = disc.read(0, 0x440)
    dol_off, fst_off, fst_size = struct.unpack(">III", hdr[0x420:0x42C])
    # apploader: header 0x20, size at +0x14, trailer at +0x18
    ah = disc.read(0x2440, 0x20)
    al_size = struct.unpack(">I", ah[0x14:0x18])[0] + struct.unpack(">I", ah[0x18:0x1C])[0]
    # DOL size from its header
    dh = disc.read(dol_off, 0x100)
    offs = struct.unpack(">18I", dh[0:0x48])
    sizes = struct.unpack(">18I", dh[0x90:0xD8])
    dol_size = max(o + s for o, s in zip(offs, sizes) if s)
    return {
        "boot.bin": (0, 0x440),
        "bi2.bin": (0x440, 0x2000),
        "apploader.img": (0x2440, 0x20 + al_size),
        "main.dol": (dol_off, dol_size),
        "fst.bin": (fst_off, fst_size),
    }


def copy_range(disc, off, size, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as o:
        while size:
            n = min(size, 1 << 22)
            o.write(disc.read(off, n))
            off += n
            size -= n


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    disc = Disc(path)
    if cmd == "info":
        print("game", disc.dhead[:6].decode(), "rev", disc.dhead[7], disc.dhead[0x20:0x60].split(b"\0")[0].decode())
        print("iso size", disc.iso_size, "chunk", hex(disc.chunk), "groups", len(disc.groups))
        for name, (o, s) in sys_files(disc).items():
            print(f"  sys/{name:14} @{o:#010x} {s:#x}")
        ents = fst_entries(disc)
        print(len([e for e in ents if not e[1]]), "files")
        return 0
    if cmd == "extract":
        out = argv[3]
        for name, (o, s) in sys_files(disc).items():
            copy_range(disc, o, s, os.path.join(out, "sys", name))
        for p, is_dir, o, s in fst_entries(disc):
            dst = os.path.join(out, "files", *p.split("/"))
            if is_dir:
                os.makedirs(dst, exist_ok=True)
            else:
                copy_range(disc, o, s, dst)
        print("extracted to", out)
        return 0
    if cmd == "iso":
        copy_range(disc, 0, disc.iso_size, argv[3])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
