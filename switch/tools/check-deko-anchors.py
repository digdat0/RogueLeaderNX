"""RL-NX: report every integrate-deko.py seam anchor that this Aurora pin no longer has.

builder/integrate-deko.py stops at the first missing anchor, which tells you one line of a much
longer story when the Aurora pin moves. This runs the same edits in report-only mode: it replaces the
script's `edit()` with a recorder and lists what is present, already applied, or missing.

    py -3 switch/tools/check-deko-anchors.py [aurora-tree]
"""
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "switch/builder/integrate-deko.py"
AURORA = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "extern/aurora"

spec = importlib.util.spec_from_file_location("integrate_deko", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

present, applied, missing = [], [], []


def record(root, name, old, new, count=1, optional=False):
    text = (root / name).read_text(encoding="utf-8")
    first = old.splitlines()[0][:88] if old else ""
    if new in text:
        applied.append((name, first))
    elif old and old in text:
        present.append((name, first))
    else:
        missing.append((name, first, optional))


mod.edit = record
try:
    mod.apply(AURORA)
except SystemExit as exc:                      # a helper stopped the run; keep what we have
    print(f"(apply stopped: {exc})")
except Exception as exc:                      # a helper read a file this pin no longer has
    print(f"(apply raised: {type(exc).__name__}: {exc})")

for label, rows in (("MISSING", missing), ("PRESENT", present), ("ALREADY APPLIED", applied)):
    print(f"\n== {label}: {len(rows)}")
    for name, first, *opt in rows:
        tag = " [optional]" if opt and opt[0] else ""
        print(f"  {name}: {first}{tag}")
print(f"\nsummary: {len(missing)} missing, {len(present)} present, {len(applied)} already applied")
