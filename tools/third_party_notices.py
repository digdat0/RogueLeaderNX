#!/usr/bin/env python3
# Writes THIRD_PARTY_NOTICES.txt for a package: Red Two's own license, then the license of
# everything a build links - Aurora and rcheevos (extern/), and what CMake fetched into
# <build>/_deps (SDL, Dawn, ImGui, FreeType, libpng, zlib, zstd, xxHash, fmt, abseil, ...).
# Prebuilt packages that ship no license file get the text from tools/licenses/.
#   python3 tools/third_party_notices.py <build dir> <output file>
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent.parent
build = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2])

NAMES = ("LICENSE", "LICENSE.txt", "LICENSE.TXT", "LICENSE.md", "LICENSE.MIT", "LICENCE", "COPYING", "COPYING.txt")
# fetched directory (without -src) -> fallback text in tools/licenses/
FALLBACK = {"dawn_prebuilt": "dawn.txt", "sdl3_prebuilt": "sdl.txt", "sqlite3": "sqlite.txt"}
SKIP = {"googletest", "corrosion"}  # build tools, not linked

def texts(directory):
    # the first license file found (zstd's COPYING is its GPLv2 alternative: LICENSE wins)
    for n in NAMES:
        f = directory / n
        if f.is_file():
            yield f.read_text(encoding="utf-8", errors="replace").strip()
            break
    # FreeType's LICENSE.TXT offers a choice; the FreeType License (FTL) is the one taken
    ftl = directory / "docs" / "FTL.TXT"
    if ftl.is_file():
        yield ftl.read_text(encoding="utf-8", errors="replace").strip()

parts = [
    "Red Two\n" + (root / "LICENSE").read_text(encoding="utf-8").strip(),
    "Red Two includes the following software. Their licenses follow.",
]
entries = [("Aurora", root / "extern" / "aurora"), ("rcheevos", root / "extern" / "rcheevos")]
deps = build / "_deps"
if deps.is_dir():
    for d in sorted(deps.glob("*-src")):
        name = d.name[: -len("-src")]
        if name not in SKIP:
            entries.append((name, d))
for name, d in entries:
    found = list(texts(d))
    if not found and name in FALLBACK:
        found = [(root / "tools" / "licenses" / FALLBACK[name]).read_text(encoding="utf-8").strip()]
    if not found:
        print(f"note: no license text for {name} ({d})", file=sys.stderr)
        continue
    for t in found:
        parts.append(f"{'=' * 78}\n{name}\n{'=' * 78}\n{t}")
# Windows: Dawn's Direct3D 12 backend brings the DirectX Shader Compiler DLLs
if (build / "dxcompiler.dll").is_file():
    t = (root / "tools" / "licenses" / "dxc.txt").read_text(encoding="utf-8").strip()
    parts.append(f"{'=' * 78}\nDirectXShaderCompiler\n{'=' * 78}\n{t}")
out.write_text("\n\n".join(parts) + "\n", encoding="utf-8", newline="\n")
print(f"{out}: {len(entries)} components")
