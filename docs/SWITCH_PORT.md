# RL-NX — Rogue Leader on Nintendo Switch: pre-port survey and plan

Star Wars Rogue Squadron II: Rogue Leader (GSWE64, rev 0), from **Red Two** (static recompilation, PC)
by gminustwo, MIT. Port date: 2026-11-23. Status: **survey done, scaffold in place, no build yet.**

Route (`SKILL.md` §1): *recomp → Aurora/Dawn PC port → deko3d*, i.e. the SFA-NX route
(`references/route-aurora-dawn.md`) with the recompiler pipeline of `references/route-static-recomp.md`.
No decomp and no Switch port of Red Two exists (checked: upstream repo, releases, recomp indexes,
all PC-only as of 2026-11-23), so this is new work.

---

## A. Survey answers (what this game does that the port must handle)

Legend: **R** = `src/runtime`, **A** = Aurora (renderer), **G** = generated game code (from the disc).

### Approach and what exists
- Red Two translates `main.dol` (PowerPC) to C++ **ahead of time** with its own recompiler
  (`src/recompiler`, no symbols; also finds a second program hidden inside `main.dol` that the game
  runs from its own demand-paged virtual memory). The generated code lives in `build/gen`
  (**never committed, never shipped in source form**) and is rebuilt from the player's own disc.
- The runtime (`src/runtime`) emulates the hardware at the register level: CPU state and MMU, guest
  threads, VI/DVD/AI/ARAM/EXI, a MusyX DSP emulation, and a bridge that hands the GX command stream
  to **Aurora**.
- **AArch64 is already a supported target upstream** (the Android build): `sjlj.cpp`, the fiber
  trampoline in `threads.cpp` and the SIGSEGV path in `memory.cpp` all have `__aarch64__` branches,
  and CMake handles cross builds through `R2_HOST_RECOMP`. That is the single biggest de-risk for
  this port: the guest CPU core was made to run on arm64.
- No SIMD intrinsics anywhere in the runtime (`_mm_*`/`__m256` count: 0). The x86-64-v3 (`AVX2`)
  build level only buys FMA and `MOVBE`; on arm64 both are native. Nothing to replace.

### Memory and allocation
- One **4 GiB host reservation** covers the guest's whole effective address space:
  `guest address a` lives at `g_base + a`. Windows/Linux `mapAt()` maps MEM1 (24 MiB) twice
  (0x80000000 cached, 0xC0000000 uncached), the locked cache (0xE0000000, 64 KiB) and a port arena;
  0x7E000000-0x7FFFFFFF is translated by its own software TLB (`vm.cpp`) and never touched directly.
- Pages **past MEM1** are committed lazily when first touched, from a SIGSEGV/VEH handler, because
  Battle of Hoth really does read ~14 MiB past the end of MEM1 (documented in `memory.cpp`).
- `operator new`/`malloc` are **not** replaced by the game: the guest heap is inside MEM1 and reached
  through `rd32/wr32`, so the kit's dlmalloc `operator new` may stay as it is (re-check at link time).
- The exe is linked at a **fixed base, no ASLR**, because save states store host stacks that contain
  return addresses into the exe. Save states are therefore per-build (already true upstream).

### Threads, interrupts, time
- Guest threads are **host fibers** on one host thread (single CPU, as on the console), switched with
  a hand-written `rt_setjmp`/`rt_longjmp` and the fiber trampoline. `kMaxFibers = 16`,
  `kFiberStackSize = 16 MiB`. **All guest threads run on one host thread** — good for Horizon, which
  does not time-slice equal-priority threads.
- Runtime-spawned host threads (~18 sites): `events.cpp` timer thread, `input.cpp` joystick thread,
  `profiler.cpp` stall watchdog, `savestate.cpp` writer, `achievements.cpp` hash/queue workers,
  `update.cpp` update workers, `dolphin_import.cpp` import progress. Plus Aurora's own render/FIFO
  threads. All need names, priorities and cores in the kit's `perf_switch.cpp` placement table.
- Interrupts: `g_irqPending` + safepoints at loop back-edges; an "idle" hook sleeps a spin loop with
  an interrupt instead of burning a core. That maps cleanly onto Horizon (no interrupt lock needed for
  audio as in SFA, because audio here is an emulated DSP driven by the event timer — verify).
- Frame rate: the game advances per retrace (`viFrameHook` → `gfx::vblank()`); pacing belongs to the
  port's vsync layer. 60 fps target.

### Graphics (GX) — the expensive part
- Rogue Leader is a Factor 5 launch title: heavy TEV use, **EFB copies**, `zfreeze`, XFB-based
  effects, many small draws. Red Two carries **33 Aurora patches** for exactly these:
  `zfreeze`, composed XFB, viewport depth range, incomplete primitives, screen-position sprites,
  widescreen/HUD anchoring, **ubershaders** (async pipeline compiles), partial texture copies,
  MSAA depth copies, render-to-texture viewports, NBT cache, forced anisotropy.
- Those 33 patches are what makes the renderer correct for this game. Losing any of them costs
  visual correctness, so they are a hard requirement on the Aurora base (see Measurements).

### Audio
- MusyX DSP is **emulated in software** (`hw/musyx_dsp.cpp` + `hle_musyx.cpp`), mixed by `audio.cpp`,
  output through the platform layer. CPU cost is real but modest; it is a good candidate for its own
  thread and core on Switch.

### Input
- Analog triggers are read through Aurora's pad layer (`aurora::pad::{analogL,analogR}`,
  `set_binding`, profiles). Rogue Leader uses **pressure-sensitive L/R** as well as Z; the Switch has
  digital ZL/ZR. Decision below.
- Button prompts are drawn with the host pad's labels (Xbox/PlayStation sets exist); a Switch set may
  be needed for the in-game prompts to read correctly.
- Rumble yes; four pads possible; gyro code exists (`gyro.cpp`).

### Saves and files
- Memory card: a raw card image in the user folder (`user/saves/GSWE64_A.raw`), `card_image.cpp`
  (`CARD*` at the register level). Horizon's `rename` cannot overwrite → delete-then-rename or
  write-temp-then-swap.
- Disc access: **reads go through `disc::Image`** (`ISO/GCM/RVZ/WIA`); RVZ is zstd-compressed and
  decoded on the fly. Rogue Leader streams from disc constantly, so decompression cost on the Switch's
  A57 matters (decision below).
- Save states: host stacks, fixed addresses, per-build. Fine, but they must be **absent from a first
  release** if the fixed-address scheme cannot be reproduced on Horizon (see Risks).

### Region / revisions
- USA rev 0 (`GSWE64`) only, as upstream. Reject other IDs at start with a message.

### Licences
- Red Two: **MIT**. Aurora: MIT. Dawn: Apache-2.0. SDL3: zlib. SFA-NX Switch layer + `switch/deko` +
  `switch/uam`: GPL-3.0 (own work) with **MPL-2.0** file-level pieces from KartPadNX/Melee-NX/
  SwitchWaker — keep `switch/licenses/THIRD_PARTY.md` and the in-file notices. dlmalloc: public domain.
- Upstream (gminustwo/red_two) is 4 days old and very active; do **not** send our Switch work upstream
  unless asked. Game code, disc images and generated C stay out of git.

---

## B. Decisions (recommendations; confirm or change)

| Decision | Recommendation for RL-NX |
|---|---|
| App / repo | `RL-NX`, SD folder `sdmc:/switch/RL-NX/`, repo `digdat0/rogue-leader-nx`, private until first beta |
| Commit identity | `digdat0`, no `Co-Authored-By` anywhere; `switch/tools/hooks/pre-push` enforces it |
| Our layer's licence | GPL-3.0 (matches the SFA-NX pieces we reuse); upstream stays MIT |
| Route | recomp (host `r2recomp`) → devkitA64/Clang → deko3d |
| Renderer | **deko3d from the start** (Aurora is already the seam; no GL stage needed) |
| Resolution | 720p handheld (fixed rule), 1080p docked with a 720p/900p option |
| Clocks | official retail profile only, as a menu option, restored at exit |
| Frame rate | 60 fps (the game's own); no interpolation hacks |
| Controls | by label; GameCube L/R (analog) → ZL/ZR digital, Z → R; C-stick → right stick |
| Widescreen | the game's own 16:9 + the HUD anchoring already in Red Two's Aurora patches |
| HD textures | support Dolphin packs in `mods/hd/` (Aurora already does) |
| Icon | `switch/icon.jpg`, 256×256 JPEG (libnx's NRO icon limit), embedded by `nx_create_nro`; downscaled from `icon.png` — grid art by **davy**, https://www.steamgriddb.com/grid/544663 |
| Updater | GitHub `releases/latest` of the port's repo, auto-check off |
| Disc formats | ISO/GCM first; RVZ behind a `read-ahead + zstd` decision after measuring |

---

## C. Measurements taken (2026-11-23, this machine)

1. **Red Two's Aurora patch series applies in full to `encounter/aurora` master.**
   `aurora` at `f77492f` (2026-11-05): **33 of 33** patches applied cleanly, in order.
   The same series on Foxhollow's vendored Aurora: **13 of 33** (20 conflict).
   → **Aurora base = `encounter/aurora` @ `f77492f` + `patches/aurora/0001..0033`.**
   (Patches are a *series*: they must be applied sequentially, not independently.)
2. **SFA-NX's deko3d seam needs 12 of its 40 anchors re-anchored, not a rewrite.**
   `switch/builder/integrate-deko.py` against Foxhollow's Aurora: 0 missing (its own pin).
   Against Red Two's Aurora: **12 missing**, all in three places —
   `lib/webgpu/gpu.hpp` (1), `lib/webgpu/gpu.cpp` (6), `lib/aurora.cpp` (5).
   These are the surface / present-target / init / shutdown seams; the deko3d backend itself
   (`switch/deko/backend_*.cpp`) is version-agnostic past `dk_aurora.h`.
   `aurora-switch-io-atomic` and `aurora-switch-card` apply on this base; `aurora-switch-fcvt` does not.
3. **devkitA64 GCC cannot build the guest core; Clang can.** GCC 16.1.0 rejects
   `__builtin_rotateleft32` (`ctx.h:209`) and ignores `[[clang::always_inline]]`; more importantly it
   **ignores `__attribute__((naked))`**, which would silently produce a broken `rt_setjmp` — the fiber
   switch would corrupt every guest thread. LLVM 23.1.5 is installed at `C:\Program Files\LLVM\bin`.
   → Game code **and** `sjlj.cpp`/`threads.cpp` compile with Clang (`switch/cmake/clang-game.sh`),
   everything else with devkitA64 GCC. This is `SKILL.md` §3.2's "compile the game code with the
   upstream compiler", and here it is mandatory.
4. **Horizon lacks the POSIX memory plumbing the runtime assumes**: `<link.h>`, `<sys/mman.h>`,
   `<sys/prctl.h>`, `<ucontext.h>`, signals, `mmap` of 4 GiB at a fixed address. Measured failures:
   `memory.cpp`, `threads.cpp`, `events.cpp` stop at the first missing header.
5. **The host recompiler builds and runs, and the disc is good.** `r2recomp` built with devkitPro's
   msys2 GCC 15.3 + zstd (`ref/zstd`), run on the ISO:
   `main.dol from the ISO image (2440288 bytes)` → **8104 functions, 527252 instructions,
   115 jump tables, 9.4 s**, writing **99 files / 37 MB** of `build-switch/gen`. Two code regions
   (the main `main.dol` and the hidden second program at 0x7FC00000-0x7FD9E2F0) are both found.
   `build-switch/host/r2recomp.exe` is reusable for every later build.
   Two gotchas for the record: the host build needs `-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE`
   (msys2's GCC does not define `_WIN32`, so `disc_image.cpp` takes its POSIX branch for
   `pread`/`fstat`/`fdopen`), and it must be driven through `/c/devkitPro/msys2/usr/bin/env.exe`
   with `PATH=/opt/devkitpro/msys2/usr/bin:/usr/bin` or it cannot find its own headers.
6. **Runtime census on `aarch64-none-elf`** (`-fsyntax-only`, header-only check of
   `src/runtime/*.cpp`; not a link test). **Clang: 11 of 26 pass**, and those 11 include the entire
   hard core — `cpu.cpp`, `interp.cpp`, `vm.cpp`, `hle_game.cpp`, `hle_os.cpp`, `hle_musyx.cpp`,
   `codes.cpp`, `dolphin_import.cpp`, `profiler.cpp`. The 15 failures are only missing headers:
   - **SDL3** (7): `audio`, `gyro`, `hotkeys`, `input`, `mods`, `savestate`, `settings` → solved by
     building SDL3 for Switch (`switch/build-sdl.sh`), not by porting code.
   - **POSIX** (5): `main` (`execinfo.h`), `memory` (`link.h`), `threads` (`sys/mman.h`),
     `events` (`sys/prctl.h`), `http` (`dlfcn.h`) → one `switch/src/posix_switch.h` shim plus the
     memory-model work below.
   - **Aurora's vendored deps** (2): `achievements` (`imgui.h`), `update` (`nlohmann/json.hpp`) →
     initialise Aurora's `extern/` submodules.
   - **`strnlen`** (1): `card_image.cpp` → trivial newlib gap.
   GCC fails the same files *plus* the ten that use `__builtin_rotateleft32`.
7. **Clang invocation from Git Bash needs two things** that cost an hour here, and belong in
   `switch/build.sh`: `-isystem` paths must be **Windows-style** (`C:/devkitPro/...`, a POSIX
   `/c/...` argument is not converted for this option), and the **C++ include dirs must come
   before newlib's C dir** (`c++/16.1.0`, `c++/16.1.0/aarch64-none-elf`, then the C dir) or
   libstdc++'s `#include_next <stdlib.h>` fails.
8. **A space in the repository path breaks the archive step.** This checkout lives under
   `E:\Red Two`, and `switch/cmake/SwitchFoxhollow.cmake` spliced the `ar-rsp.sh` wrapper path
   unquoted into `CMAKE_{C,CXX,ASM}_ARCHIVE_CREATE`, so CMake ran `/e/Red` and Dawn's compiler test
   failed with `/bin/sh: line 1: /e/Red: No such file or directory`. Fixed by quoting the wrapper
   path. Any other path spliced into a command line needs the same treatment.
9. **SDL 3.4.10 patches: five apply, one is obsolete.** `sdl-dusklight-{switch,melee-nx,sfa-nx,
   sfa-nx-shoulders,sfa-nx-rumble}.patch` are a **series** and are git-format patches: they apply
   with `git apply --ignore-space-change --recount`, not with `patch(1)`. `sdl-switch-external-
   graphics.patch` no longer applies and is not needed: SDL 3.4.10 already ships the
   `SDL_SWITCH_EXTERNAL_GRAPHICS` option (`CMakeLists.txt:3319`).
10. **The deko3d seam needs a port, not a re-anchor.** The 12 missing anchors are a symptom: this
    Aurora moved presentation out of `lib/webgpu/gpu.cpp` + `g_surface` into
    `lib/gfx/present.cpp` + `lib/window.cpp`, with `AuroraSurfaceType` (`AURORA_SURFACE_HEADLESS`)
    and `gfx::present::bind_xfb_surface()`. The old seam's `g_surface` anchors no longer exist, so
    the renderer integration has to be written against the new present path. `AURORA_SURFACE_HEADLESS`
    is the natural seam: Aurora already supports running with no surface at all, which is exactly the
    state deko3d wants (it owns the swapchain).
11. **Runtime census with SDL3 headers and the POSIX shims: 19 of 27.** The eight that still fail need
    only dependencies the CMake provides (`imgui.h` from Aurora, `xxhash.h`, `nlohmann/json.hpp`),
    plus `strnlen` (added to `switch/src/libc_switch.c`).
12. **Aurora f77492f wants a Dawn newer than 80ee0043.** `lib/gfx/present_target.cpp:451` declares
    `wgpu::ConvertibleStatus`; our Dawn's `Surface::Present()` returns plain `wgpu::Status`. Two
    lines differ, nothing else: `switch/tools/patch-aurora-dawn-status.py` applies them to the staged
    tree by string match (GNU `patch` cannot: the Aurora tree is CRLF and it refuses its hunks with
    "different line endings").
13. **A per-source `CXX_COMPILER_LAUNCHER` is not applied by this CMake**, so every TU that includes
    `ctx.h` must reach Clang through a *target* launcher. `main_switch.cpp` and `runtime_switch.cpp`
    are therefore built as the `rl_entry` library.
13b. **`TARGET_PC` is load-bearing, not cosmetic.** Aurora's `dolphin/types.h` only defines `BOOL`
    under it, and without it `s32`/`u32`/`s64` are `signed long`/`unsigned long` - **64-bit on
    AArch64** - so every Aurora struct a runtime TU sees would disagree with the Aurora we compiled.
    It was missing from the Switch targets until the input layer pulled those headers in; the fix is
    a `target_compile_definitions(... TARGET_PC)` on `runtime` and `rl_entry`. Any switch-layer TU
    that includes an Aurora header without it is a latent ABI bug, not a compile error.
13c. **Options on a console: `boot.cfg`.** Red Two's switches are `R2_*` environment variables, and
    an NRO has no shell to set them from. `switch/src/main_switch.cpp` reads
    `sdmc:/switch/RL-NX/config/boot.cfg` (root until 2026-11-26) (`KEY=VALUE`, `#` comments) and `setenv`s each line before anything
    reads it, logging every one that is set. That is how `R2_INPUT` scripted presses, `R2_GX_THREAD`,
    `R2_AURORA_DEBUG` and the rest are reachable on the console.
13d. **Measured headroom (Ryujinx, 640x480, title screen).** `g_idleTb` - the time the guest's host
    thread spends waiting for its next event - says the guest thread is busy ~640 ms per 120 frames
    (~5.3 ms per frame, ~32%) and idle ~1360 ms (~68%). GX processing on Aurora's own thread
    (`R2_GX_THREAD=1`) measures the same within noise on this scene, so the port keeps it inline by
    default (one less thread, no cross-thread hand-off) and leaves the switch for heavier scenes.
    Note fps cannot show this: the guest paces itself on the emulated retrace, so it reports 59.9
    whether it has 5 ms or 16 ms of work.
14. **`rt::logf` and `rt::describeAddr` live in `src/runtime/main.cpp`**, the desktop app this port
    parks, and the guest core calls both (cpu.cpp's fatal paths, threads.cpp's backtrace).
    `switch/src/runtime_switch.cpp` provides them for the Switch: `logf` goes through
    `melee_nx_log_write` (thread-safe, SD card) and `describeAddr` searches the generated
    `g_symNames`. It also defines the `aurora_flush_caches` hook that `clang_tls_switch.c` calls on
    every exit path.

---

## D. Architecture of the Switch version

```
disc (ISO/GCM/RVZ, player's own) ─► host r2recomp ─► build-switch/gen/*.cpp   (never committed)
                                                        │
                          generated game code (Clang) ──┤
                          runtime (devkitA64 GCC)  ─────┤
                                                        ▼
                            guest CPU (fibers) ─► GX FIFO ─► Aurora GX thread
                                                                 │  (patched: 33 Red Two patches)
                                                                 ▼
                                              Dawn Null + Tint (objects, WGSL→GLSL)
                                                                 ▼
                                       switch/deko: kd shim + direct recorder ─► deko3d ─► screen
```

Reused as-is from SFA-NX (`switch/`, already copied here): async log, dlmalloc + leak finder, thread
registry and clean exit/relaunch, updater, vsync pacing, profiler, SQLite VFS, the patched deko3d and
`uam`, the shader database tooling.

### The five problems to solve, with the chosen approach

**1. Guest memory without `mmap` or page faults (the central change).**
Horizon gives no 4 GiB fixed reservation and no user-visible page faults, so `g_base + a` and the
"commit pages past MEM1 on first touch" trick are both unavailable. Approach: a `RL_NX` variant of the
address translation in `ctx.h`/`memory.cpp`:
- MEM1 + the heap extension live in **one contiguous arena** (64 MiB: 24 MiB MEM1 + room for the
  14 MiB-past-the-end reads Battle of Hoth performs), allocated once at start and zero-filled, so no
  fault handling is needed at all.
- Cached and uncached views both collapse to the arena: `hp(a) = g_mem + (a & 0x3FFFFFFF)`.
  This must be checked against every place that assumes `g_base + a`, notably `physPtr()`,
  `faultDump()` and DMA writes from Aurora's threads.
- The locked cache (0xE0000000, 64 KiB) and the port arena get their own small allocations.
- The VM region already goes through `g_vmRead/g_vmWrite`, so it needs only its page stores pointed at
  ARAM-backed buffers rather than at `g_base + a`.
- Cost: one extra `and` on the guest load/store path. Measure it (`perf_switch`) before optimising.
- Consequence: **save states may be dropped from the first release** (they additionally need the fixed
  exe base and host stacks at fixed addresses). Decide after boot works; a release without save states
  is acceptable, a release that crashes on load is not.

**2. Fibers and `setjmp` on AArch64.** Already implemented upstream (`sjlj.cpp` AArch64 branch,
`r2FiberTrampoline` AArch64 branch) — but they must be compiled with **Clang**, because GCC ignores
`naked` (measurement 3). Fiber stacks come from the fixed stack region; on Switch reduce
`kFiberStackSize` from 16 MiB to something sane (1–2 MiB is plenty for recompiled frames) and allocate
16 stacks = 16–32 MiB.

**3. Renderer.** Aurora @ `f77492f` + the 33 patches + the platform patches that still apply
(`io-atomic`, `card`), then re-anchor the 12 seam edits for the newer Aurora surface architecture
(`lib/window.cpp` now owns the surface; `g_surface` moved out of `lib/webgpu/gpu.hpp`), then
`integrate-deko.py` + `switch/deko`. Dawn is built **Null + Tint only** (`switch/build-dawn.sh`), SDL3
from `ref/SDL-dusklight` with `sdl-dusklight-switch.patch` and the external-graphics patch.

**4. Disc and assets.** ISO/GCM first (SD reads are cheap and constant); RVZ (zstd) is a measured
follow-up, since Rogue Leader streams continuously and A57 zstd decode competes with the game thread.

**5. Audio and pacing.** Run the MusyX mix on its own thread and core; output through SDL's Switch
audio driver; vsync-event pacing from the kit, 60 fps target.

---

## E. Bring-up checklist (each step has an exit test)

- [ ] **0. Survey + decisions** — this file. Confirm part B.
- [x] **1. Repo + kit** — done: `switch/` (renamed via `kit/new_port.py`), `switch/deko`, `switch/uam`,
      `switch/third_party/deko3d`, `switch/builder`, patch sets, `.gitignore`. Aurora is pinned in
      place: `extern/aurora` = `f77492f` + the 33 patches, applied and verified. Still to do:
      `git init` + identity hooks.
- [x] **1b. Guest memory arena** — done and compiling for the target: `src/runtime/memory_switch.cpp`
      (256 MiB arena, locked-cache buffer, heap fiber stacks), `ctx.h` (`memPtr`, `hp`, uncached-alias
      fold), `memory.cpp` guarded out, `cpu.cpp`/`hle_game.cpp` on `hp()`, 1 MiB fiber stacks on the
      Switch. `switch/src/main_switch.cpp` checks launch mode and proves the alias on hardware.
- [x] **2. Host recompiler** — built (`build-switch/host/r2recomp.exe`) and run against the disc:
      8104 functions → `build-switch/gen` (99 files, 37 MB), 9.4 s. `code_000.cpp` compiles for
      `aarch64-none-elf` with Clang (741 KB object, 5.3 s/file → ~8 min for the set on one job).
      Exit met: `gen/funcs.h` + `code_*.cpp` exist and compile for the target.
- [x] **2b. All 96 generated files compile** for the target: the CMake `game` target builds them
      with Clang through `switch/cmake/clang-game-cxx.sh` (96/96, no errors, 8 jobs).
- [x] **3. Build the guest core for AArch64** — 19 of the 27 runtime TUs compile for the target and
      the CMake build now reaches the **link** of `RL-NX.elf`. What is parked for the Switch, and
      why, is `RL_RUNTIME_PARKED` in `switch/CMakeLists.txt` (the desktop app layer);
      `switch/src/main_switch.cpp` is the bring-up entry point until the real one replaces it.
- [x] **3b. A buildable, booting NRO** — `bash switch/build.sh 8` produces
      `build-switch/RL-NX.nro` (305 KB, valid NRO header, NACP attached) and a 174 MB debug ELF.
      **Verified on Ryujinx** (portable install at `/e/ryujinx_test`, SD card at
      `portable/sdcard`): the NRO starts, reports title takeover, `rt::memInit()` allocates the
      256 MiB arena at `0x2025A0000`, and the guest alias test reads back `12345678` through both
      `0x80000100` and its `0xC0000100` uncached view — the §C memory model, on an emulated
      Horizon kernel, with no mmap and no page faults. The log is `sdmc:/switch/RL-NX/logs/log.txt`.
- [x] **3c. The real entry point** — done: `main_switch.cpp` runs the desktop boot sequence
      (`eventsInit` → `memInit` → `tableInit` → disc → `threadsInit` → `bootDiscSwitch` → `hwInit` →
      `threadsRun`). `boot_switch.cpp` carries the two static boot helpers from `main.cpp`
      (`loadFst`, `bootDisc`), including the apploader low-memory block, and finds the disc in the
      port's SD folder. `host_switch.cpp` is the Switch side of the parked desktop files (disc,
      `fatal`/`quit`/`envFlag`, settings defaults, `mods::userDir` → `sdmc:/switch/RL-NX/`, no-op save
      states/achievements/gyro/input-rumble/gfx-aspect, and an audio `push` that counts blocks).
- [x] **3d. The guest boots the game** — with the player's disc at `sdmc:/switch/RL-NX/game.iso` the
      NRO loads `main.dol`, sets up the apploader block and the game's own OS takes over. From the
      log on Ryujinx: `Dolphin OS $Revision: 37 $`, `Kernel built : Jul 19 2001`, `Memory 24 MB`,
      `app booted from bootrom`, `--- GSYS VM init ---`, the game's own VM at `0x7F000000` (16 MiB,
      1.5 MiB of page tables), `Copying 1603808 bytes of text section data to 0x7fc00000`, then
      `--------- resident setup done 20485888 bytes free ---------` and a steady **59.9 fps** (2280
      frames) with MusyX producing 32 kHz blocks. Exit test met: the guest runs its own main loop.
- [x] **4a. Aurora runs the game's GX** — `gfx::switchInit()` (a marked addition to
      `gx_bridge.cpp`) configures Aurora like `init()` does without the desktop parts, with
      `AURORA_SURFACE_HEADLESS`, `R2_AURORA_DEBUG` for detail, and the same sink/sync wiring, and
      `viFrameHook` now calls `gfx::vblank()`. Verified on Ryujinx: `Aurora up: backend 8, window
      640x480` (backend 8 = Null; Vulkan and OpenGL ES are unavailable there), then the game runs at
      59.9 fps with `aurora: 8 draws (0 merged), pipelines 0 queued 11 created` every second - the
      guest's whole display list is being translated into Aurora pipelines, just with no swapchain
      to show it. Two things this needed: Dawn's Null device caps its accounting at 512 MiB
      (`switch/tools/patch-dawn-null-memory.py`, now 8 GiB - a game's pipelines and staging buffers
      pass it mid-boot), and `srcdir/aurora-cache` has to exist for Aurora's pipeline cache.
- [~] **4b. Picture on screen** — the deko3d renderer (§C.10). **It builds and initialises.** `RL_NX_DEKO=1
      bash switch/build.sh 8` links (63.1 MB NRO): `libdeko3d_release.a`, `libuamlib.a`, all 17
      switch/deko translation units and the deko-seamed Aurora, plus `--wrap` for the 40+ Dawn entry
      points in `switch/deko/wrap-symbols.txt` (that wiring was missing from the adapted Deko3D.cmake).
      At run time it reaches: device created (origin upper left, depth 0-1), queue created, image heap,
      3 x 1280x720 swapchain images, the uam worker (self-test shader compiled in ~1 s), the texture
      policy line.
      The seam itself is three call sites, all `RL-NX-DEKO` marked and applied by the rewritten
      `switch/builder/integrate-deko.py`: `aurora_switch_dk_init(gx::GXPipelineConfigVersion)` at the top
      of `webgpu::initialize`, `aurora_switch_dk_shutdown()` at the top of `webgpu::shutdown`, and
      `kartpad_deko_present(webgpu::g_frameBuffer.texture.Get(), w, h)` in `end_frame`'s render-worker job
      (the headless surface creates no present target, so `present::submit()` returns at its first line).
      **It now runs the game stably**: 7320 frames at a steady 59.9 fps for over two minutes, 8 draws a
      frame (the same attract screen the headless build shows), one uam worker, no crash. Four bugs
      were found and fixed to get there, each one named by a symbolized guest stack:
      1. **Dawn's entry-point lookup on a native handle** - crash in
         `dawn::native::PipelineBase::PipelineBase` from
         `readback::gpu::initialize_efb_gather()`. switch/deko's WGSL modules are native handles by
         design ("the depth-peek compute pipeline is compiled out under deko"), but this Aurora builds
         two compute pipelines (efb_gather, copy_pack), and a compute pipeline asks Dawn for the module's
         entry point. Under `AURORA_GFX_DEKO3D` readback now reports unavailable instead (seam #4 in
         `integrate-deko.py`); the native backend keeps its own EFB snapshot.
      2. **Unbounded staging generations** - `Buffer::prepare_map()` allocated a new 107 MiB block per
         write and never reused one: 20 of them (2+ GiB) in 35 s, then the allocation inside
         `dkCmdBufCopyBufferToImage` failed. Capped at 4 per buffer, reusing the oldest with a *bounded*
         (2 ms) fence wait - waiting forever hung the game when the queue could not progress.
      3. **GPFIFO ring overflow** - `Queue::appendGpfifoEntries` -> `RaiseError` from
         `__wrap_wgpuQueueSubmit` after ~28 s. Entries were only kicked off once 128 were pending, which
         never happened; the ring filled at a few entries a frame instead. Every submit now flushes.
      4. **The seam ran once per backend attempt** - `aurora_switch_dk_init` sat at the top of
         `webgpu::initialize`, which Aurora calls once per candidate backend, so it created a device,
         queue, image heap and uam worker each time (`uam worker started` x3) and the repeated thread
         creation storm took the emulator down at ~16 s. It is now idempotent.
      Two blockers remain:
      - **Swapchain on Ryujinx**: `nwindowSetDimensions`/`nwindowConfigureBuffer` answer `0xF59` while the
        same run's Nv buffer maps succeed, so the emulator's window binder cannot take deko3d's buffers.
        `init_swapchain` now probes with `nwindowSetDimensions` first (the release deko3d aborts *inside*
        `dkSwapchainCreate`) and runs without presenting instead of dying.
      - **First shader module**: after the swapchain is skipped, Aurora's adapter request fails twice
        (`No supported adapters`, then the Null backend is adopted) and the renderer dies in
        `dawn::native::ShaderModuleBase::GetEntryPoint` (null `ShaderModule`) - the next thing to chase.
      The earlier anchor census (§E 4b, below) describes the Foxhollow-shaped script that was replaced.
- [ ] **4b-old (superseded anchor census)** — the seam as measured: a report-only
      run of the staging script (`switch/tools/check-deko-anchors.py`, which replaces `edit()` with a
      recorder so it does not stop at the first miss) says **15 anchors missing, 6 present, 0 applied**,
      and every miss is in the surface/present path:
      - `lib/webgpu/gpu.hpp`/`gpu.cpp`: `extern wgpu::Surface g_surface;`, `create_surface()`,
        `g_surface.GetCapabilities`, `select_present_mode`, `g_surface.Configure`, the `!g_surface`
        guard, `depthClipControl` - all gone or moved;
      - `lib/aurora.cpp`: the backend Null branch, `refresh_surface`, the acquire (`is_presentable() &&
        g_surface`), `canPresent && g_surface`, `status = g_surface.Present()` - presentation now
        lives in `lib/gfx/present_target.cpp` (`PresentTarget::surface`, `bind_xfb_surface()`,
        `aurora.cpp` only calls `gfx::xfb_target()` / `gfx::present::*`);
      - `lib/gfx/depth_peek.cpp` does not exist in this pin at all.
      The object-model half (the `#include`, the Null backend selection, `shutdown()`, the depth
      texture line, dual-source blending) still applies. So the rewrite is: keep
      `AURORA_SURFACE_HEADLESS`, create the deko present texture next to `g_dekoPresentTexture` in
      `present_target.cpp`'s target creation, call `aurora_switch_dk_init(GXPipelineConfigVersion)`
      from `gpu::init` after the device exists, and replace `target.surface.Present()` (line ~454 of
      `present_target.cpp`) with `kartpad_deko_present(target texture)`. Then build `switch/deko`,
      `uam` and `deko3d` with `-DRL_NX_DEKO=ON`.
- [x] **4c. Seeing the renderer without a swapchain.** `COS_DK_CAPTURE=1` in `boot.cfg` makes
      `switch/deko/device.cpp` read the presented picture back at present time, log its statistics and
      write a downscaled PPM to `sdmc:/switch/RL-NX/logs/deko-capture/`, one image every five seconds from
      20 s on (at most 24 a run). This is the only way to see the output on an emulator, whose window
      cannot take deko3d's swapchain (§E 4b), and it settled §E 7 within one run. Two bugs found while
      making it work: the schedule was re-evaluated inside `capture_image()` after the caller had already
      consumed it (a debounce closed the window on the first call, so no image was ever written once the
      game was busy), and the readback block was allocated per image, which failed silently - starving
      under the frame's own allocations - so it is now allocated once and reused, and a failure logs.
- [x] **5. Sound output** — `switch/src/audio_switch.cpp`: libnx `audout` (48 kHz stereo s16 LE), four
      6144-byte buffers, no audio thread (reclaim is non-blocking, and a full queue drops and counts
      rather than stalling the game thread), endian swap from the MusyX blocks and a 32 kHz -> 48 kHz
      two-point lerp with phase carried across blocks. `audio::init()` is called from the entry point
      (it was the desktop `main.cpp` that used to), and the log confirms `audout up: 48000 Hz, 2
      channels, 4 buffers of 6144 bytes` with buffers draining and 0 dropped.
      Two bugs found on the way: the staging vector counts **samples**, not frames (the first version
      copied twice what it erased - the counters now reconcile exactly: 8500 blocks of 160 frames ->
      1328 buffers of 1536 frames, precisely 1.5x), and `audio::init()` was never called.
- [ ] **5b. AI cadence under emulation** (unchanged, see below) — with `R2_AI_LOG=1` every AI boundary is 5-30 ms late (1029
      of them in 30 s) and the stream arrives ~16-20% short, so the device starves and the output is
      gappy; the guest's mix cannot keep a 5 ms audio clock when the AI is serviced at safepoints.
      The resampler therefore measures the *arriving* rate instead of trusting the DMA registers
      (clamped to +/-25%, smoothed). On Ryujinx that measurement is itself distorted (emulated ticks
      vs the real-time audio device), so this needs one run on hardware before it is called right.
- [x] **6. Input** — `input.cpp`, `hotkeys.cpp` and `gyro.cpp` are back in the build (they compile
      for the target once `TARGET_PC` is defined; that was the whole reason they were parked).
      Aurora's PAD initializes (`input::init(true)` from `switchInit`) and the log shows the
      controller scan on its own thread with the game still running. SDL3's Switch driver supplies
      the pads, so on hardware the joycons and Pro Controller are what the game sees.
- [x] **7. The 8 draws were the memory-card dialog** (answered by the captures, §E 4c). A card Red Two
      created was written erased (all `0xFF`), so the game stopped on "This Memory Card contains
      corrupted data and needs to be formatted" and drew that text and its menu - 8 draws, no motion,
      unaffected by `R2_INPUT`. `hw.cpp`'s create path now formats a new card with `rt::card::format()`
      ("new card ... (4 Mbit, 59 blocks, formatted)"), and the guest walks past it on its own: same run,
      **1400-1700 draws a frame** (5300 merged), pipelines created, 600 KiB of vertices a frame, textures
      uploading, a steady 59.8 fps. The attract screen is live - there was never a stuck SI handshake.
- [ ] **7b. The picture is squeezed into a thin horizontal band** (measured, cause not yet found). The captured presented image (640x480)
      has content only in 16 of 240 rows: a bright band around the vertical middle, a bright first row,
      black elsewhere (lit 19022/307200 pixels, RGB up to 92, alpha 0 - the present target carries no
      alpha, which is expected). Real 3D geometry is being drawn, so the remaining error is in the
      transform or the viewport rather than in the draws: check the GX viewport's Y scale/offset and
      `kartpad_deko_present`'s blit of 640x480 into 1280x720 first, then the copy that fills the present
      texture. Diagnose with `COS_DK_CAPTURE=1` (§E 4c), one image every five seconds.
      **What the captures and `R2_DRAW_LOG` established** (so the next session starts from facts):
      - **the presented texture holds exactly 30 of the 480 rows.** A full-resolution capture
        (`R2_CAPTURE_FULL=1`) puts the content in rows 228..257, all 640 columns, and the lit count is
        19200 = 640 x 30 exactly. Stretched back to the frame's aspect it reads as a *slice of a 3D scene*
        (smooth grey shading with diagonal edges), not as a squashed logo - the earlier reading of it as a
        compressed picture came from the 8x zoom of a 320x15 downscale. Every capture in a run shows the
        same slice, only brighter (the fade-in), so the same point of the frame is being presented every
        time, not a random tear.
      - the guest asks for a **full-frame** viewport and scissor: `viewport scale 320 -240` (i.e. 640x480)
        at `origin 660 580`, `scissor x 340..979 y 340..819`. All four distinct combinations in a run are
        full-frame; there is no short viewport a 30-row band could come from.
      - the +340 in both is the scissor box offset the GX bridge already documents (BP 0x59), but **BP
        0x59 is never written in this build** (the `R2_COPY_LOG` line added to that case never fires), so
        `s_boxOffX/Y` stay 0 and neither the projection shift nor a viewport translation is applied. Three
        mappings were tried - projection shift (old), viewport translation (`R2_BOX_VIEWPORT`), and
        none - and all three produce a band: the two-pass halves (`y 356..597`, `y 578..819`) each draw
        the same full-frame viewport, so the offset is not what squeezes the picture. **The viewport
        translation and its `R2_BOX_BIAS` companion are reverted** (they are gone from `gx_bridge.cpp`:
        the projection carries the offset again, `sendProjection`, and XF 0x10 is forwarded untouched), so
        one less variable is in play. `bp59` is `0x2A8AA`, which decodes to an offset of (0,0): the +340 in
        the guest's viewport and scissor is the game's own window-coordinate origin, not an offset, which
        is why Aurora's `origin - 340` viewport is exactly right for it.
      - a pass readback taken mid-frame is partial (5350 of the eventual lit pixels), so pass images say
        nothing; only the presented image is usable.
      - **the passes say the frame is drawn into the presented target.** With `R2_DRAW_LOG=1` the deko
        backend logs every pass with its target (`pass into <label> WxH (mip N), clear B`). A frame is two
        passes into `Render texture 640x480`, one clearing and one not, which is the game's two-pass frame
        (the halves at scissor rows 16..257 and 238..479 once the offset is applied). There is a
        `Copy Conv Texture 640x450` pass near the start, the XFB-sized conversion, and after it no more
        copies. So the frame really is drawn into the texture that gets presented, and 30 of its 480 rows
        are what survives - the two passes between them must be drawing far less than a half each.
      - Aurora presents at the end of the frame in its render worker (`lib/gfx/frame.cpp`, the
        `kartpad_deko_present` seam) from `webgpu::g_frameBuffer.texture`, so the texture handed over holds
        a 640x30 strip while the frame logs 1400+ draws. Either only that strip reaches `g_frameBuffer`
        (the game drawing into another target and a copy moving part of it) or the record of the frame is
        cut short. `EFB 0..3` pass labels in the earlier captures suggest several such targets are in play:
        the next step is to log which texture `g_frameBuffer` is, per pass, and where the game's writes to
        the other EFB targets go.
      - **the guest's viewport never reaches the render pass.** The deko wrapper logs what Aurora asks for
        (`R2_DRAW_LOG`, `viewport x y wxh` in `backend_commands.cpp`) and it is always
        `0.0 0.0 640.0x480.0` - the full target - while the guest's registers say origin 660,580. So Aurora
        normalises the viewport and the +340 cannot be what moves or squeezes the picture; the placement
        comes entirely from the transform data. `bp59` is `0x2A8AA`, i.e. offset (0,0) by the field's own
        convention, so nothing in the guest is asking for a shift either.
      - `R2_NO_MERGE=1` changes nothing (same band, same rows), so it is not draw merging.
      - 2D is right and 3D is not: the memory-card dialog and its text filled the frame correctly before
        the card was formatted. The logo quad is drawn with an **orthographic** projection whose logged
        registers are self-consistent for 640x480 (`p0 0.003125` = 2/640, `p1 -1` = the left edge as
        origin, `p3 1` = y from the top). A projection that says 640x480 with a picture 28 rows tall in the
        middle is a **vertical scale of about 1/16 inside the transform**, not a viewport or scissor
        effect, which leaves the XF position matrix / model-view data, or the shader's reading of it
        (`switch/uam` translates Aurora's WGSL to deko3d shaders), as the place to look.
      **Measured with `R2_DRAW_BOUNDS=1`** (the CPU-side check added for this, see the diagnostics list):
      - **the guest's geometry and the bridge's transform are not the problem.** Every draw of a frame is
        transformed on the CPU with the position matrix, the projection (scissor box offset folded in) and
        the XF viewport: the full-frame quad covers EFB `x -3..643 y -2..482`, the 2D HUD pieces land at
        `y 60..78`, `y 310..358`, the 3D draws of the attract scene at `y 141..170` - the whole 640x480
        frame, in the right places. Both placements are printed, and the "hardware" one (the game's own
        offset, before the fold) also covers the frame.
      - **the clip is not the problem either.** With `R2_DRAW_LOG=1` the deko backend now logs every
        distinct scissor rectangle: `0,16 640x242` (the top half, screen rows 356..597) and `0,238 640x242`
        (the bottom half, 578..819 with the 238 offset), plus `0,0 640x480` for the full-target passes.
        Their union is the whole frame. The viewport the renderer is given is `0,0 640x480`, the passes go
        into `Render texture 640x480`, and `present from Render texture 640x480` confirms the presented
        texture is that same target. Relaxing the tests (`R2_GX_OVERRIDE=zalways,aalways,nocull,nofog`)
        changes nothing, `R2_NO_MERGE=1` changes nothing.
      - so the vertical mapping is destroyed **inside the renderer's per-draw vertex path**, after the data
        the bridge logs. `bp59` really is never written (the `R2_COPY_LOG` line now fires - see below), so
        the offset is (0,0) and the fold is a no-op: this is not the scissor box offset at all.
      - the presented band is `rows 228..257`, full width, hard cut above and below: a **vertical
        compression of about 1/16** (480 / 16 = 30 rows), x untouched. That matches `widescreen_scale`'s
        `ProjScale` shape (a per-draw y scale applied to the projection in `command_processor.cpp`), and
        the next check is what `widescreen_factor()`/`letterbox_factor()` return on the console: k should
        be 1 for a 640x480 target and v = 480/450 = 1.0667 (`xfbHeight` 450 is confirmed by the copy log),
        so any value near 16 there is the bug.
      - **`R2_COPY_LOG` and `R2_DRAW_BOUNDS` were inert on the console until now**: `main_switch.cpp`
        enters `switchInit()`, not the desktop `init()`, and only the latter read them. The earlier "BP
        0x59 is never written" conclusion rested on a log line that could not have appeared; it is true,
        but it had to be re-measured (it now is, by the same line).
- [ ] **3. Toolchain** — `build-dawn.sh`, `build-sdl.sh`, `build.sh` and a linking `.nro`.
      Exit: `.nro` links; `nm` shows no game symbol replacing a libnx/SDL symbol.
- [ ] **4. Switch shell** — SD folders, `boot.txt` + async log, launch-mode check (title takeover),
      disc lookup and `GSWE64` ID check, crash handler.
      Exit: on hardware `boot.txt` has the stamp; a wrong disc or applet mode shows a message.
- [ ] **5. Guest memory + fibers** — the arena variant, fibers on AArch64.
      Exit: the game reaches its own main loop headless (`R2_NO_GFX=1`), no `bad call` in the log.
- [ ] **6. First picture** — Aurora → deko3d, quadrant test, title screen.
      Exit: title screen upright, dock/undock without corruption.
- [ ] **7. Gameplay** — input by label, audio, save + reload.
- [ ] **8. Exit and restart** — quit/relaunch/Restart 3× each with `0 left` threads.
- [ ] **9. 30–60 min hardware session** — flat heap, steady fps, HOME/sleep/dock.
- [ ] **10. Profile**, then optimise one switchable change at a time.
- [ ] **11. Features** (menu, widescreen, HD textures, updater, shader seed), **release** per the
      done bar in `references/new-game-survey.md` part C.

---

## F. Risks, ranked

1. **Aurora version drift** (medium, quantified): 12 seam anchors to re-anchor; if a further Aurora
   change breaks `switch/deko`'s assumptions, fall back to Foxhollow's pin and port the 33 game patches
   forward instead (20 conflicts — the worse option, but a real fallback).
2. **The 4 GiB → arena memory change** (medium): it touches the hottest path and every place that
   assumes a flat `g_base`. Mitigate with `R2_WATCH`/`R2_FAULT_DUMP`, the existing `--native-verify`
   style comparisons against a PC run, and a paged build for A/B.
3. **Guest-side performance** (medium): Rogue Leader is heavier than the three reference ports.
   Measure early with the profiler; the Android AArch64 target suggests the CPU side is viable.
4. **RVZ streaming cost** (low): ship ISO/GCM first.
5. **Save states** (low, scoped out of v1 if needed).

---

## G. Repository layout

```
RL-NX/
  src/            Red Two runtime + recompiler (upstream, MIT) — our edits marked // RL-NX:
  config/GSWE64/  symbols, hooks, layout for the recompiler
  patches/aurora/ Red Two's 33-patch Aurora series (required for correct graphics)
  extern/aurora   pinned Aurora (clone of f77492f + the 33 patches; NOT upstream's submodule dir)
  switch/         the SFA-NX Switch layer, renamed: src, deko, uam, third_party/deko3d, builder,
                  cmake, patches, tools, licenses
  docs/           this file
  build-switch/   build output, generated game code (git-ignored, never shared)
```
