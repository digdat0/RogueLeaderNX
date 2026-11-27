# RL-NX (code audit, 2026-11-26): where the recompiled game code's time goes, per host instruction.
#   py -3 switch/tools/code_audit.py <log folder> <that build's elf> <that build's gen dir> <top N> <out.txt>
# Charges main-thread profile samples of the N hottest gen functions to buckets (guest memory access, VM TLB
# test, ABI boundary, host spills, CR flags, ...). The elf has line tables but no inline chains, so a sample
# inside a ctx.h helper takes the gen line of the nearest instruction that has one.
import sys, os, re, subprocess, collections, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prof_report as pr

LOGDIR, ELF, GEN, TOPN = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
OUT = sys.argv[5]
profs = sorted(glob.glob(LOGDIR + "/profile-*.txt"))
head, rows = pr.merge(profs)
addrs, names, anchor = pr.load_symbols(ELF)
bias = (int(head["anchor"], 16) if isinstance(head["anchor"], str) else head["anchor"]) - anchor
frames = sum(1 for _ in [0])  # filled below
frames = head.get("frames") or 0
try:
    frames = int(frames)
except Exception:
    frames = 0

# function ranges with sizes
nm = subprocess.run([pr.tool("nm"), "-S", "-C", ELF], capture_output=True, text=True).stdout
funcs = []
for ln in nm.splitlines():
    p = ln.split(None, 3)
    if len(p) == 4 and p[2] in "tTwW":
        funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
funcs.sort()
starts = [f[0] for f in funcs]
import bisect
def fn_of(a):
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and funcs[i][0] <= a < funcs[i][0] + max(funcs[i][1], 4):
        return funcs[i]
    return None

pcs = collections.Counter()
total0 = 0
for role, count, pc, lr in rows:
    if role != 0:
        continue
    total0 += count
    pcs[pc - bias] += count
byfn = collections.Counter()
for a, c in pcs.items():
    f = fn_of(a)
    if f:
        byfn[f] += c
genre = re.compile(r"^rt::f_[0-9A-F]{8}(_orig)?\(")
genfns = [(f, c) for f, c in byfn.most_common() if genre.match(f[2])]
gentotal = sum(c for _, c in genfns)
top = genfns[:TOPN]
topsum = sum(c for _, c in top)

# gen source: index of every code_NNN.cpp line -> (guest addr, opcode word) of the guest instruction it implements
srcs = {}
def load_src(path):
    base = os.path.basename(path)
    if base in srcs:
        return srcs[base]
    p = os.path.join(GEN, base)
    lines = open(p, encoding="utf-8", errors="replace").read().split("\n")
    srcs[base] = lines
    return lines
cmt = re.compile(r"^\s*// ([0-9A-F]{8}): ([0-9A-F]{8})")
def guest_of(base, lineno):
    lines = load_src(base)
    i = lineno - 1
    txt = lines[i] if i < len(lines) else ""
    j = i
    while j >= 0:
        m = cmt.match(lines[j])
        if m:
            return m.group(1), int(m.group(2), 16), txt
        if lines[j].startswith("void f_") or lines[j].startswith("}"):
            return None, None, txt
        j -= 1
    return None, None, txt

def ppc_class(w):
    op = w >> 26
    xo = (w >> 1) & 0x3FF
    if op in (32, 33, 34, 35, 40, 41, 42, 43): return "load int"
    if op in (36, 37, 38, 39, 44, 45, 46, 47): return "store int"
    if op in (46, 47): return "lmw/stmw"
    if op in (48, 49, 50, 51): return "load fp"
    if op in (52, 53, 54, 55): return "store fp"
    if op in (56, 57, 60, 61): return "psq ld/st"
    if op == 4:
        x = (w >> 1) & 0x1F
        if (w >> 1) & 0x3F in (6, 7, 38, 39): return "psq ld/st"
        return "paired-single math"
    if op in (59, 63):
        x5 = (w >> 1) & 0x1F
        if op == 63 and xo in (0, 32): return "fp compare"
        if op == 63 and xo in (12, 14, 15, 72, 40, 264, 136, 583, 711, 70, 38, 134, 64): return "fp move/convert"
        return "fp math"
    if op in (16, 18, 19):
        if op == 19 and xo in (16, 528): return "branch (blr/bctr)"
        if op == 19: return "cr logic"
        if op == 18 and (w & 1): return "call (bl)"
        if op == 16: return "branch cond"
        return "branch"
    if op in (10, 11): return "compare"
    if op in (20, 21, 23): return "rotate/mask"
    if op in (7,): return "mul/div"
    if op in (14, 15, 12, 13, 8, 24, 25, 26, 27, 28, 29): return "int alu imm"
    if op == 31:
        if xo in (0, 32): return "compare"
        if xo in (235, 11, 75, 459, 491, 233, 203, 139, 107): return "mul/div"
        if xo in (339, 467, 19, 83, 146, 371, 144): return "spr/cr/msr move"
        if xo in (23, 55, 87, 119, 279, 311, 343, 375, 534, 790, 20, 535, 567, 599, 631): return "load int indexed"
        if xo in (151, 183, 215, 247, 407, 439, 662, 918, 150, 663, 695, 727, 759, 983): return "store int indexed"
        if xo in (86, 54, 278, 246, 1014, 470, 982, 598): return "cache/sync"
        return "int alu reg"
    return "other op%d" % op

# objdump for the instructions, addr2line -i for the inline chain (innermost first) of sampled ones
def disasm(lo, hi):
    r = subprocess.run([pr.tool("objdump"), "-d", "--no-show-raw-insn",
                        "--start-address=0x%x" % lo, "--stop-address=0x%x" % hi, ELF],
                       capture_output=True, text=True).stdout
    out = []
    for ln in r.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", ln)
        if m:
            out.append([int(m.group(1), 16), m.group(2), m.group(3), []])
    want = out
    if want:
        r = subprocess.run([pr.tool("addr2line"), "-a", "-i", "-e", ELF],
                           input="\n".join("0x%x" % x[0] for x in want) + "\n",
                           capture_output=True, text=True).stdout.splitlines()
        cur = None; k = -1
        for ln in r:
            if ln.startswith("0x"):
                k += 1; cur = want[k][3]
            else:
                m = re.match(r"^(.*):(\d+)", ln.replace("\\", "/"))
                if m: cur.append((m.group(1), int(m.group(2))))
    return [tuple(x) for x in out]

by_guest_class = collections.Counter()
by_region = collections.Counter()
by_helper = collections.Counter()
by_host = collections.Counter()
by_host_cls = collections.Counter()
host_ins_total = 0
guest_ins_total = 0
per_fn = []
def host_cls(mn, ops):
    if mn in ("rev", "rev16", "rev32"): return "byte swap (rev)"
    if mn.startswith("fcvt") or mn in ("scvtf", "ucvtf"): return "fp convert"
    if mn.startswith("ldr") or mn.startswith("ldp") or mn.startswith("ldur") or mn.startswith("ld1"):
        return "load (ctx/stack)" if re.search(r"\[x(19|20|21|0|sp)\b", ops) and "uxtw" not in ops else "load"
    if mn.startswith("str") or mn.startswith("stp") or mn.startswith("stur") or mn.startswith("st1"):
        return "store (ctx/stack)" if re.search(r"\[x(19|20|21|0|sp)\b", ops) and "uxtw" not in ops else "store"
    if mn in ("b", "bl", "blr", "br", "ret") or mn.startswith("b.") or mn in ("cbz", "cbnz", "tbz", "tbnz"): return "branch"
    if mn in ("cmp", "cmn", "tst", "ccmp", "fcmp", "fcmpe", "csel", "cset", "csinc", "csneg", "csinv"): return "compare/select"
    if mn.startswith("f"): return "fp math/move"
    return "int alu"

ctxh = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "runtime", "ctx.h"), encoding="utf-8").read().split("\n")
def helper_fn(fname, line):
    if not fname.endswith("ctx.h"):
        return os.path.basename(fname)
    for j in range(line - 1, -1, -1):
        m = re.match(r"^(?:inline|static inline|RT_INLINE)[^(]*?\b(\w+)\(", ctxh[j])
        if m:
            return m.group(1)
    return "ctx.h?"
by_bucket = collections.Counter()
by_memop = collections.Counter()
by_membase = collections.Counter()
by_helperfn = collections.Counter()
def is_ld(mn): return mn.startswith(("ldr", "ldp", "ldur", "ld1"))
def is_st(mn): return mn.startswith(("str", "stp", "stur", "st1"))
GUESTMEM = {"ld32be", "st32be", "rd8", "rd16", "rd32", "rd64", "wr8", "wr16", "wr32", "wr64", "rd8S", "rd16S",
            "rd32S", "rd64S", "wr8S", "wr16S", "wr32S", "wr64S", "rd8V", "rd16V", "rd32V", "rd64V", "wr8V", "wr16V",
            "wr32V", "wr64V", "inArena", "inArenaN", "arenaPtrM", "stackPtr", "memBase", "rd16r", "rd32r", "wr32r",
            "wr16r", "rd32rS", "wr32rS", "rd16rS", "wr16rS"}
for (lo, size, name), cnt in top:
    ins = disasm(lo, lo + size)
    host_ins_total += len(ins)
    genl = []
    for a, mn, ops, locs in ins:
        gl = None
        for f, l in locs:
            if re.search(r"code_\d+\.cpp$", f):
                gl = (os.path.basename(f), l)
        genl.append(gl)
    # nearest instruction with a gen line, for those whose only line is a helper's
    near = list(genl)
    for i in range(len(ins)):
        if near[i] is None:
            for d in range(1, 12):
                for k in (i - d, i + d):
                    if 0 <= k < len(ins) and genl[k]:
                        near[i] = genl[k]; break
                if near[i]: break
    # guest instruction count from the source comments of this function
    first = next((g for g in genl if g), None)
    nguest = 0
    if first:
        lines = load_src(first[0])
        j = first[1] - 1
        while j >= 0 and not lines[j].startswith("void f_"):
            j -= 1
        j += 1
        while j < len(lines) and not lines[j].startswith("void f_"):
            if cmt.match(lines[j]): nguest += 1
            j += 1
    guest_ins_total += nguest
    seen_body = False
    for i, (a, mn, ops, locs) in enumerate(ins):
        c = pcs.get(a, 0)
        if not c:
            continue
        hf = helper_fn(*locs[0]) if locs and not re.search(r"code_\d+\.cpp$", locs[0][0]) else None
        g = None
        region = "?"
        if near[i]:
            gaddr, w, txt = guest_of(*near[i])
            if gaddr:
                region = "guest code"; g = ppc_class(w)
                if re.search(r"\bf_[0-9A-F]{8}\(c\)|dispatch\(", txt): region = "call site marshal"
            else:
                region = "entry/exit"
        by_region[region] += c
        if g: by_guest_class[g] += c
        by_helper["%s:%d" % (os.path.basename(locs[0][0]), locs[0][1]) if hf else "(gen line itself)"] += c
        by_helperfn[hf or "(gen line itself)"] += c
        by_host[mn] += c
        by_host_cls[host_cls(mn, ops)] += c
        spill = (is_ld(mn) or is_st(mn)) and re.search(r"\[(sp|x29)\b", ops)
        if region in ("entry/exit", "call site marshal"): bk = "ABI boundary (ctx load/store at entry, exit, calls)"
        elif spill: bk = "host register spill/reload (sp/x29)"
        elif hf in ("vmHitR", "vmHitW"): bk = "VM TLB test (V forms)"
        elif hf in GUESTMEM or (hf and hf.startswith("memcpy")): bk = "guest memory access (test + load/store + swap)"
        elif hf and hf.startswith("cr"): bk = "CR flag compute"
        elif hf and ("psq" in hf.lower() or "quant" in hf.lower()): bk = "paired-single quantise"
        elif hf: bk = "other helper: " + hf
        elif g and g.startswith(("load", "store", "psq")): bk = "guest memory access (test + load/store + swap)"
        elif g and g.startswith("call"): bk = "ABI boundary (ctx load/store at entry, exit, calls)"
        elif g and g.startswith("branch"): bk = "guest branches"
        elif g and g.startswith("compare"): bk = "CR flag compute"
        elif g: bk = "guest ALU/FP work"
        else: bk = "unattributed"
        by_bucket[bk] += c
        # RL-NX (code audit): split the memory buckets by host op (the access itself vs its checks) and by guest base
        if bk.startswith(("guest memory", "VM TLB")):
            by_memop["load/store itself" if (is_ld(mn) or is_st(mn)) else "check/address math (" + host_cls(mn, ops) + ")"] += c
            if near[i] and g:
                ra = (w >> 16) & 31
                kind = {1: "r1 stack", 2: "r2 small data (read-only)", 13: "r13 small data"}.get(ra)
                if kind is None:
                    lines_ = load_src(near[i][0])
                    k0 = near[i][1] - 1
                    kind = "other pointer"
                    for k in range(k0 - 1, max(0, k0 - 16), -1):
                        m2 = re.match(r"^\s*r%d = (0x[0-9A-F]+)u;" % ra, lines_[k])
                        if m2:
                            kind = "constant base (lis) 0x%02X..." % (int(m2.group(1), 16) >> 24); break
                        if re.match(r"^\s*r%d = " % ra, lines_[k]) or re.match(r"^void f_", lines_[k]):
                            break
                by_membase[kind] += c
    per_fn.append((name, cnt, len(ins), nguest))

with open(OUT, "w", encoding="utf-8") as o:
    P = lambda *a: print(*a, file=o)
    P("main-thread samples %d, gen %d (%.1f%%), top %d gen functions %d (%.1f%% of gen)" %
      (total0, gentotal, 100 * gentotal / total0, TOPN, topsum, 100 * topsum / gentotal))
    P("host instructions %d for %d guest instructions in the top %d: %.2f host per guest (static)" %
      (host_ins_total, guest_ins_total, TOPN, host_ins_total / max(1, guest_ins_total)))
    def tab(title, ctr):
        P("\n" + title)
        s = sum(ctr.values())
        for k, v in ctr.most_common(30):
            P("  %6d  %5.1f%%  %s" % (v, 100 * v / s, k))
    tab("BUCKETS", by_bucket)
    tab("Memory buckets by host op", by_memop)
    tab("Memory buckets by guest base register", by_membase)
    tab("By helper function", by_helperfn)
    tab("By region of the gen function (samples of the top functions)", by_region)
    tab("By guest instruction class", by_guest_class)
    tab("By host instruction class", by_host_cls)
    tab("By host mnemonic", by_host)
    tab("By innermost inline helper (file:line)", by_helper)
    P("\nPer function: samples, host ins, guest ins, ratio")
    for name, cnt, h, g in per_fn:
        P("  %6d  %6d  %5d  %5.1f  %s" % (cnt, h, g, h / max(1, g), name))
print("done", OUT)
