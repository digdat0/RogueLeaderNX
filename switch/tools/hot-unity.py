#!/usr/bin/env python3
"""RL-NX (9.39): cluster the hottest recompiled guest functions into one translation unit.

Why: every generated guest function takes `Ctx&` and the guest's registers live in that struct, so a call to
another guest function forces the caller to write its live registers back and reload them afterwards - Clang
cannot keep them in host registers across a call it cannot see into. The functions live one batch of files per
`code_NNN.cpp`, so a hot caller and its callee are rarely in the same TU and nothing gets inlined. 9.37
measured 20.6 host instructions per guest instruction for that reason.

This collects the functions the profile says are hot, plus the functions they call, into `gen/code_hot.cpp`
and deletes them from their original files, so Clang can inline inside the cluster and keep the guest's
registers in host registers across those call boundaries.

    py -3 switch/tools/hot-unity.py <profile-NNN.txt> <elf-of-that-build> [--top N] [--print-only]

The ELF must be the one that build wrote (profile pcs are absolute, so a different build's ELF names the wrong
functions - check the report's "build ..." line). Re-run this after regenerating `gen/`: a fresh gen puts every
function back in its original file and leaves a stale code_hot.cpp. `gen/code_hot.cpp` matches the existing
`code_*.cpp` glob in switch/CMakeLists.txt, so no CMake change is needed.
"""
import bisect
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(ROOT, "build-switch", "gen")
HOT_FILE = os.path.join(GEN, "code_hot.cpp")
NM = os.environ.get("RL_NX_NM") or r"C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe"

DEF_RE = re.compile(r"void (f_[0-9A-Fa-f_]+)\(Ctx& __restrict c\) \{(.*?)\n\}\n", re.S)
CALL_RE = re.compile(r"\b(f_[0-9A-Fa-f_]+)\(c\)")


def symbols(elf):
    """(sorted addrs, names) for the generated guest functions, from nm."""

    def run():
        for nm in (NM, "aarch64-none-elf-nm", "aarch64-none-elf-nm.exe"):
            try:
                return subprocess.run([nm, "-C", "--defined-only", elf], capture_output=True, text=True).stdout
            except (FileNotFoundError, OSError):
                continue
        sys.exit("hot-unity: no aarch64-none-elf-nm (set RL_NX_NM)")

    addrs, names = [], []
    anchor = None
    for ln in run().splitlines():
        f = ln.split(None, 2)
        if len(f) != 3:
            continue
        try:
            addr = int(f[0], 16)
        except ValueError:
            continue
        # The report's pcs are runtime addresses; prof_report.py converts them with the bias between the
        # report's anchor and this ELF's rl_nx_prof_anchor, and this has to do the same or it silently names
        # the wrong functions (a first attempt matched one function and 100% of the samples).
        if f[2] == "rl_nx_prof_anchor":
            anchor = addr
        if not f[2].startswith("rt::f_"):
            continue
        addrs.append(addr)
        names.append(f[2][4:].split("(")[0])  # rt::f_80032738(rt::Ctx&) -> f_80032738
    if anchor is None:
        sys.exit("hot-unity: rl_nx_prof_anchor is not in %s (not a profiler build)" % elf)
    order = sorted(range(len(addrs)), key=lambda i: addrs[i])
    return [addrs[i] for i in order], [names[i] for i in order], anchor


def hot_by_samples(profile, elf, top):
    addrs, names, elf_anchor = symbols(elf)
    head = {}
    rows = []
    for ln in open(profile, encoding="utf-8", errors="replace"):
        f = ln.split()
        if len(f) == 4 and f[0].isdigit() and f[1].isdigit() and f[0] == "0":  # the guest thread only
            rows.append((int(f[2], 16), int(f[1])))
        elif len(f) == 2 and f[0] in ("anchor", "build"):
            head[f[0]] = f[1]
    if "anchor" not in head:
        sys.exit("hot-unity: no anchor line in %s" % profile)
    bias = int(head["anchor"], 16) - elf_anchor
    print("hot-unity: report %s, bias 0x%x" % (head.get("build", "?"), bias & 0xFFFFFFFF))
    samples = {}
    for pc, n in rows:
        if True:
            pc = pc - bias
            i = bisect.bisect_right(addrs, pc) - 1
            if i >= 0:
                samples[names[i]] = samples.get(names[i], 0) + n
    ranked = sorted(samples.items(), key=lambda kv: -kv[1])
    return [n for n, _ in ranked[:top]], sum(samples.values()), samples


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    profile, elf = argv[1], argv[2]
    top = int(argv[argv.index("--top") + 1]) if "--top" in argv else 60
    print_only = "--print-only" in argv
    # RL-NX (9.40): --flatten marks the hot functions __attribute__((flatten)), which orders Clang to inline
    # every call it can reach inside them. That is the whole point: `Ctx& __restrict c` is what lets Clang keep
    # the guest's registers in host registers, but only while it can see the code that would clobber them - one
    # opaque call and every live guest register goes back to memory. The inliner's cost model declines these
    # callees on its own (raising -inline-threshold to 2500 inlined only ~14% of them), so the attribute does
    # the ordering instead of a number.
    # On by default: a cluster without it is inert (9.39: byte-identical output), so the flag is only kept to
    # switch the attribute off for an A/B.
    flatten = "--no-flatten" not in argv

    hot, total, samples = hot_by_samples(profile, elf, top)
    print("hot-unity: %d sampled guest-thread functions, top %d = %.1f%% of guest samples" %
          (len(samples), len(hot), 100.0 * sum(samples[n] for n in hot) / max(total, 1)))

    # index every definition in gen/
    bodies, files = {}, {}
    for path in glob.glob(os.path.join(GEN, "code_*.cpp")):
        if os.path.basename(path) == "code_hot.cpp":
            continue
        txt = open(path, encoding="utf-8", errors="replace").read()
        for m in DEF_RE.finditer(txt):
            bodies[m.group(1)] = m.group(0)
            files[m.group(1)] = path

    # the cluster: the hot functions plus what they call (one level - enough to inline the hot chain)
    cluster = [n for n in hot if n in bodies]
    for n in list(cluster):
        for callee in CALL_RE.findall(bodies[n]):
            if callee in bodies and callee not in cluster:
                cluster.append(callee)
    print("hot-unity: cluster = %d functions (%d hot + %d callees), %d lines" %
          (len(cluster), len(hot), len(cluster) - len(hot),
           sum(bodies[n].count("\n") for n in cluster)))
    if print_only:
        for n in cluster[:20]:
            print("   ", n, files[n].split(os.sep)[-1])
        return

    # write the cluster, then cut those definitions out of their files. Cutting by exact text keeps every
    # other byte of the generated code untouched.
    with open(HOT_FILE, "w", encoding="utf-8", newline="\n") as out:
        out.write("// Generated by switch/tools/hot-unity.py from a profile - do not edit.\n"
                  "// RL-NX (9.39): the guest's hot call chains, together in one translation unit so Clang can\n"
                  "// inline them and keep the guest's registers in host registers across the calls.\n"
                  '#include "funcs.h"\n\nnamespace rt {\n\n')
        hot_set = set(hot)
        for n in cluster:
            body = bodies[n]
            if flatten and n in hot_set:
                body = body.replace("void %s(Ctx& __restrict c)" % n,
                                    "__attribute__((flatten)) void %s(Ctx& __restrict c)" % n, 1)
            out.write(body + "\n")
        out.write("}  // namespace rt\n")

    for path in sorted(set(files[n] for n in cluster)):
        txt = open(path, encoding="utf-8", errors="replace").read()
        for n in cluster:
            if files[n] == path:
                txt = txt.replace(bodies[n], "")
        open(path, "w", encoding="utf-8", newline="\n").write(txt)
    print("hot-unity: wrote %s and cut those functions from %d files" %
          (os.path.basename(HOT_FILE), len(set(files[n] for n in cluster))))


if __name__ == "__main__":
    main(sys.argv)
