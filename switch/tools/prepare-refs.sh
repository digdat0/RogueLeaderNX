#!/bin/bash
# RL-NX: fetch and patch the two external trees the Switch build needs (once).
#   switch/tools/prepare-refs.sh
#
#   ref/dawn          encounter/dawn @ 80ee0043 (Null backend + Tint), abseil/jinja/markupsafe/spirv
#                     submodules, switch/patches/dawn-*.patch, then builder/prepare-deko.py
#   ref/SDL-dusklight libsdl-org/SDL release-3.4.10 + switch/patches/sdl-dusklight-*.patch
#
# After this: switch/build-dawn.sh, switch/build-sdl.sh, switch/build.sh.
# Both trees are git-ignored (ref/ is in .gitignore): they are upstream source, not ours.
set -e
cd "$(dirname "$0")/../.."
ROOT="$PWD"   # absolute: $OLDPWD is useless inside a subshell that cd'd twice
mkdir -p ref

# patch(1) from inside the target tree (git apply resolves against the repo root and can silently
# skip a patch). Idempotent: a patch that is already applied is not an error, a missing patch file
# is. $1 = tree, $2 = patch file.
apply_patch() {
  if [ ! -f "$2" ]; then echo "   missing patch: $2"; return 1; fi
  if ( cd "$1" && patch -p1 -N -s --dry-run --no-backup-if-mismatch -i "$ROOT/$2" >/dev/null 2>&1 ); then
    ( cd "$1" && patch -p1 -N -s --no-backup-if-mismatch -i "$ROOT/$2" ) && echo "   applied $(basename "$2")"
  elif ( cd "$1" && patch -p1 -N -s --dry-run -R --no-backup-if-mismatch -i "$ROOT/$2" >/dev/null 2>&1 ); then
    echo "   already applied: $(basename "$2")"
  else
    echo "   CONFLICTS: $(basename "$2")"; return 1
  fi
}

# ---- Dawn -------------------------------------------------------------------------------
if [ ! -d ref/dawn/.git ]; then
  echo "== fetching dawn 80ee0043"
  mkdir -p ref/dawn
  git -C ref/dawn init -q
  git -C ref/dawn remote add origin https://github.com/encounter/dawn 2>/dev/null || true
  git -C ref/dawn fetch -q --depth 1 origin 80ee0043018a51532ea0fa2e77496cc66634157e
  git -C ref/dawn checkout -q FETCH_HEAD
  git -C ref/dawn submodule update -q --init --depth 1 \
      third_party/abseil-cpp third_party/jinja2 third_party/markupsafe third_party/spirv-headers/src
fi

if [ ! -f ref/dawn/.rlnx-patched ]; then
  echo "== patching dawn"
  # patch(1) with -p1 from inside the tree: git apply resolves paths against the repo root and
  # silently skips patches when invoked from a subdirectory of another repo (SFA-NX, 2026-11-17).
  for p in libnx surface; do apply_patch ref/dawn "switch/patches/dawn-switch-$p.patch"; done
  apply_patch ref/dawn/third_party/abseil-cpp switch/patches/dawn-abseil-switch.patch
  # dawn-switch-renderdoc.patch is malformed upstream: the one line it changes is added by hand.
  RD=ref/dawn/third_party/renderdoc/renderdoc/api/app/renderdoc_app.h
  if [ -f "$RD" ] && ! grep -q "defined(__SWITCH__)" "$RD"; then
    sed -i 's/^#if defined(__linux__)$/#if defined(__linux__) || defined(__SWITCH__)/' "$RD"
  fi
  py -3 switch/builder/prepare-deko.py --dawn ref/dawn
  # RL-NX: the Null device's 512 MiB accounting cap is a test heuristic, not a device property; this
  # port's Dawn device is always Null (deko3d is the real renderer), and a whole game's pipelines and
  # staging buffers pass 512 MiB, which showed up as "WebGPU error 3: Out of memory" mid-boot.
  py -3 switch/tools/patch-dawn-null-memory.py ref/dawn
  touch ref/dawn/.rlnx-patched
fi

# ---- SDL3 -------------------------------------------------------------------------------
if [ ! -d ref/SDL-dusklight/.git ]; then
  echo "== cloning SDL 3.4.10"
  git clone -q --depth 1 -b release-3.4.10 https://github.com/libsdl-org/SDL ref/SDL-dusklight
fi
if [ ! -f ref/SDL-dusklight/.rlnx-patched ]; then
  echo "== patching SDL"
  # SDL's patches are a *series* (melee-nx/sfa-nx build on "switch"), and they are git-format
  # patches whose context does not survive patch(1): git apply with --ignore-space-change does it,
  # which is what SFA-NX's README prescribes. sdl-switch-external-graphics.patch is obsolete:
  # SDL 3.4.10 already has the SDL_SWITCH_EXTERNAL_GRAPHICS option (CMakeLists.txt), so it is gone.
  for p in switch melee-nx sfa-nx sfa-nx-shoulders sfa-nx-rumble; do
    if ( cd ref/SDL-dusklight && git apply --ignore-space-change --recount "$ROOT/switch/patches/sdl-dusklight-$p.patch" ); then
      echo "   applied sdl-dusklight-$p.patch"
    else
      echo "   CONFLICTS: sdl-dusklight-$p.patch"
    fi
  done
  touch ref/SDL-dusklight/.rlnx-patched
fi

echo "== refs ready: ref/dawn ref/SDL-dusklight"
