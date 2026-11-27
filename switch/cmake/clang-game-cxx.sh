#!/bin/sh
# RL-NX: compile the recompiled game code and the guest core with Clang, on AArch64.
#
# CMake calls "clang-game-cxx.sh <devkitA64 g++> <args>"; the args are the ones it would have given
# g++, including the newlib include paths in g++'s order (C++ first — libstdc++'s #include_next
# <stdlib.h> needs that, see docs/SWITCH_PORT.md §C.7). Why Clang at all: devkitA64 GCC 16 rejects
# __builtin_rotateleft32 and [[clang::always_inline]], and silently ignores
# __attribute__((naked)) — which would break rt_setjmp and every guest thread with it.
#
# The paths are Windows-style on purpose: this script runs under devkitPro's msys2, where a Git
# Bash /c/... path does not resolve.
shift   # drop the gcc path
CLANG="${RL_NX_CLANGXX:-C:/Program Files/LLVM/bin/clang++.exe}"
[ -x "$CLANG" ] || { echo "clang-game-cxx: no Clang at $CLANG (set RL_NX_CLANGXX)" >&2; exit 1; }
set -- "$@" END
for a in "$@"; do
  shift
  case "$a" in
    END) break ;;
    -mtp=soft) ;;   # devkitPro GCC option (soft TLS); the guest code has no thread-locals
    *) set -- "$@" "$a" ;;
  esac
done
# Clang does not know devkitA64's implicitly-found headers the way the g++ it replaced does, so the
# C++ and C runtime dirs are passed explicitly. Order is load-bearing: libstdc++'s
# #include_next <stdlib.h> needs the C++ dirs before newlib's (docs/SWITCH_PORT.md §C.7).
# Clang is a Windows binary, so this is a Windows path even when CMake runs under devkitPro's msys2
# (where DEVKITPRO is /opt/devkitpro). Override with RL_NX_SYSROOT for a non-default install.
SYS="${RL_NX_SYSROOT:-C:/devkitPro/devkitA64/aarch64-none-elf/include}"
exec "$CLANG" --target=aarch64-none-elf -mcpu=cortex-a57 -Wno-everything \
  -isystem "$SYS/c++/16.1.0" -isystem "$SYS/c++/16.1.0/aarch64-none-elf" -isystem "$SYS" \
  "$@"
