#!/usr/bin/env python3
# RL-NX: hd_pack_shrink.py - make a Dolphin-format HD texture pack the right size for the Switch.
#
# The GSWE64 HD pack ships every texture as DXT5 (BC3) DDS at about 4x the GameCube size, up to 4096x4096,
# with a full mip chain (22 MiB a file, 6.1 GB in all). The console reads the whole file from the SD card
# before Aurora's HD TEX 2048 limit throws the top levels away, which is the multi-second load.
#
# This tool does the same cut offline and without loss:
#   1. Picks a target size per texture from its GameCube size (in the file name, tex1_WxH_...):
#      target = clamp(gc_size * --scale, gc_size, --max). Never above the file, never below the GameCube size.
#   2. Drops the mip levels above the target. The levels that stay are the pack's own, byte for byte, so the
#      picture is exactly what the full pack shows at that distance (no re-encode, no quality loss).
#   3. With --bc1 (default on): a texture whose every kept texel has alpha 255 is transcoded BC3 -> BC1, which
#      halves it again. Also lossless: BC3's colour block is a BC1 4-colour block; endpoints are swapped (and
#      indices remapped) where BC1 would otherwise read it as 3-colour + transparent.
#   4. Writes a mirror of the input tree to --out. File names stay exactly the same (they carry the hash).
#
# Usage (py -3, not Git Bash's msys python):
#   py -3 switch/tools/hd_pack_shrink.py <pack dir> <out dir> [--preset balanced|quality|light]
#                                        [--scale N] [--max N] [--no-bc1] [--dry-run] [--jobs N]
# Presets: balanced = scale 4, max 1024 (default); quality = scale 4, max 2048; light = scale 2, max 512.
# Re-running skips files whose output is newer than the input (use --force to redo all).
# Then copy <out dir>/GSWE64 to sdmc:/switch/RL-NX/mods/hd/ (replacing the 4K pack).

import argparse
import multiprocessing
import os
import re
import struct
import sys
import time

# RL-NX: DDS header layout (DDS_HEADER, after the 4-byte magic). Offsets below are from the file start.
DDS_MAGIC = b"DDS "
OFF_FLAGS, OFF_HEIGHT, OFF_WIDTH, OFF_PITCH, OFF_MIPS = 8, 12, 16, 20, 28
OFF_PF_FLAGS, OFF_FOURCC, OFF_CAPS = 80, 84, 108
HEADER_SIZE = 128
DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE = 0x20000, 0x80000
DDSCAPS_COMPLEX, DDSCAPS_MIPMAP = 0x8, 0x400000

NAME_RE = re.compile(r"tex1_(\d+)x(\d+)_", re.IGNORECASE)
PRESETS = {"balanced": (4, 1024), "quality": (4, 2048), "light": (2, 512)}


# RL-NX: bytes of one BCn mip level (4x4 blocks; BC1 8 bytes a block, BC3 16).
def level_size(w, h, block_bytes):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block_bytes


# RL-NX: True when every texel of a BC3 alpha block is 255 (exact BC3 alpha decode, per the D3D spec).
def alpha_block_opaque(a0, a1, bits):
    if a0 > a1:
        # 8-value mode: only code 0 (= a0) can be 255, and only when a0 is.
        return a0 == 255 and bits == 0
    for i in range(16):
        code = (bits >> (3 * i)) & 7
        if code == 7:
            continue  # 6-value mode: code 7 is 255
        if code == 6:
            return False  # code 6 is 0
        if code == 0 and a0 != 255:
            return False
        if code == 1 and a1 != 255:
            return False
        if 2 <= code <= 5 and not (a0 == 255 and a1 == 255):
            return False
    return True


# RL-NX: True when every kept level of a BC3 texture is fully opaque. Fast path for the common encoder output
# (a0 = a1 = 255, all indices 0); everything else is decoded exactly. Stops at the first non-opaque block.
OPAQUE_FAST = b"\xff\xff\x00\x00\x00\x00\x00\x00"


def bc3_opaque(data):
    for off in range(0, len(data), 16):
        ab = data[off:off + 8]
        if ab == OPAQUE_FAST:
            continue
        if not alpha_block_opaque(ab[0], ab[1], int.from_bytes(ab[2:8], "little")):
            return False
    return True


# RL-NX: BC3 -> BC1 for opaque data. BC3's colour block always decodes in 4-colour mode; BC1 does so only when
# c0 > c1. c0 < c1: swap the endpoints and flip bit 0 of each 2-bit index (0<->1, 2<->3: same palette, exact).
# c0 == c1: every 4-colour entry equals c0, so all indices become 0 (index 3 would be transparent in BC1).
def bc3_to_bc1(data):
    out = bytearray(len(data) // 2)
    o = 0
    for c0, c1, idx in struct.iter_unpack("<8xHHI", data):
        if c0 > c1:
            struct.pack_into("<HHI", out, o, c0, c1, idx)
        elif c0 < c1:
            struct.pack_into("<HHI", out, o, c1, c0, idx ^ 0x55555555)
        else:
            struct.pack_into("<HHI", out, o, c0, c1, 0)
        o += 8
    return bytes(out)


# RL-NX: convert one file. Returns (rel path, in bytes, out bytes, note) for the report.
def convert(job):
    src, dst, rel, scale, cap, bc1, dry, force = job
    in_size = os.path.getsize(src)
    if not dry and not force and os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return rel, in_size, os.path.getsize(dst), "up to date"

    m = NAME_RE.match(os.path.basename(rel))
    with open(src, "rb") as f:
        blob = f.read()
    if blob[:4] != DDS_MAGIC or len(blob) < HEADER_SIZE or blob[OFF_FOURCC:OFF_FOURCC + 4] != b"DXT5":
        return rel, in_size, 0, "skipped: not a DXT5 DDS (copy it by hand if needed)"
    h, w = struct.unpack_from("<II", blob, OFF_HEIGHT)
    mips = max(1, struct.unpack_from("<I", blob, OFF_MIPS)[0])

    gc = max(int(m.group(1)), int(m.group(2))) if m else max(w, h)
    target = min(max(gc * scale, gc), cap) if cap > 0 else gc * scale
    target = max(target, gc)  # never below the GameCube size

    # Drop top levels while the level is larger than the target (always keep at least one level).
    drop, skip = 0, 0
    while drop + 1 < mips and max(max(w >> drop, 1), max(h >> drop, 1)) > target:
        skip += level_size(max(w >> drop, 1), max(h >> drop, 1), 16)
        drop += 1
    nw, nh, nmips = max(w >> drop, 1), max(h >> drop, 1), mips - drop
    data = blob[HEADER_SIZE + skip:]
    expect = sum(level_size(max(nw >> i, 1), max(nh >> i, 1), 16) for i in range(nmips))
    if len(data) < expect:
        return rel, in_size, 0, "skipped: file shorter than its header says"
    data = data[:expect]

    fourcc, note = b"DXT5", ""
    if bc1 and bc3_opaque(data):
        data, fourcc, note = bc3_to_bc1(data), b"DXT1", "BC1"
    block = 8 if fourcc == b"DXT1" else 16

    hdr = bytearray(blob[:HEADER_SIZE])
    struct.pack_into("<II", hdr, OFF_HEIGHT, nh, nw)
    struct.pack_into("<I", hdr, OFF_PITCH, level_size(nw, nh, block))
    struct.pack_into("<I", hdr, OFF_MIPS, nmips)
    flags = struct.unpack_from("<I", hdr, OFF_FLAGS)[0] | DDSD_LINEARSIZE
    caps = struct.unpack_from("<I", hdr, OFF_CAPS)[0]
    if nmips > 1:
        flags |= DDSD_MIPMAPCOUNT
        caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    struct.pack_into("<I", hdr, OFF_FLAGS, flags)
    struct.pack_into("<I", hdr, OFF_CAPS, caps)
    hdr[OFF_FOURCC:OFF_FOURCC + 4] = fourcc
    out = bytes(hdr) + data

    size_note = f"{w}x{h} -> {nw}x{nh}" + (f" {note}" if note else "")
    if not dry:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".tmp"
        with open(tmp, "wb") as f:
            f.write(out)
        os.replace(tmp, dst)
    return rel, in_size, len(out), size_note


def main():
    ap = argparse.ArgumentParser(description="Shrink a Dolphin-format DDS HD pack for the Switch (lossless mip cut).")
    ap.add_argument("src", help="pack folder (the one holding GSWE64/, or GSWE64 itself)")
    ap.add_argument("out", help="output folder (mirrors the input tree)")
    ap.add_argument("--preset", choices=sorted(PRESETS), default="balanced")
    ap.add_argument("--scale", type=int, help="target = GameCube size x this (overrides the preset)")
    ap.add_argument("--max", type=int, help="largest side kept, in pixels (overrides the preset)")
    ap.add_argument("--no-bc1", action="store_true", help="keep opaque textures as BC3")
    ap.add_argument("--dry-run", action="store_true", help="report sizes, write nothing")
    ap.add_argument("--force", action="store_true", help="redo files that are already up to date")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    a = ap.parse_args()

    scale, cap = PRESETS[a.preset]
    scale = a.scale or scale
    cap = a.max if a.max is not None else cap
    src, out = os.path.abspath(a.src), os.path.abspath(a.out)
    # RL-NX: different drives can't nest (commonpath raises on them).
    if os.path.splitdrive(src)[0].lower() == os.path.splitdrive(out)[0].lower() and \
            os.path.commonpath([src, out]) == src:
        sys.exit("out must not be inside src")

    jobs = []
    for dp, _, files in os.walk(src):
        for f in files:
            if f.lower().endswith(".dds"):
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, src)
                jobs.append((p, os.path.join(out, rel), rel, scale, cap, not a.no_bc1, a.dry_run, a.force))
    jobs.sort()
    print(f"{len(jobs)} DDS files, scale {scale}x, max {cap}, BC1 {'off' if a.no_bc1 else 'on'}, "
          f"{a.jobs} jobs{' (dry run)' if a.dry_run else ''}")

    t0, tin, tout, bc1n, skipped = time.time(), 0, 0, 0, []
    with multiprocessing.Pool(a.jobs) as pool:
        for n, (rel, isz, osz, note) in enumerate(pool.imap_unordered(convert, jobs, chunksize=4), 1):
            tin += isz
            tout += osz
            bc1n += note.endswith("BC1")
            if note.startswith("skipped"):
                skipped.append(f"{rel}: {note}")
            if n % 50 == 0 or n == len(jobs):
                print(f"  {n}/{len(jobs)}  {tin / 2**30:.2f} GiB -> {tout / 2**30:.2f} GiB  "
                      f"({time.time() - t0:.0f} s)", flush=True)

    print(f"done: {tin / 2**30:.2f} GiB -> {tout / 2**30:.2f} GiB ({100 * tout / max(tin, 1):.1f} %), "
          f"{bc1n} opaque textures as BC1, {len(skipped)} skipped, {time.time() - t0:.0f} s")
    for s in skipped:
        print("  " + s)


if __name__ == "__main__":
    main()
