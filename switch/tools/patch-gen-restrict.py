# RL-NX: mark each recompiled function's context as not aliased (`Ctx& __restrict c`).
#
#   py -3 -I switch/tools/patch-gen-restrict.py build-switch/gen [--undo]
#
# Why: every guest store is a memcpy into guest RAM through a byte pointer, and a byte pointer may
# alias anything. So after each store Clang reloaded the guest registers (c.r[], c.f[]) from memory
# and stored them back eagerly. In the hardware profile (build 08:56) the recompiled code was 60 ms of a
# 97 ms frame, and its machine code spends most of each store on those reloads. Guest RAM never overlaps
# a Ctx (Ctx lives in host static storage: threads.cpp s_fibers), so the hint is true for every access
# made in the function itself.
#
# Calls stay correct: every generated function passes c to traceEnter() on entry, so c counts as
# captured, and Clang still assumes any out-of-line call (vmMiss and the game's page-fault handler,
# ioRead/ioWrite, safepoint) may read or write it. Dirty registers are stored before those calls, on
# their cold paths. The one function that keeps a setjmp buffer (JmpBuf jb, OSSaveContext) is left
# alone: OSLoadContext rewrites its context before the second return.
#
# Idempotent; a file is rewritten only if it changes, so an unchanged gen/ does not rebuild.
import re
import sys
from pathlib import Path

SIG = re.compile(r"^void (f_[0-9A-F]{8})\(Ctx& c\) \{\n(    if \(g_trace\)[^\n]*\n)(?!    JmpBuf jb;)", re.M)
RSIG = re.compile(r"^void (f_[0-9A-F]{8})\(Ctx& __restrict c\) \{\n", re.M)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    gen = Path(sys.argv[1])
    undo = "--undo" in sys.argv[2:]
    files = sorted(gen.glob("code_*.cpp"))
    if not files:
        print(f"patch-gen-restrict: no code_*.cpp in {gen}")
        return 1
    changed = marked = 0
    for p in files:
        text = p.read_text(encoding="utf-8", newline="")
        if undo:
            new = RSIG.sub(r"void \1(Ctx& c) {\n", text)
        else:
            new = SIG.sub(r"void \1(Ctx& __restrict c) {\n\2", text)
        marked += len(RSIG.findall(new))
        if new != text:
            p.write_text(new, encoding="utf-8", newline="")
            changed += 1
    print(f"patch-gen-restrict: {marked} functions marked, {changed} of {len(files)} files rewritten"
          + (" (undo)" if undo else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
