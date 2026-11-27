# RL-NX (Stage 9): turns a VM census (logs/vmcensus.txt, written while the - menu's VM CENSUS row is on) into
# config/GSWE64/vmsites.txt, the list of guest instructions r2recomp gives the inline VM accessor forms (ctx.h rd32V
# and friends).
#
#   py -3 switch/tools/vm_census.py <vmcensus.txt> [more census files] --elf <the build's .elf>
#       [--gen build-switch/gen] [--out config/GSWE64/vmsites.txt] [--min 2000] [--merge]
#
# Each census line is a return address in the recompiled code (the instruction after a call to rdSlow/wrSlow). The
# file's "anchor" line is the runtime address of rl_nx_prof_anchor; the tool subtracts it from every address and
# adds the anchor's .elf address, asks addr2line which gen/code_NNN.cpp line made the call, and walks up that file
# to the "// PC: WORD" comment above it: the guest instruction. The .elf must be the build the census ran on, and
# the gen directory the one that build compiled (the gen changes once vmsites.txt does, so census, elf and gen go
# together). Counts are summed over every file given; --merge keeps the sites already in --out as well.
# A site needs --min accesses over the whole census (default 2000) to be listed.
#
# RL-NX (Stage 11): a census line has two more columns, the site's other slow accesses (locked cache, uncached view,
# IO, the gather pipe through a plain store); a fastmem build's logs/fmcensus.txt has its faults there too. --fastmem
# reads those columns instead and writes config/GSWE64/fmchecked.txt (the sites r2recomp R2RECOMP_FASTMEM=1 keeps
# tested), with --min 1 by default:
#   py -3 switch/tools/vm_census.py <vmcensus.txt / fmcensus.txt ...> --elf <elf> --fastmem [--merge]
import argparse
import bisect
import os
import re
import subprocess
import sys

DKA64 = "C:/devkitPro/devkitA64/bin/"


def nm_addr(elf, sym):
    out = subprocess.run([DKA64 + "aarch64-none-elf-nm", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == sym:
            return int(p[0], 16)
    sys.exit("vm_census: %s not in %s" % (sym, elf))


def read_census(path, other=False):  # RL-NX (Stage 11): other - columns 4 and 5
    anchor = None
    hits = []
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] == "anchor":
            anchor = int(p[1], 16)
        elif p[0] == "lost":
            if int(p[1]):
                print("vm_census: %s lost %s hits (table full)" % (path, p[1]))
        elif len(p) >= 3:  # RL-NX (Stage 11): 5 columns since Stage 11
            if other:
                if len(p) >= 5:
                    hits.append((int(p[0], 16), int(p[3]), int(p[4])))
            else:
                hits.append((int(p[0], 16), int(p[1]), int(p[2])))
    if anchor is None:
        sys.exit("vm_census: no anchor line in " + path)
    return anchor, hits


class GenFile:
    """A gen/code_NNN.cpp file: for a line number, the guest pc of the instruction comment above it."""

    def __init__(self, path):
        self.lines, self.pcs = [], []
        for n, text in enumerate(open(path, encoding="utf-8"), 1):
            m = re.match(r"\s*// ([0-9A-F]{8}): [0-9A-F]{8}$", text)
            if m:
                self.lines.append(n)
                self.pcs.append(int(m.group(1), 16))

    def pc_at(self, line):
        i = bisect.bisect_right(self.lines, line) - 1
        return self.pcs[i] if i >= 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census", nargs="+")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--gen", default="build-switch/gen")
    ap.add_argument("--out", default=None)
    ap.add_argument("--min", type=int, default=None)
    ap.add_argument("--merge", action="store_true")
    ap.add_argument("--fastmem", action="store_true")  # RL-NX (Stage 11)
    a = ap.parse_args()
    if a.out is None:
        a.out = "config/GSWE64/fmchecked.txt" if a.fastmem else "config/GSWE64/vmsites.txt"
    if a.min is None:
        a.min = 1 if a.fastmem else 2000

    elf_anchor = nm_addr(a.elf, "rl_nx_prof_anchor")
    addrs = {}  # .elf address -> [reads, writes]
    for path in a.census:
        anchor, hits = read_census(path, a.fastmem)
        for ra, rd, wr in hits:
            e = ra - anchor + elf_anchor - 4  # the call instruction, not the one after it
            v = addrs.setdefault(e, [0, 0])
            v[0] += rd
            v[1] += wr
    keys = sorted(addrs)
    # -a prints each address in front of its inline chain; -f puts the function name above each frame's location
    r = subprocess.run([DKA64 + "aarch64-none-elf-addr2line", "-a", "-f", "-i", "-e", a.elf],
                       input="".join("%x\n" % k for k in keys), capture_output=True, text=True)
    chains = []
    for line in r.stdout.splitlines():
        if line.startswith("0x"):
            chains.append([])
        elif chains:
            chains[-1].append(line)
    if len(chains) != len(keys):
        sys.exit("vm_census: addr2line gave %d chains for %d addresses" % (len(chains), len(keys)))

    gens = {}
    sites = {}  # guest pc -> [reads, writes]
    fns = {}    # guest function entry -> [reads, writes], for addresses with no gen line (no inline record)
    unmapped = [0, 0]
    for k, chain in zip(keys, chains):
        pc = fn = None
        for i in range(0, len(chain) - 1, 2):  # (function, location) per frame, innermost first
            m = re.search(r"f_([0-9A-F]{8})", chain[i])  # mangled: _ZN2rt10f_8000538CERNS_3CtxE
            if m:
                fn = int(m.group(1), 16)
            m = re.match(r"(.*code_\d{3}\.cpp):(\d+)", chain[i + 1].replace("\\", "/"))
            if m:  # a frame in a gen file is the recompiled statement
                name = os.path.basename(m.group(1))
                g = gens.get(name)
                if g is None:
                    g = gens[name] = GenFile(os.path.join(a.gen, name))
                pc = g.pc_at(int(m.group(2)))
        rd, wr = addrs[k]
        if pc is not None:
            v = sites.setdefault(pc, [0, 0])
        elif fn is not None:
            v = fns.setdefault(fn, [0, 0])
        else:
            unmapped[0] += rd
            unmapped[1] += wr
            continue
        v[0] += rd
        v[1] += wr

    total = sum(v[0] + v[1] for v in sites.values()) + sum(v[0] + v[1] for v in fns.values())
    keep = {("%08X" % pc): v for pc, v in sites.items() if v[0] + v[1] >= a.min}
    keep.update({("F %08X" % fn): v for fn, v in fns.items() if v[0] + v[1] >= a.min})
    if a.merge and os.path.exists(a.out):
        for line in open(a.out):
            p = line.split("#")[0].split()
            if p:
                key = " ".join(p[:2]) if p[0] == "F" else p[0]
                nums = p[2:] if p[0] == "F" else p[1:]
                keep.setdefault(key, [int(nums[0]), int(nums[1])] if len(nums) >= 2 else [0, 0])
    kept = sum(v[0] + v[1] for v in keep.values())
    with open(a.out, "w", newline="\n") as f:
        if a.fastmem:  # RL-NX (Stage 11)
            f.write("# RL-NX (Stage 11): guest memory accesses r2recomp R2RECOMP_FASTMEM=1 keeps tested (they reach memory that is\n")
            f.write("# not plain RAM: locked cache, uncached view, IO, gather pipe). Written by switch/tools/vm_census.py --fastmem from\n")
            f.write("# a census (- menu VM CENSUS) or a fastmem build's logs/fmcensus.txt.\n")
            f.write("# <pc> <reads> <writes>: one instruction; F <entry> ...: every access of that function\n")
        else:
            f.write("# RL-NX (Stage 9): guest memory accesses r2recomp emits with the inline VM forms (ctx.h rd32V ...).\n")
            f.write("# Written by switch/tools/vm_census.py from a VM census (- menu VM CENSUS).\n")
            f.write("# <pc> <VM reads> <VM writes>: one instruction; F <entry> ...: every access of that function (the census\n")
            f.write("# address had no gen line to name the instruction)\n")
        for key in sorted(keep, key=lambda q: -(keep[q][0] + keep[q][1])):
            f.write("%s %d %d\n" % (key, keep[key][0], keep[key][1]))
    nf = sum(1 for q in keep if q.startswith("F "))
    print("vm_census: %d return addresses -> %d instructions + %d functions (%d accesses); listed %d instructions and "
          "%d functions with >= %d in %s, covering %.1f%% of the accesses; %d reads / %d writes not mapped"
          % (len(keys), len(sites), len(fns), total, len(keep) - nf, nf, a.min, a.out, 100.0 * kept / max(total, 1),
             unmapped[0], unmapped[1]))


if __name__ == "__main__":
    main()
