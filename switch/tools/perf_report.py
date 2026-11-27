#!/usr/bin/env python3
r"""Summarise an RL-NX PERF LOG run (the [pm*] and [hitch] lines in logs/log.txt) - PERF_HANDOFF 10.x.

    py -3 switch/tools/perf_report.py "E:/RL-NXLOGS/LOGS <date>/log.txt" [--level N] [--from S] [--to S]

The log holds the last runs; this reads the newest one (the last "build" line onwards). For each level
seen it prints where a game frame's time went (game thread vs the hand-off to the render worker, and the
game thread's timed spans), the slow-path memory traffic, GX and deko3d figures and per-thread CPU, all
averaged over that level's seconds, then the worst hitches. Times are ms per game frame; counts are per
game frame. "untimed" = game time the timers do not cover: the recompiled code itself, the GX bridge and
everything else the 1 kHz profiler (prof_report.py) has to break down.
"""
import argparse
import re
import sys
from collections import defaultdict

KV = re.compile(r"(\w+)=([-\w.]+)")


def parse(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    start = 0
    for i, l in enumerate(lines):
        if "RL-NX (Rogue Leader)" in l and "build" in l:
            start = i
    build = lines[start] if lines else ""
    secs, hitches = [], []
    cur = None
    for l in lines[start:]:
        m = re.search(r"\[(pm|pm-mem|pm-gx|pm-dk|pm-thr|hitch)\] (.*)", l)
        if not m:
            continue
        kind, rest = m.groups()
        kv = {}
        for k, v in KV.findall(rest):
            try:
                kv[k] = float(v)
            except ValueError:
                kv[k] = v
        if kind == "hitch":
            hitches.append(kv)
        elif kind == "pm":
            if "t" not in kv:
                continue  # the on/off and legend lines
            cur = {"pm": kv}
            secs.append(cur)
        elif cur is not None and kv.get("t") == cur["pm"].get("t"):
            cur[kind] = kv
    return build, secs, hitches


def mean(rows, key, sub="pm"):
    vals = [r[sub][key] for r in rows if sub in r and isinstance(r[sub].get(key), float)]
    return sum(vals) / len(vals) if vals else 0.0


def frame_weighted(rows, key, sub):
    # per-frame values weighted by each second's frame count
    tot = n = 0.0
    for r in rows:
        if sub in r and isinstance(r[sub].get(key), float):
            f = r["pm"].get("frames", 0.0)
            tot += r[sub][key] * f
            n += f
    return tot / n if n else 0.0


def report(rows, title):
    if not rows:
        return
    fr = sum(r["pm"].get("frames", 0) for r in rows)
    secs = len(rows)
    w = lambda k, s="pm": frame_weighted(rows, k, s)
    game, present = w("game"), w("present")
    timed = {k: w(k) for k in ("idle", "dsp", "disc", "audio", "lcdma", "gxsync")}  # RL-NX (9.53): + gxsync
    print("\n== %s: %d s, %d game frames, %.1f fps (game frames/s)" % (title, secs, fr, fr / max(secs, 1)))
    h = [sum(r["pm"].get(k, 0) for r in rows) for k in ("h16", "h33", "h50", "hx")]
    print("   frame times: <=16.7 ms %d, <=33.3 %d, <=50 %d, >50 %d" % tuple(h))
    print("   per game frame (ms):  game %.2f + present %.2f = %.2f" % (game, present, game + present))
    untimed = game - sum(timed.values())
    for k, v in sorted(timed.items(), key=lambda x: -x[1]):
        print("     %-8s %7.2f" % (k, v))
    print("     %-8s %7.2f   (recompiled code, GX bridge, rest: see prof_report.py)" % ("untimed", untimed))
    print("   reporter cost %.1f us/frame" % w("pm_us"))
    mem = [(k, w(k, "pm-mem")) for k in ("pipe_wr", "cp_wr", "pe_wr", "vi_wr", "pi_wr", "mi_wr", "dsp_wr", "dsp_rd",
                                         "exi_rw", "cp_rd", "pe_rd", "pi_rd", "vi_rd", "efb_rd", "efb_wr", "io_other",
                                         "vm_rd", "vm_wr", "lc_rd", "lc_wr", "other_rd", "other_wr", "psq_ld_q",
                                         "psq_st_q")]
    print("   slow-path accesses per frame: " + ", ".join("%s %.0f" % kv for kv in sorted(mem, key=lambda x: -x[1]) if kv[1] >= 1))
    gx = [(k, w(k, "pm-gx")) for k in ("draws", "au_draws", "au_merged", "tex_new", "tex_up_kib", "arrays_kib",
                                       "copy_tex", "copy_tex_kpx", "copy_xfb", "copy_xfb_kpx")]
    print("   gx per frame: " + ", ".join("%s %.1f" % kv for kv in gx))
    sk = sum(r.get("pm-gx", {}).get("skipped", 0) for r in rows)
    ub = sum(r.get("pm-gx", {}).get("uber", 0) for r in rows)
    print("   pipelines: %d draws skipped, %d drawn with the ubershader (pipelines still building)" % (sk, ub))
    dk = [(k, mean(rows, k, "pm-dk")) for k in ("presents", "fence", "acquire", "record", "submit", "submits")]
    print("   deko3d per present (ms): " + ", ".join("%s %.2f" % kv for kv in dk))
    sh = sum(r.get("pm-dk", {}).get("shaders", 0) for r in rows)
    shm = sum(r.get("pm-dk", {}).get("shader_ms", 0) for r in rows)
    bw = sum(r.get("pm-dk", {}).get("burst_waits", 0) for r in rows)
    print("   shaders compiled on the console: %d (%.0f ms of uam); GPU burst waits %d" % (sh, shm, bw))
    thr = defaultdict(list)
    for r in rows:
        for k, v in r.get("pm-thr", {}).items():
            if k != "t" and isinstance(v, float):
                thr[k].append(v)
    print("   thread CPU %: " + ", ".join("%s %.0f" % (k, sum(v) / len(v)) for k, v in thr.items()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--level", type=int)
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e12)
    ap.add_argument("--hitches", type=int, default=15)
    a = ap.parse_args()
    build, secs, hitches = parse(a.log)
    print(build)
    if not secs:
        sys.exit("no [pm] lines: this run had PERF LOG off, or the build predates it")
    secs = [r for r in secs if a.t0 <= r["pm"]["t"] <= a.t1]
    by = defaultdict(list)
    for r in secs:
        by[int(r["pm"].get("lvl", -1))].append(r)
    report(secs, "whole run")
    for lvl in sorted(by):
        if a.level is None or lvl == a.level:
            report(by[lvl], "level %d" % lvl)
    if hitches:
        print("\n== worst hitches (ms; game = game thread, present = hand-off)")
        for hch in sorted(hitches, key=lambda x: -x.get("ms", 0))[: a.hitches]:
            print("   " + " ".join("%s=%s" % (k, ("%g" % v) if isinstance(v, float) else v) for k, v in hch.items()))


if __name__ == "__main__":
    main()
