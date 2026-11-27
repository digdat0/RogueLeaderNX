#!/bin/bash
# RL-NX: repeatable "where does the port stand" census.   switch/tools/port-census.sh [disc]
#
# 1. builds the host recompiler (build-switch/host/r2recomp.exe) if zstd is already built,
# 2. regenerates the game code from the disc (only if the gen folder is empty),
# 3. compiles every src/runtime/*.cpp for aarch64-none-elf with Clang, -fsyntax-only, and lists
#    the first error of each file that fails.
#
# The numbers belong in docs/SWITCH_PORT.md section C. Run it after every dependency lands
# (SDL3, Aurora's extern submodules, the posix shim) to see the failure list shrink.
#
# Notes, both learned the hard way:
#   * devkitPro's msys2 GCC must be driven through env.exe with PATH=/opt/devkitpro/msys2/usr/bin:/usr/bin,
#     and needs -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE (it does not define _WIN32).
#   * Clang needs Windows-style -isystem paths from Git Bash, and the C++ dirs BEFORE newlib's C dir
#     (libstdc++'s #include_next <stdlib.h>).
set -u
cd "$(dirname "$0")/../.." || exit 1

DISC="${1:-E:/RogueSquadronII/Disc Image/Star Wars - Rogue Squadron II - Rogue Leader.iso}"
ENVX=/c/devkitPro/msys2/usr/bin/env.exe
CLANG="${RL_NX_CLANG:-/c/Program Files/LLVM/bin/clang++.exe}"
I="C:/devkitPro/devkitA64/aarch64-none-elf/include"

# ---- 1. host recompiler
if [ ! -f build-switch/host/r2recomp.exe ]; then
  if [ ! -f ref/zstd/lib/libzstd-rlnx.a ]; then
    echo "== building zstd (ref/zstd)"
    [ -d ref/zstd ] || git clone --quiet --depth 1 https://github.com/facebook/zstd ref/zstd || exit 1
    ( cd ref/zstd/lib && rm -f *.o libzstd-rlnx.a &&
      "$ENVX" PATH=/opt/devkitpro/msys2/usr/bin:/usr/bin /usr/bin/gcc.exe -O2 -DXXH_NAMESPACE=ZSTD_ \
        -I. -Icommon -Icompress -Idecompress -IdictBuilder \
        -c common/*.c compress/*.c decompress/*.c dictBuilder/*.c &&
      "$ENVX" PATH=/opt/devkitpro/msys2/usr/bin:/usr/bin /usr/bin/ar.exe rcs libzstd-rlnx.a *.o ) || exit 1
  fi
  echo "== building r2recomp"
  mkdir -p build-switch/host
  "$ENVX" PATH=/opt/devkitpro/msys2/usr/bin:/usr/bin /usr/bin/g++.exe -std=c++20 -O2 \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Isrc/common -Iref/zstd/lib \
    src/recompiler/main.cpp src/recompiler/analysis.cpp src/recompiler/emit.cpp src/recompiler/ppc.cpp \
    src/common/disc_image.cpp ref/zstd/lib/libzstd-rlnx.a -o build-switch/host/r2recomp.exe || exit 1
fi

# ---- 2. game code
if [ ! -f build-switch/gen/funcs.h ]; then
  echo "== recompiling the game from: $DISC"
  mkdir -p build-switch/gen
  ./build-switch/host/r2recomp.exe "$DISC" config/GSWE64 build-switch/gen --files 96 || exit 1
fi
echo "== generated game code: $(ls build-switch/gen/code_*.cpp 2>/dev/null | wc -l) files"

# ---- 3. runtime census
echo "== runtime files that compile for aarch64-none-elf (Clang, header check only)"
pass=0; fail=0
for f in src/runtime/*.cpp; do
  if "$CLANG" --target=aarch64-none-elf -mcpu=cortex-a57 -std=c++20 -fsyntax-only -Wno-everything \
      -isystem "$I/c++/16.1.0" -isystem "$I/c++/16.1.0/aarch64-none-elf" -isystem "$I" \
      -Isrc/runtime -Ibuild-switch/gen -Iextern/aurora/include -Iextern/aurora/lib "$f" \
      >/dev/null 2>build-switch/probe/err.txt; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    printf "  %-24s %s\n" "$(basename "$f")" "$(grep -m1 -E 'fatal error|error:' build-switch/probe/err.txt | sed 's/.*error: //' | cut -c1-60)"
  fi
done
echo "== $pass of $((pass+fail)) runtime files compile"
