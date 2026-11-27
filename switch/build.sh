#!/bin/bash
# Build RL-NX.nro (Rogue Leader on Switch).   switch/build.sh [jobs]
# Prerequisites (once): switch/tools/prepare-refs.sh, switch/build-dawn.sh and switch/build-sdl.sh
# (see switch/README.md).
# Each run re-stages Aurora (extern/aurora + the platform patches that apply to it + the deko3d
# seams) into build-switch/aurora-deko, then configures and builds build-switch/.
# Output: build-switch/RL-NX.nro, copied with its .elf to debug/RL-NX-HHMM.elf.
set -e
cd "$(dirname "$0")/.."
JOBS="${1:-8}"
# devkitPro install folder as Git Bash sees it (installer default C:\devkitPro). Set RL_NX_DEVKITPRO for
# another location, e.g. RL_NX_DEVKITPRO=/d/devkitPro switch/build.sh. No local paths are hard-coded here.
DKP="${RL_NX_DEVKITPRO:-/c/devkitPro}"
# RL-NX: RL_NX_DEKO=1 builds with the native deko3d renderer, which needs switch/deko ported to this
# Aurora's gfx::present/window seams. Until then the default is the headless bring-up build: Aurora
# runs its Dawn Null backend with no picture (docs/SWITCH_PORT.md §E step 5).
RL_NX_DEKO="${RL_NX_DEKO:-0}"

# ---- stage Aurora (both builds; only the deko3d seam edits are conditional)
STAMP=build-switch/aurora.stamp
mkdir -p build-switch
# RL-NX: extern/aurora is a *vendored* tree (encounter/aurora f77492f + Red Two's 33 patches, see
# docs/SWITCH_PORT.md §G), not a submodule, so the stamp hashes its content. Hashing the content
# also covers edits made to the tree by hand, which the submodule's commit id did not (SFA-NX,
# 2026-11-17: two test builds were identical to the one before).
AURORA_TREE="$(find extern/aurora -type f -print0 | sort -z | xargs -0 sha1sum | sha1sum | cut -c1-12)"
INPUTS="$(cat switch/patches/aurora-switch-io-atomic.patch switch/tools/patch-aurora-dawn-status.py \
           switch/tools/patch-aurora-switch-input.py \
           switch/builder/integrate-deko.py | sha1sum | cut -c1-12)-$AURORA_TREE-$RL_NX_DEKO"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$INPUTS" ] || [ ! -d build-switch/aurora-deko ]; then
  echo "== staging Aurora"
  rm -rf build-switch/aurora-patched build-switch/aurora-deko
  cp -r extern/aurora build-switch/aurora-patched
  # Applied with patch(1): git apply inside a checkout resolves paths against the repo root and can
  # silently skip a patch. Only the platform patches that this Aurora still takes are applied; the
  # rest are Melee-NX's, written for the older Aurora SFA-NX pins, and do not apply here:
  #   io-atomic   atomic file I/O on Horizon (needed)
  #   card        Aurora's memory card: off in this port (AURORA_ENABLE_CARD OFF, the runtime owns
  #               CARD through EXI), and it does not apply on top of Red Two's patch series
  #   fcvt        float->int floor conversion (SFA's GCC fcvtzu clamping fix); it edits
  #               lib/gx/shader.cpp, which Red Two's ubershader patch rewrote, so it cannot apply.
  #               Red Two's generated code already casts through s32 (see references/subsystems),
  #               and the guest core is built with Clang, which wraps like the GameCube.
  (cd build-switch/aurora-patched && for p in io-atomic; do
    patch -p1 -N -s --no-backup-if-mismatch -i "../../switch/patches/aurora-switch-$p.patch"
  done)
  # RL-NX: our Dawn pin (80ee0043) has Surface::Present() returning wgpu::Status, this Aurora expects
  # the newer ConvertibleStatus. Applied by string match, not patch(1): the tree is CRLF and GNU
  # patch refuses its hunks. See switch/tools/patch-aurora-dawn-status.py.
  py -3 -I switch/tools/patch-aurora-dawn-status.py build-switch/aurora-patched
  # RL-NX: Aurora starts the controllers only with a non-headless surface; the console needs them
  # too (no SDL joystick subsystem = no pad, the title screen ignored every button).
  py -3 -I switch/tools/patch-aurora-switch-input.py build-switch/aurora-patched
  echo "$INPUTS" > "$STAMP"
fi   # the stamp check
# The deko3d seam rewrite is separate from the staging above: it is what RL_NX_DEKO adds, and a
# headless build must not need it (docs/SWITCH_PORT.md §C.10).
if [ "$RL_NX_DEKO" != 0 ] && [ ! -d build-switch/aurora-deko ]; then
  py -3 -I switch/builder/integrate-deko.py --src build-switch/aurora-patched --dst build-switch/aurora-deko
fi
AURORA_SRC=build-switch/aurora-patched
[ "$RL_NX_DEKO" != 0 ] && AURORA_SRC=build-switch/aurora-deko

# ---- configure + build through devkitPro's msys2 (env vars don't cross from Git Bash; gcc needs a
# Windows temp dir)
# RL_NX_DEKO_DEBUG=1 links libdeko3dd, which checks every deko3d call and reports a failing one
# instead of aborting - the only way to see why dkSwapchainCreate (or any other call) refused.
mkdir -p build-switch/tmp
T="$(cygpath -m "$PWD/build-switch/tmp")"
RUN=("$DKP/msys2/usr/bin/env.exe"
     "PATH=/opt/devkitpro/msys2/usr/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/bin"
     DEVKITPRO=/opt/devkitpro TMPDIR="$T" TMP="$T" TEMP="$T")
"${RUN[@]}" cmake -S switch -B build-switch -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -u "$PWD")/switch/cmake/SwitchFoxhollow.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -UCMAKE_NINJA_FORCE_RESPONSE_FILE \
    -DRL_NX_DEKO=$([ "$RL_NX_DEKO" = 0 ] && echo OFF || echo ON) \
    -DMELEE_NX_DEKO_DEBUG=$([ "${RL_NX_DEKO_DEBUG:-0}" = 0 ] && echo OFF || echo ON) \
    -DRL_NX_WATCH_BUILD=$([ "${RL_NX_WATCH_BUILD:-0}" = 0 ] && echo OFF || echo ON) \
    -DRL_NX_ICOUNT=$([ "${RL_NX_ICOUNT:-0}" = 0 ] && echo OFF || echo ON) \
    -DRL_NX_ORACLE=$([ "${RL_NX_ORACLE:-0}" = 0 ] && echo OFF || echo ON) \
    -DRL_NX_FASTMEM=$([ "${RL_NX_FASTMEM:-0}" = 0 ] && echo OFF || echo ON) \
    -DRL_NX_GAME_OPT="${RL_NX_GAME_OPT:--O3}" \
    -DRL_NX_GAME_PIC="${RL_NX_GAME_PIC:-}" \
    -DRL_NX_GAME_INLINE="${RL_NX_GAME_INLINE:-}" \
    -DMELEE_AURORA_SOURCE="$(cygpath -u "$PWD")/$AURORA_SRC"
touch switch/src/main_switch.cpp # boot.txt / stdout.log build stamp = this build
# RL-NX: recompiled functions take their context as `Ctx& __restrict c`, so guest stores no longer force
# every guest register to be reloaded (hardware 08:56: the game code was 60 ms of a 97 ms frame). Idempotent;
# only changed files are rewritten. RL_NX_NO_RESTRICT=1 takes it out again (for an A/B or a suspected
# miscompile).
if [ "${RL_NX_NO_RESTRICT:-0}" = 0 ]; then
  py -3 -I switch/tools/patch-gen-restrict.py build-switch/gen
else
  py -3 -I switch/tools/patch-gen-restrict.py build-switch/gen --undo
fi
# RL-NX (9.39): switch/tools/hot-unity.py moves the profiled-hot guest functions into gen/code_hot.cpp and cuts
# them from their files. A regenerated gen/ puts them back, and a stale code_hot.cpp would then define them a
# second time - the link allows multiple definitions, so the wrong copy could silently win. Drop the stale file
# instead: that build has no cluster (the same as 9.38) and says so.
if [ -f build-switch/gen/code_hot.cpp ]; then
  FIRST="$(grep -m1 -oE '^void f_[0-9A-Fa-f_]+' build-switch/gen/code_hot.cpp | awk '{print $2}')"
  if [ -n "$FIRST" ] && grep -lE "^void ${FIRST}\(" build-switch/gen/code_[0-9]*.cpp >/dev/null 2>&1; then
    echo "== hot-unity: gen/ was regenerated (${FIRST} is back in its own file); dropping the stale code_hot.cpp"
    echo "   re-run: py -3 switch/tools/hot-unity.py <profile> <that build's elf>"
    rm -f build-switch/gen/code_hot.cpp
  fi
fi
"${RUN[@]}" cmake --build build-switch --parallel "$JOBS" ${KEEP_GOING:+-- -k 0}

# The game links whole-archive with --allow-multiple-definition, so a game function named like a
# libnx/SDL one silently replaces it (padUpdate did: crash before the game started). Fail instead.
# The game's own float trig (acosf, asinf, atanf, atan2f) replaces libm's on desktop too: allowed.
NM="$DKP/devkitA64/bin/aarch64-none-elf-nm.exe"
syms() { "$NM" -g --defined-only "$@" 2>/dev/null | awk 'NF==3{print $3}' | sort -u; }
CLASH="$(comm -12 <(syms build-switch/libgame.a build-switch/libruntime.a) \
                  <(syms "$DKP"/libnx/lib/libnx.a build-switch-sdl/libSDL3.a) \
         | grep -vxE 'acosf|asinf|atanf|atan2f' || true)"
if [ -n "$CLASH" ]; then
  echo "error: game symbols replace libnx/SDL ones (rename them in CMakeLists.txt):"; echo "$CLASH"; exit 1
fi

# The heap is dlmalloc (switch/src/malloc_switch.c). If any newlib malloc object still links in (some
# entry point malloc_switch.c does not define), --allow-multiple-definition would hide the clash and two
# allocators would share one heap: fail instead. __malloc_av_ is newlib's bin array (libc_a-mallocr.o).
if "$NM" build-switch/RL-NX.elf | grep -qw "__malloc_av_"; then
  echo "error: newlib's malloc is linked next to dlmalloc (switch/src/malloc_switch.c misses an entry point)"
  exit 1
fi

mkdir -p debug build-switch/sdcard/switch/RL-NX
cp build-switch/RL-NX.elf "debug/RL-NX-$(date +%H%M).elf"
cp build-switch/RL-NX.nro build-switch/sdcard/switch/RL-NX/
rm -f build-switch/sdcard/switch/RL-NX/initial_dksh_cache.bin build-switch/sdcard/switch/RL-NX/initial_pipeline_cache.db
ls -la build-switch/sdcard/switch/RL-NX/RL-NX.nro
