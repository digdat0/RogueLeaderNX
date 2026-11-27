# RL-NX: Rogue Leader on Nintendo Switch.  Built by switch/build.sh (see switch/README.md).
#
# Order of bring-up (docs/SWITCH_PORT.md §E): this file starts with the HEADLESS milestone —
# the guest CPU, guest memory (the 256 MiB arena), disc, audio and input alive, Aurora running its
# Dawn Null backend with no picture. The deko3d renderer is then switched on with -DRL_NX_DEKO=ON,
# which needs switch/deko ported to this Aurora's gfx::present/window seams (see §D.3).
cmake_minimum_required(VERSION 3.25)
project(rl-nx LANGUAGES C CXX ASM VERSION 0.1.4)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(RL_ROOT "${_repo}")

# RL-NX: RL_NX_WATCH_BUILD=1 (switch/build.sh forwards it; OFF on every ordinary build, so the watch is
# built out again) compiles the guest store watch into the Switch build. ctx.h RT_WATCHING is false there by
# default - every guest store would carry a load and a branch - which also disables R2_WATCH and
# R2_WATCH_VALUE, the only way to name the code that writes a guest block.
if(RL_NX_WATCH_BUILD)
  add_compile_definitions(RL_NX_WATCH)
  message(STATUS "RL-NX: guest store watch compiled in (RL_NX_WATCH_BUILD)")
endif()

# RL-NX (PERF_PLAN phase 0): the diagnostic compile-in switches that have to reach BOTH archives - the game
# carries the hook, the runtime reports on it - so they are set here, before any target exists, where
# add_compile_definitions covers everything in this directory.
#   RL_NX_ICOUNT=1 - one guest-instruction count per basic block, for ns per guest instruction (meter.cpp).
#   RL_NX_ORACLE=1 - one oracle probe per guest function entry (oracle.cpp compares it with the interpreter).
if(RL_NX_ICOUNT)
  add_compile_definitions(RL_NX_ICOUNT)
  message(STATUS "RL-NX: guest instruction metering compiled in (RL_NX_ICOUNT)")
endif()
if(RL_NX_ORACLE)
  add_compile_definitions(RL_NX_ORACLE)
  message(STATUS "RL-NX: oracle test probe hook compiled in (RL_NX_ORACLE)")
endif()
# RL-NX (Stage 11): RL_NX_FASTMEM=1 - the fastmem NRO: the guest window and its fault path (switch/src/fastmem_switch.cpp,
# fastmem_entry.S). Build it with a gen made by R2RECOMP_FASTMEM=1; an ordinary gen runs in it unchanged.
if(RL_NX_FASTMEM)
  add_compile_definitions(RL_NX_FASTMEM)
  message(STATUS "RL-NX: FASTMEM window and fault path compiled in (RL_NX_FASTMEM)")
endif()

# Dawn (Null + Tint), SDL3, Aurora (staged copy) and, optionally, the deko3d renderer.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Graphics.cmake")

find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(nlohmann_json 3 CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(nlohmann_json)
endif()
# zstd: Red Two reads RVZ discs and save states (devkitPro portlibs).
add_library(rl_zstd STATIC IMPORTED GLOBAL)
set_target_properties(rl_zstd PROPERTIES IMPORTED_LOCATION "${DEVKITPRO}/portlibs/switch/lib/libzstd.a")
# RL-NX (Stage 10): libjpeg-turbo (devkitPro portlibs, NEON) decodes the .movie JPEG frames (src/runtime/hle_movie.cpp).
add_library(rl_turbojpeg STATIC IMPORTED GLOBAL)
set_target_properties(rl_turbojpeg PROPERTIES IMPORTED_LOCATION "${DEVKITPRO}/portlibs/switch/lib/libturbojpeg.a")

# --- the game's own code -------------------------------------------------------------------
# Generated from the player's own disc by build-switch/host/r2recomp.exe (never committed).
# Compiled with Clang, as upstream does: devkitA64 GCC rejects the Clang builtins ctx.h relies on
# (__builtin_rotateleft32, [[clang::always_inline]]) and, worse, ignores __attribute__((naked)),
# which would silently break rt_setjmp (docs/SWITCH_PORT.md §C.3).
file(GLOB RL_GEN_SOURCES CONFIGURE_DEPENDS
  "${RL_ROOT}/build-switch/gen/code_*.cpp"
  "${RL_ROOT}/build-switch/gen/table.cpp")
if(NOT RL_GEN_SOURCES)
  message(FATAL_ERROR "No generated game code: run switch/tools/port-census.sh (or build-switch/host/r2recomp.exe) first")
endif()
# RL-NX (9.36): the optimisation level of the guest code and the runtime. -O2 was inherited from the desktop
# build, and the recompiled game *is* the frame (75 ms of a 75 ms frame in the 9.33 profile), so this is the
# one lever that acts on the thread nothing else can help. -O3 also vectorises and unrolls, which can cost
# i-cache on decompiled code - that is what the A/B is for: RL_NX_GAME_OPT=-O2 switch/build.sh 8 rebuilds it
# the old way, and the FPS counter decides.
set(RL_NX_GAME_OPT "-O3" CACHE STRING "optimisation level for the recompiled game and the runtime")
# RL-NX (9.37): -fwrapv is not optional on this code. The guest's `add`/`mullw`/`addi` wrap, and the generated
# C relies on signed overflow wrapping too; without it the optimiser is entitled to assume it cannot happen and
# fold away the overflow checks the game makes (the port skill's rule for recompiled code is
# "-O2 -fwrapv -fno-strict-aliasing"). It was missing from the 9.36 -O3 build.
set(RL_NX_GAME_WRAP "-fwrapv" CACHE STRING "signed-overflow semantics for the recompiled game and the runtime")
# RL-NX (9.38): the toolchain compiles everything -fPIC (SwitchFoxhollow.cmake), which is wrong for this code:
# an NRO is a static ELF with no dlopen, so position independence only buys a GOT load for every global the
# guest code touches - and it touches g_ram on every single load and store (disassembly: adrp + ldr GOT +
# ldr g_ram, three instructions and a dependent load before the address is even formed). The guest's code and
# runtime are built -fno-pic instead; the rest of the tree (Aurora, deko3d, the Switch layer) is untouched, so
# a link problem here can only come from these two archives. RL_NX_GAME_PIC="-fPIC" restores the old code.
# RL-NX (9.38): -fno-pic is not available here - the NRO is linked as a PIE, so a non-PIC archive puts dynamic
# relocations in the read-only segment ("ld.exe: read-only segment has dynamic relocations"). Hidden visibility is
# the same win without that: a hidden global cannot be interposed, so Clang addresses it with adrp+add instead of
# loading its address out of the GOT. That removes two instructions and a dependent load from every guest load and
# store, which reach g_ram through arenaPtr(). RL_NX_GAME_PIC="" restores the old code.
# RL-NX (9.38): neither of those two works here. -fno-pic puts dynamic relocations in the read-only segment of the
# PIE ("ld.exe: read-only segment has dynamic relocations"), and -fvisibility=hidden over two archives does the same
# because their address constants become non-preemptible absolutes. The one symbol worth it is g_ram, so it carries
# __attribute__((visibility("hidden"))) in ctx.h and memory_switch.cpp instead, and this variable stays empty.
set(RL_NX_GAME_PIC "" CACHE STRING "extra position-independence flags for the recompiled game and the runtime")
# RL-NX (9.38): the three cache variables above are strings, and a CMake variable expands as ONE argument —
# "-fno-pic -mcmodel=small" reached clang as a single unknown option. Split them into real argument lists.
# RL-NX (9.39): a calmer way to ask for inlining than raising it over the whole tree by hand. -O3's default
# cost threshold (225) declines almost every guest function (the average one is ~145 guest instructions), so the
# hot cluster switch/tools/hot-unity.py builds would never be inlined into its callers. 2500 gets the hot chains
# inlined (NRO +3.4% over 9.38, no crash, same draws) without unrolling the whole program.
# RL-NX (PERF_PLAN 0b): back to no flag by default. The 2500 threshold was raised for the profile-driven
# hot cluster (9.39), and the cluster measured 0 to -3 % on hardware (9.43) - but the threshold applies to
# every TU, not just the cluster, and it costs 3.3 % of .text (40.16 -> 41.50 MB) for a gain nobody measured.
# RL_NX_GAME_INLINE="-mllvm -inline-threshold=2500" puts it back for an A/B.
set(RL_NX_GAME_INLINE "" CACHE STRING "inlining for the recompiled game and the runtime")
separate_arguments(RL_NX_GAME_INLINE_ARG UNIX_COMMAND "${RL_NX_GAME_INLINE}")
separate_arguments(RL_NX_GAME_OPT_ARG UNIX_COMMAND "${RL_NX_GAME_OPT}")
separate_arguments(RL_NX_GAME_WRAP_ARG UNIX_COMMAND "${RL_NX_GAME_WRAP}")
separate_arguments(RL_NX_GAME_PIC_ARG UNIX_COMMAND "${RL_NX_GAME_PIC}")
add_library(game STATIC ${RL_GEN_SOURCES})
target_include_directories(game PRIVATE "${RL_ROOT}/src/runtime" "${RL_ROOT}/build-switch/gen")
target_compile_options(game PRIVATE ${RL_NX_GAME_OPT_ARG} ${RL_NX_GAME_INLINE_ARG} ${RL_NX_GAME_WRAP_ARG} ${RL_NX_GAME_PIC_ARG} -fno-strict-aliasing -fno-stack-protector -w)

# RL-NX: guest instruction metering (docs/PERF_PLAN.md phase 0). Off in shipped builds, where the
# generated code then carries neither the counter nor its branch; RL_NX_ICOUNT=1 switch/build.sh 8 builds
# the variant whose [meter] line reports M guest insn/frame and ns per guest instruction - the metric
# that compares codegen builds, because it does not move when the scene does.


# RL-NX: the oracle test's probe hook (docs/PERF_PLAN.md phase 0b-ii). Off in shipped builds, where the
# generated code carries no entry call; RL_NX_ORACLE=1 switch/build.sh 8 builds the variant whose functions
# are compared against the interpreter at runtime (runtime/oracle.cpp, R2_ORACLE=<n> arms it).

set_target_properties(game PROPERTIES CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/cmake/clang-game-cxx.sh")

# --- Red Two's runtime ---------------------------------------------------------------------
# RL-NX: the desktop application layer is not part of the Switch build. Each of these is replaced by
# the Switch layer below (main) or ported in a later step; the list is the bring-up TODO.
set(RL_RUNTIME_PARKED
  main.cpp        # the desktop app: SDL window, launcher, menus, restart; replaced by switch/src/main_rl.cpp
  gfx/menu.cpp    # the F1 settings/launcher UI (ImGui)
  gfx/touch.cpp   # Android on-screen controls (ImGui)
  achievements.cpp# RetroAchievements: needs extern/rcheevos (not fetched yet) + libpng + curl
  update.cpp      # desktop self-updater: the kit's updater replaces it
  dolphin_import.cpp
  mods.cpp        # texture-pack/code loading from the desktop user folder; goes to mods/hd on SD
  settings.cpp
  savestate.cpp
  audio.cpp       # SDL audio output; replaced by the Switch audio path (audout)
  )
file(GLOB RL_RUNTIME_SOURCES CONFIGURE_DEPENDS
  "${RL_ROOT}/src/runtime/*.cpp" "${RL_ROOT}/src/runtime/gfx/*.cpp" "${RL_ROOT}/src/runtime/hw/*.cpp")
foreach(_p IN LISTS RL_RUNTIME_PARKED)
  list(REMOVE_ITEM RL_RUNTIME_SOURCES "${RL_ROOT}/src/runtime/${_p}")
endforeach()
list(APPEND RL_RUNTIME_SOURCES "${RL_ROOT}/src/common/disc_image.cpp")

add_library(runtime STATIC ${RL_RUNTIME_SOURCES})
target_include_directories(runtime PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include"   # RL-NX: execinfo.h / dlfcn.h / sys/prctl.h shims, FIRST
  "${RL_ROOT}/src/runtime" "${RL_ROOT}/src/common" "${RL_ROOT}/build-switch/gen")
# RL-NX: TARGET_PC is not a convenience flag. Aurora's dolphin/types.h only defines BOOL under it, and
# without it s32/u32/s64 are typedef'd to `signed long`/`unsigned long` - which are 64-bit on AArch64,
# so every Aurora struct a runtime TU sees would disagree with the Aurora we compiled. (The desktop
# build gets it from Aurora's own targets.)
target_compile_definitions(runtime PRIVATE TARGET_PC)
target_compile_options(runtime PRIVATE ${RL_NX_GAME_OPT_ARG} ${RL_NX_GAME_INLINE_ARG} ${RL_NX_GAME_WRAP_ARG} ${RL_NX_GAME_PIC_ARG} -fno-strict-aliasing -fno-stack-protector -w)
# RL-NX (Stage 10): + rl_turbojpeg
target_link_libraries(runtime PUBLIC ZLIB::ZLIB rl_zstd rl_turbojpeg PNG::PNG nlohmann_json::nlohmann_json
  aurora::core aurora::gx aurora::vi aurora::pad xxHash::xxhash)
# The guest core and its HLE (cpu/interp/vm/hle_*) are built with Clang like the game code: same
# compiler as the code they cooperate with, and the naked setjmp of sjlj.cpp survives.
set_target_properties(runtime PROPERTIES CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/cmake/clang-game-cxx.sh")

# --- the Switch layer ----------------------------------------------------------------------
file(GLOB RL_SWITCH_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/src/*.c" "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
list(REMOVE_ITEM RL_SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/sqlite_vfs.cpp")   # built into sqlite3
if(RL_NX_FASTMEM)  # RL-NX (Stage 11): replaces libnx's weak __libnx_exception_entry
  list(APPEND RL_SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/fastmem_entry.S")
endif()
list(REMOVE_ITEM RL_SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/main_switch.cpp")     # rl_entry, Clang
list(REMOVE_ITEM RL_SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/runtime_switch.cpp")  # rl_entry, Clang
list(REMOVE_ITEM RL_SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/boot_switch.cpp")     # rl_entry, Clang
list(REMOVE_ITEM RL_SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/host_switch.cpp")     # rl_entry, Clang
list(REMOVE_ITEM RL_SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/audio_switch.cpp")    # rl_entry, Clang

# The entry point includes ctx.h for the guest-memory accessors, and ctx.h uses Clang builtins
# (__builtin_rotateleft32) that devkitA64 GCC does not have, so it is compiled with Clang like the
# runtime is and linked in as a library (a per-source COMPILER_LAUNCHER is not applied by this CMake).
add_library(rl_entry STATIC
  "${CMAKE_CURRENT_LIST_DIR}/src/main_switch.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/runtime_switch.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/boot_switch.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/host_switch.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/menu_switch.cpp"  # RL-NX: the - menu (switch/src/menu_switch.cpp)
  "${CMAKE_CURRENT_LIST_DIR}/src/audio_switch.cpp")
target_include_directories(rl_entry PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src" "${RL_ROOT}/src/runtime")
target_include_directories(rl_entry PRIVATE "${RL_ROOT}/build-switch")  # RL-NX: build_number.h (build.sh)
target_compile_definitions(rl_entry PRIVATE RL_NX_VERSION="${PROJECT_VERSION}")  # RL-NX: the - menu version line
target_compile_definitions(rl_entry PRIVATE TARGET_PC)  # Aurora headers: see the note on `runtime`
target_compile_options(rl_entry PRIVATE -O2 -fno-strict-aliasing -w)
set_target_properties(rl_entry PROPERTIES CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/cmake/clang-game-cxx.sh")

add_executable(rl-nx ${RL_SWITCH_SOURCES})
target_include_directories(rl-nx PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src" "${RL_ROOT}/src/runtime")
if(RL_NX_DEKO)
  target_sources(rl-nx PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/deko/host/deko_host_switch.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/deko/host/gpu_submit_diagnostics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/deko/host/posix_stubs_switch.c")
  target_link_libraries(rl-nx PRIVATE melee_deko)
  # RL-NX: gpu_submit_diagnostics.cpp's wrappers were documented as linked with --wrap but had no such
  # flag, so they were dead code: the "at most N entries per kickoff" stat always read 0, a failed
  # submission only surfaced as the generic 2359-0001, and a GPU fault was never reported. Wrapped here.
  target_link_options(rl-nx PRIVATE
    "LINKER:--wrap=nvGpuChannelAppendEntry"
    "LINKER:--wrap=nvGpuChannelKickoff"
    "LINKER:--wrap=nvGpuChannelGetErrorNotification")
endif()

# One rescanning group: the generated game code calls into the runtime (hle_* hooks), the runtime
# calls back into the game (function lookups) and both call Aurora, so plain left-to-right archive
# order leaves thousands of undefined references. LINK_GROUP:RESCAN makes the linker repeat the
# group until nothing is left.
# (explicit --start-group flags: CMake's $<LINK_GROUP:RESCAN> is rejected for this toolchain's CXX
# link language, and RESCAN is what these archives need - see the comment above.)
target_link_libraries(rl-nx PRIVATE
  "-Wl,--start-group" rl_entry runtime game aurora::core aurora::gx aurora::vi aurora::pad "-Wl,--end-group"
  nlohmann_json::nlohmann_json "${DEVKITPRO}/portlibs/switch/lib/libcurl.a")
target_compile_definitions(rl-nx PRIVATE RL_NX_VERSION="${PROJECT_VERSION}" CURL_STATICLIB)

# Threads get 8 MB stacks and exit tears down cleanly (switch/src/clang_tls_switch.c).
target_link_options(rl-nx PRIVATE
  "LINKER:--wrap=pthread_create" "LINKER:--wrap=pthread_join" "LINKER:--wrap=pthread_detach"
  "LINKER:--wrap=exit" "LINKER:--wrap=_exit" "LINKER:--wrap=abort"
  "LINKER:--gc-sections" "LINKER:--allow-multiple-definition")
set(CMAKE_CXX_STANDARD_LIBRARIES "-Wl,--start-group ${MELEE_GFX_LINK_LIBS} -lstdc++ -lnx -lm -lz -Wl,--end-group" CACHE STRING "" FORCE)

set_target_properties(rl-nx PROPERTIES OUTPUT_NAME RL-NX)
nx_generate_nacp(RL-NX.nacp NAME "Rogue Leader" AUTHOR "digdat0" VERSION "${PROJECT_VERSION}")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/icon.jpg")
  nx_create_nro(rl-nx NACP RL-NX.nacp ICON "${CMAKE_CURRENT_LIST_DIR}/icon.jpg")
else()
  nx_create_nro(rl-nx NACP RL-NX.nacp)
endif()
