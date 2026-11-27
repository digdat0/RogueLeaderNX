"""RL-NX: adapt the staged Aurora to the Dawn pin this port builds.

This Aurora (f77492f) is written against a Dawn newer than 80ee0043: there,
Surface::Present() returns wgpu::ConvertibleStatus, a bool-convertible wrapper around Status. Our
Dawn returns wgpu::Status, so exactly two lines differ — the declaration and the truth test.

Usage:  py -3 -I switch/tools/patch-aurora-dawn-status.py build-switch/aurora-patched

Applied by string match rather than by `patch(1)`: the vendored Aurora tree uses CRLF line endings
and GNU patch refuses its hunks ("Hunk #1 FAILED ... different line endings"), while matching the
two lines is exact and idempotent. Exits 0 when already applied.
"""
import pathlib
import sys

EDITS = [
    ("  wgpu::ConvertibleStatus status = wgpu::Status::Error;",
     "  wgpu::Status status = wgpu::Status::Error;"),
    ("  if (status) {",
     "  if (status == wgpu::Status::Success) {"),
]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1])
        return 2
    path = pathlib.Path(sys.argv[1]) / "lib/gfx/present_target.cpp"
    if not path.is_file():
        print(f"missing {path}")
        return 1
    text = path.read_bytes().decode("utf-8")
    changed = False
    for old, new in EDITS:
        if new in text and old not in text:
            continue                              # already applied
        if text.count(old) != 1:
            print(f"{path}: expected exactly one {old!r}, found {text.count(old)}")
            return 1
        text = text.replace(old, new)
        changed = True
    if changed:
        path.write_bytes(text.encode("utf-8"))
        print(f"patched {path.name} (Dawn Status)")
    else:
        print(f"{path.name}: already patched")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
