# RL-NX — Rogue Leader on Nintendo Switch (port README)

This is the **port log** for running Star Wars Rogue Squadron II: Rogue Leader (GameCube,
`GSWE64` rev 0) on Nintendo Switch homebrew, from the PC static-recompilation project
[Red Two](../README.md) by gminustwo.

Upstream `README.md` describes **Red Two itself** (Windows / Linux / Steam Deck). Everything below is
the Switch layer: what exists, what was decided, what is broken, and what is left.

> **No game data is in this repository, ever.** No disc images, no generated game C, no keys. You need
> your own copy of the USA rev-0 disc; `switch/build.sh` recompiles the game from it into
> `build-switch/gen/` (git-ignored, never shared). Never send this work upstream: Red Two is MIT and
> actively maintained by someone else, and we follow it by hand.

- Status date: **2026-11-26** - performance work; the picture and the in-level flicker are fixed
- Upstream base: Red Two `VERSION 1.2`, Aurora `encounter/aurora@f77492f` + Red Two's 33 patches
- Current build: **`40,706,902 B`**, the A baseline of the hardware ladder (see 7.1). The first
  bring-up sizes (headless `60,029,125 B`, deko `63,154,373 B`) are history, not this tree
- **It runs and it draws correctly** - full picture, menus once, movies, the mission's world. What is
  left is **speed**: an in-level frame is **47.35 ms (~21 fps)** and the guest's own recompiled code is
  more than half of it (7.1). The `-` in-screen menu is in the source but not drawn yet (7.2).
- **Handing over?** `AGENTS.md` is the executable brief: the build and smoke commands, the `-` menu's
  exact remaining edits and its bisect matrix, the optimisation queue with acceptance tests, and the
  traps that have already cost days.

---

## 1. At a glance

| | |
|---|---|
| Route | static recompilation (host `r2recomp`) → runtime → Aurora/Dawn **Null + Tint** → **deko3d** |
| Guest CPU | reinterpreted PowerPC on **AArch64**, 16 host fibers on one host thread |
| Guest memory | one **256 MiB arena**, no `mmap`, no page faults (`hp(a) = mem + (a & 0x3FFFFFFF)`) |
| Renderer | Aurora (patched) with the GX FIFO bridge; **deko3d** for the screen, `uam` compiles the shaders |
| Output | 640×480 native EFB, resampled to a 1280×720 swapchain |
| Audio | libnx `audout`, 48 kHz s16, MusyX 32 kHz → 48 kHz lerp |
| Input | Aurora's pad layer, mapped **by label** (decision: GC L/R → ZL/ZR, Z → R; the label set itself is unverified) |
| Save | raw memory-card image in `sdmc:/switch/RL-NX/saves/`; save states / achievements parked |
| Builds | `bash switch/build.sh 8` (headless, default) and `RL_NX_DEKO=1 bash switch/build.sh 8` |

What actually works today, verified in a log:

- the guest boots, loads `main.dol`, runs its own OS and main loop at a steady **59.9 fps**;
- the GX stream is translated into Aurora pipelines (`1400–1700 draws a frame`, 5000+ merged);
- the deko3d renderer initialises, creates a swapchain **on hardware**, presents, and the game's
  snapshot is `640x480, native present`;
- sound comes out of `audout` with **0 dropped buffers**;
- memory card (created + formatted) and the disc reader work; input works from build 08:56 in
  Ryujinx (§8.6), hardware unconfirmed;
- the window hand-over works on hardware: the boot report owns the window first, then the renderer
  takes it, creates the swapchain and presents the game.

What does not work:

- **speed**: an in-level frame is **47.35 ms (~21 fps)**, 17-19 fps in the `[perf]` windows of
  the same run, and the measured ceiling on this codegen is ~39 fps even with the port's whole
  share removed (7.1);
- **the `-` in-screen menu**: built, never seen on screen, its drawing call switched off (7.2);
- exit/restart, updater, shader seeding, widescreen and docked 1080p: not done or not verified.

---

## 2. Quick start

```bash
# once
bash switch/build-dawn.sh      # Dawn, Null + Tint only
bash switch/build-sdl.sh       # SDL3 from ref/SDL-dusklight (+ the Switch patch series)

# every build (headless first: it proves the guest + Aurora path without the screen)
bash switch/build.sh 8                     # -> build-switch/RL-NX.nro
RL_NX_DEKO=1 bash switch/build.sh 8        # -> build-switch/RL-NX.nro, deko3d, ~5 min more
```

Outputs land in `build-switch/RL-NX.nro` and the SD mirror
`build-switch/sdcard/switch/RL-NX/RL-NX.nro`. Logs of the last build are
`build-switch/logs/build-headless.log` and `build-deko.log` (both must end with `0 errors`).

On the console:

1. `sdmc:/switch/RL-NX/RL-NX.nro` + your own disc next to it (any single `.iso/.gcm/.rvz/.wia`).
2. Start with **title takeover** (hold R on any game). Applet mode has too little memory.
3. `sdmc:/switch/RL-NX/logs/log.txt` is the log. The folder root holds only the NRO and the disc; `config/`
   (the `-` menu's saved rows in `menu.cfg`, Aurora's `input.json`, an optional diagnostics `boot.cfg`),
   `cache/` (`deko/`, `aurora/`), `logs/` and `saves/` hold the rest (2026-11-26).

A clean build is ~15 min on 8 jobs (the generated game code and both Aurora stagings are rebuilt);
incremental builds are 2–4 min. `switch/README.md` covers the toolchain prerequisites and the
black-screen checklist.

---

## 3. Architecture

```
disc (yours) ─► host r2recomp ─► build-switch/gen/*.cpp        (git-ignored, never shared)
                                      │
                  generated game code (Clang) ──┐
                  runtime + guest core (GCC)  ──┤
                                                ▼
                      guest CPU (fibers) ─► GX FIFO ─► Aurora GX thread
                                                            │  (33 Red Two patches)
                                                            ▼
                                            Dawn Null + Tint (objects, WGSL→GLSL)
                                                            ▼
                                  switch/deko (kd shim, direct recorder, --wrap) ─► deko3d ─► screen
                                                            ▲
                                            switch/uam (Mesa GLSL → Maxwell)
```

Two builds share everything except the last stage:

- **headless** (`RL_NX_DEKO=0`, default): `AURORA_SURFACE_HEADLESS` + Dawn's Null device. The whole
  GX path runs and is counted, nothing is shown. This is the build used to prove guest/Aurora
  correctness, and the one that always has to stay green.
- **deko** (`RL_NX_DEKO=1`): adds `switch/deko`, `switch/uam`, patched `deko3d`, the deko-seamed
  Aurora (`build-switch/aurora-deko`) and `--wrap` for the Dawn entry points. This is the one that
  goes to the console.

`switch/builder/integrate-deko.py` stages the deko variant and applies its seams; `build.sh`
re-creates `build-switch/aurora-patched` and `build-switch/aurora-deko` from `extern/aurora` on
**every** run, so those two directories are transient — never edit them, edit the staging script or
`switch/deko` instead.

### Where our edits live

| Area | File(s) | Mark |
|---|---|---|
| Guest memory arena | `src/runtime/memory_switch.cpp`, `ctx.h`, `cpu.cpp`, `hle_game.cpp` | `// RL-NX:` |
| Entry point / boot | `switch/src/main_switch.cpp`, `boot_switch.cpp`, `runtime_switch.cpp` | `// RL-NX:` |
| GX bridge additions | `src/runtime/gfx/gx_bridge.cpp` (`switchInit`, diagnostics) | `// RL-NX:` |
| Audio | `switch/src/audio_switch.cpp` | `// RL-NX:` |
| deko seams | `switch/builder/integrate-deko.py`, `switch/deko/*`, `switch/cmake/Deko3D.cmake` | `RL-NX-DEKO` |
| Diagnostics | `switch/deko/backend_*.cpp`, `src/runtime/gfx/gx_bridge.cpp` | `R2_*` env switches |

Convention kept throughout: **comment everything added, prefix edits inside upstream files with
`// RL-NX:`,** and never put comments inside Aurora's WGSL raw strings (they are hashed into the
shader cache).

---

## 4. Decisions, and why

| Decision | Why |
|---|---|
| **deko3d from the start**, no GL stage | Aurora is already the seam and the skill's numbers (MKDD: 42 fps on GL, 59.9 on deko3d; app 10 MB smaller) make GL a detour. A deko seam existed in SFA-NX to adapt. |
| **Headless is the default build** | It is the only way to prove guest + Aurora correctness without a screen, and it stays green while the renderer is broken. `RL_NX_DEKO=1` is the advanced build. |
| **Aurora pinned to `encounter/aurora@f77492f` + Red Two's 33 patches** | Measured: 33/33 apply cleanly there, 13/33 on Foxhollow's vendored Aurora (20 conflicts, including graphics correctness patches). The 33 patches *are* the renderer for this game. |
| **Staged Aurora copies, not submodules** | Two variants (patched, deko-seamed) must coexist; staging by content also avoids `git apply`'s silent path resolution inside a parent repo. |
| **Compiler split: Clang 23 for anything with `ctx.h`, GCC for the rest** | devkitA64 GCC **ignores `__attribute__((naked))`**, which would silently break `rt_setjmp` and corrupt every guest thread, and rejects `__builtin_rotateleft32`. This is the skill's §3.2 rule, and here it is mandatory. So `main_switch.cpp` + `runtime_switch.cpp` are a separate `rl_entry` library built through a *target* launcher (a per-source `CXX_COMPILER_LAUNCHER` is not applied by this CMake). |
| **`TARGET_PC` defined on every TU that sees an Aurora header** | Without it Aurora's `dolphin/types.h` makes `s32`/`u32` 64-bit on AArch64, so struct layouts disagree with the Aurora we compiled. A latent ABI bug, not a compile error. |
| **256 MiB arena instead of the 4 GiB reservation + lazy page commit** | Horizon has no 4 GiB fixed mapping and no user-visible page faults. One contiguous arena covers MEM1 (24 MiB) plus the ~14 MiB past its end that Battle of Hoth really reads. |
| **Guest threads = fibers on one host thread** | Same as the console (single CPU) and what Horizon wants: it does not time-slice equal-priority threads. |
| **`boot.cfg` instead of env vars on the console** | An NRO has no shell. Every `R2_*` switch is read from `sdmc:/switch/RL-NX/boot.cfg` and each line is logged, so a log always says how the build was run. |
| **Capture instead of screenshot on emulators** | Ryujinx's layer owns the window's buffers, so deko3d's swapchain cannot be created there; the renderer runs without presenting and `COS_DK_CAPTURE=1` writes PPMs of the presented texture. |
| **All diagnostics are env-gated** | Normal runs must not change speed or behaviour; every probe is off unless `boot.cfg` asks for it. |

---

## 5. Trade-offs and compromises

- **Native 640×480 EFB upscaled to 720p**, not true 720p rendering. Correct for now (the game's own
  resolution), but it is not the skill's "720p handheld" bar; raising the internal resolution is a
  later, measurable step.
- **Memory is tight.** The deko build sits at ~2.05 GiB in use of ~2.79 GiB usable, image heap ~63 MiB,
  deko3d blocks ~940 MiB. To get there, buffers larger than 16 MiB keep **two** staging generations
  instead of four (18 buffers of 107 MiB had reached 1.9 GiB); small buffers keep four so uploads stay
  pipelined. That is a real throughput compromise made to fit.
- **Waiting is bounded, not correct.** Staging reuse waits at most 2 ms for a fence; waiting forever
  hung the game when the queue could not progress. Under a slow GPU this can reuse a buffer whose
  fence has not signalled (logged: `staging: reusing a generation whose fence has not signalled`).
- **No swapchain on emulators** (window binder answers `0xF59`). The renderer bails quietly and runs
  without presenting instead of dying inside `dkSwapchainCreate`, which means emulator testing must go
  through PPM captures. On hardware the swapchain is created and presents.
- **The console has no crash backtrace UI**: crashes are symbolized offline against
  `build-switch/rl-nx.elf` (regenerate `syms.tmp` after every rebuild).
- **Save states, achievements, gyro and the update checker are parked** on the Switch (`host_switch.cpp`
  provides no-ops / counts). A first release can ship without them; it must not ship crashing ones.
- **RVZ is supported but unmeasured on hardware.** Rogue Leader streams continuously and A57 zstd
  decode competes with the game thread; ISO/GCM is the recommended format until measured.
- **Docked mode is 720p scaled by the system** (`COS_DK_DOCKED_1080=0`): the window is 1280x720 in both
  modes. A real 1080p docked path is not implemented.

---

## 6. Where this deviated from the skill

Honest list, with what it costs:

1. **Bring-up order.** The skill's order is shell → first picture → gameplay → **exit/restart early** →
   long session → profile → features. We worked: shell → memory → recompiler → guest boot → Aurora →
   deko → audio/input/memory card, and have **not** done exit/restart or a long session. Every new
   thread, service or GPU block found later makes the exit path harder; this is now the largest
   process risk.
2. **No `scripts/check_log.py` / `ryujinx_smoke.py` run on log drops.** The log sent from the console
   was read by hand. The kit scripts are not wired for this repo yet, so boot crashes, GPFIFO overflow
   and leak signatures were found by reading, not by the checker.
3. **No `boot.txt`, no rotating log.** The build stamp is the first line of `logs/log.txt`, which is
   truncated each run (`O_TRUNC`) instead of appending to a capped, run-delimited file. A player's log
   therefore holds only the last run, and an old `.nro` on the SD card has to be spotted by eye.
4. **Crash reports are not imported.** Atmosphere's `crash_reports`/`fatal_reports` are not copied into
   `logs/` at the next start, so a crash usually arrives without its report.
5. **No shader cache in RomFS.** Shaders are compiled on the console and appended to
   `sdmc:/switch/RL-NX/Cache/deko/dksh_local.bin` (99 loaded on the reporting console). Seeding a
   merged cache inside the NRO is not done, so a new player compiles from cold.
6. **Updater not wired or verified**, and `switch/tools/hooks/pre-push` +
   `sync-upstream.sh` still describe Foxhollow's upstream (they would block a push to the port's repo).
7. **No profiling on hardware before optimising.** The memory cap and the "run without presenting" bail
   were made from failures, not from a profile; the skill says profile first. The hardware log now
   gives the first real numbers to start from (§7).
8. **No git repository yet** (by the user's instruction, commits only on request). `.gitignore` is
   complete and ready for the first `git init`.
9. **`switch/README.md` is partly stale** (it still says "there is no working build yet"). This file
   supersedes its status section; `switch/README.md` remains the layer's own notes on options and the
   black-screen hand-over.

---

## 7. Current status

Legend: **done** = verified in a log; **partial** = builds/runs but the exit test is not met;
**open** = not started.

| Step | State | Evidence |
|---|---|---|
| Survey + decisions | done | `docs/SWITCH_PORT.md` §A–C |
| Kit, `.gitignore`, licence notes | done | `switch/`, `switch/licenses/`, `.gitignore` |
| Host recompiler | done | `r2recomp` on the disc: 8104 functions, 527k instructions, 99 files / 37 MB, 9.4 s |
| Guest memory arena | done | 256 MiB at `0x2025A0000`, cached/uncached alias reads back the same word |
| Guest core builds for AArch64 | done | 19/27 runtime TUs compile; the rest are parked desktop files (`RL_RUNTIME_PARKED`) |
| NRO builds | done | current **40,706,902 B**; the first bring-up sizes were headless 60,029,125 B and deko 63,154,373 B; 0 errors |
| Guest boots the game | done | OS banner, VM init, `resident setup done 20485888 bytes free`, 59.9 fps |
| Aurora runs the GX | done | `1400–1700 draws a frame`, 5000+ merged, pipelines created |
| deko3d init + swapchain | done | hardware: 3×1280x720 RGBA8, `swap interval 1`, `first game snapshot: 640x480, native present` |
| **First picture** | done | the 640x30 band is fixed (8.1); menus draw once (8.7), the movies and the mission's world render (8.8-8.9) |
| Audio | done (emulation caveat) | `audout up: 48000 Hz`, 0 dropped, 32 kHz arriving; AI cadence under emulation is 5–30 ms late (§8.4) |
| Input | partial | pad scan on its own thread, game reads it; ZL/ZR → L/R and the on-screen button-prompt labels are unverified |
| Memory card | done | created + formatted, the game walks past the format prompt |
| Performance | **partial** | in-level 47.35 ms a frame (~21 fps) from the first honest profile: guest 25.6 ms, port 17.4 ms (7.1, PERF_HANDOFF 9.44) |
| Optimisation, phases 0-1 | **partial** | six measured changes kept, three codegen experiments measured 0 and parked (7.1) |
| Correctness gate (oracle) | done | interpreter vs recompiled from identical bytes: 6/6 `ok`, fault control fails 4/4 (9.48) |
| Exit / restart ×3 | open | not attempted |
| 30 min hardware session | open | not attempted |
| Profile on hardware | done | `R2_PROFILE=<s>` + `switch/tools/prof_report.py`; a 200 s in-level report, 2.5 % of samples dropped (8.2) |
| `-` in-screen menu | **partial** | on in build 18:45: tap Minus, d-pad, A; opens in Ryujinx; not yet seen on a console screen (7.2) |
| Widescreen / docked 1080p / updater / shader seed | open | - |

### 7.1 Optimisation: where it stands (2026-11-26)

The goal is a locked 60 on stock clocks. The first honest in-level profile says what that costs:
**47.35 ms a frame (~21 fps)** in the heavy levels - 17-19 fps in the `[perf]` windows of the same
run - of which **25.61 ms (54.1 %) is recompiled guest code** and **17.38 ms (36.7 %) is the port**
(`docs/PERF_HANDOFF.md` 9.44: 80 s, 1,686 frames, 2.5 % of samples dropped, 0 audio underruns). The
arithmetic is unforgiving: remove the port and the guest alone runs ~39 fps; remove the guest and the
port alone runs ~57 fps. **60 fps is not reachable on this codegen** - the ceiling is ~39 fps - and a
locked 30 is: the plan's four moves (no texture hashing, an audio thread, a replay cache, 25-30 % off
the guest) land at ~32.5 ms.

Measured on the console and kept, each in the tree with its switch documented in section 10:

| Change | Effect | Handoff |
|---|---|---|
| Aurora threads placed one per core instead of sharing a mask | guest `0x2`, render `0x4`, FIFO `0x1`, no contention | 9.35 |
| `R2_STATS`'s per-draw bounds measurement made opt-in | **6.7 ms a frame** (9 % of the guest thread) back | 9.35 |
| Guest accessors: one arena test, one out-of-line slow path | expansion **35.7x -> 20.6x**, NRO **-37.4 %** | 9.37 |
| `g_ram` given hidden visibility | one GOT load removed from every guest load and store | 9.38 |
| Texture identity cache: fingerprint + round-robin net | XXH3 stopped running on every bind | 9.34 |
| GX invalidate no longer forces a full hash | the cache stops degenerating to every-frame hashing (phase 1b) | 9.49 |
| `-O3` for the generated guest code (`RL_NX_GAME_OPT`) | shipped; speed unmeasured, NRO +1.9 % | 9.36 |

Tried and measured **zero**, and not coming back: hot-call clustering with `__attribute__((flatten))`
(0 to -3 % on hardware, 9.40-9.43), the raised `-inline-threshold` it needed (dead code, 9.46), the
recompiler's locals emitter (plausible code, but two builds never submitted a draw: parked, 9.45),
and whole-game determinism (a frame-driven guest clock deadlocks the boot, 9.47).

What the work needed instead was a **gate**. A run-to-run state hash cannot compare two builds - the
same NRO hashes differently at the same frame index because the guest's timeline is wall clock
(9.46) - so the gate is an **oracle test**: the interpreter and the recompiled body run on one
function from identical bytes with the devices muted, and their hashes must match. On a probe build
(`RL_NX_ORACLE=1`) `R2_ORACLE=<n>` gives 6/6 `ok`, and the control (`R2_ORACLE_FAULT=1`, one bit
flipped in the recompiled result) reports `MISMATCH` 4/4 while the interpreter's own hashes stay
byte-identical across runs (9.48). Emitter and LLVM-backend work is gated on it from now on.

**Open, in order** (`docs/PERF_PLAN.md` phase 1): the in-level reading of 1b - `[stats]` should now
read `hashed N of M` closer to the net, and `hashed KiB first/net/probe/copy` says where any rest
goes, because the fix is only verified in the attract scene (9.49); audio off the guest thread
(~3.4 ms); a replay cache for `execute`/`cmdLength`/`cpRun` (~2.3 ms); then phase 2 (a native
GX -> deko3d path) and phase 3 (an LLVM backend) for the guest's 25.6 ms. None of this can be timed
in Ryujinx - the emulator proves correctness only - so each step is a console run.

### 7.2 The `-` in-screen menu (2026-11-25, build 18:45): on, hardware unconfirmed

There is no desktop UI on this build (`gfx/menu.cpp` is parked in `switch/CMakeLists.txt`) and no
ImGui on the deko path, so the menu is a small panel drawn by the renderer's own 3x5 pixel font
(`switch/deko/pattern.cpp`, `swdk::draw_text` / `swdk::draw_rect`) over the game's picture, in the
same swapchain pass and after the `R2_FPS` counter, which uses the same text pass and is proven on
hardware:

| Row | What it switches | Starts from |
|---|---|---|
| FRAME RATE | the top-middle `FPS n` counter (frames actually presented, `swdk::overlay_fps_set`) | `R2_FPS` (on unless 0) |
| STATS LOG | the `[stats]` per-frame counters (`rt::gfx::setStatsEnabled`, `gx_bridge.cpp`) | `R2_STATS` |
| PROFILER | the 1 kHz sampler (`rlProfSetEnabled`) | `rlProfEnabled()` |

**Tap Minus** (press and release with no other button) to open or close it; the d-pad moves, A or
right switches a row. Minus held with another button is still a save-state hotkey (Minus + RB / LB /
d-pad) and does not open the panel. While it is open `rt::gfx::menuOpen()` is true: `input.cpp` hands
the game idle controllers and the hotkeys are off, so no press leaks into play.

How it is wired (build 18:45):

- **Input**: `hotkeys::tick()` makes one SDL controller query a vblank and gives the same set to the
  hotkeys and to `pollMenuPad()`, which latches edges; `menuDraw()` (guest thread, `endFrame`)
  consumes them. Latching matters: tick runs once a vblank and the menu once a game frame, so at
  20-30 fps an unlatched edge was lost two times in three.
- **Drawing**: `rl_nx_menu_draw()` in `switch/deko/device.cpp` right after `picture(user, 1)`. Stage 2
  records nothing for the game picture (`backend_present.cpp` returns for every stage but 1), so this
  is the same as drawing after it. It records nothing while the panel is closed, and restores the
  frame's viewport when it did draw. The state it reads is atomic (it runs on the render worker).
- **Rows** start from the switch's real state; the old panel started every row at OFF. `STATS LOG`
  can now turn the lines off on a card whose `boot.cfg` sets `R2_STATS=1`
  (`setStatsEnabled` no longer ORs the env flag in).
- The panel's own guest-frame counter is gone: the FRAME RATE row drives the existing present-rate
  counter instead of drawing a second, less honest one.

Bugs found and fixed while finishing it: `R2_MENU_OPEN` was read at static initialisation, before
`boot.cfg` reaches the environment, so it never worked; the panel opened on every save-state combo;
presses were lost between game frames; the rows lied about their state.

**The stall question is answered: the menu is innocent.** In two 60 s Ryujinx smokes of build 18:42/18:45
the first `[stall] guest quiet` came at 10.3 s and the panel opened (`R2_MENU_OPEN`) at 12.3 s, and the
run in which the panel never opened (18:42, the static-init bug) stalled at the same points
(10.3, 17.0, 25.7 s). The 4 s gaps are the boot and attract sequence's own black stretches.

**Verified:** builds with 0 errors (NRO 40,706,902 B, `debug/RL-NX-1845.elf`); in Ryujinx the panel
opens at start and nothing faults. **Not verified:** the picture (Ryujinx never reaches
`frame_present`, so only a hardware `COS_DK_CAPTURE=1` swapchain PPM or the screen shows it), and the
Minus tap / d-pad / A path by hand. Switches for a bisect if it misbehaves on the console:
`R2_MENU_DRAW=0` (no drawing), `R2_MENU_PAD=0` (no menu input).

Next rows, in order: the texture-hash A/B (`R2_TEX_HASH_EVERY`), the oracle status, thread
placement (`perf.cfg`).

---|---|
| FRAME RATE | the panel's own 0.5 s presentation counter, top-left (`FPS n`) |
| STATS LOG | the `[stats]` per-frame counters (`rt::gfx::setStatsEnabled`, `gx_bridge.cpp`) |
| PROFILER | the 1 kHz sampler (`rlProfSetEnabled`) |

Minus opens and closes it (the d-pad moves, A or right switches a row), and while it is open
`rt::gfx::menuOpen()` is true, which makes `input.cpp` hand the game idle controllers and the hotkey
layer mute its own actions, so no press leaks into play.

Code: `switch/src/menu_switch.cpp` (panel, state, input, counter), the hook in
`switch/deko/device.cpp` after `picture(...)`, and `R2_MENU_OPEN=1` to open the panel on the first
frame for an automated run.

**State: it compiles and links into the NRO, and it is switched off.** Two deliberate stops:

- the draw call is `if (false) rl_nx_menu_draw();` in `device.cpp` - the panel has never been seen
  on screen, so nothing about its layout is verified;
- `hotkeys::pollMenuPad()` is not called from `hotkeys::tick()` (commented out there), so
  `padEdge()` has no producer and the panel cannot even be opened by hand.

Both come from one unresolved measurement. The first version polled a second libnx `PadState` from
the present path and the guest stalled - `[stall] guest quiet: no Aurora draws for 4 s`, twice in
one run - so input moved to SDL edges (`hotkeys::padEdge`, the path the save-state hotkeys already
use). The stalls did not go away, but they are not attributable to the menu either: they survived
both revert paths, and smokes still end with `aurora: 1 draws` plus `[stall] guest quiet` lines,
which the attract scene's own black transitions can produce as well. Until one run separates the
two, the feature stays off rather than risk a stall in a play session.

**To finish it** (step by step in `AGENTS.md` section 4): re-enable the draw call and give the edges
one shared SDL query per frame; then run one smoke with each half compiled in and one with it out and
compare the `[frame]` draw series and the `[stall]` counts; confirm the panel on hardware or in a
`COS_DK_CAPTURE` frame. Then the rows the port actually needs: the texture-hash A/B
(`R2_TEX_HASH_EVERY`), the oracle status, thread placement, and the decision about the `R2_FPS`
counter it currently duplicates.

---

## 8. Known issues

### 8.1 The picture was a 640×30 band — fixed 2026-11-23 (build 08:38), hardware unconfirmed

**Cause:** the deko present showed the **EFB**, not the composed XFB. Rogue Leader display-copies its
frame into the XFB **with clear** (in two half passes in 3D, `0,16 640x242` and `0,238 640x242`), so by
the end of the frame the EFB holds only the strip neither copy cleared: the band. The geometry,
scissors, viewport and shaders were all right (the measurements that ruled them out still stand).

**Fix:** `switch/builder/integrate-deko.py` captures `xfb_present_source()` into the frame packet at
`end_recording()` (`FramePacket::dekoPresent`) and `frame.cpp` hands that texture to
`kartpad_deko_present()`; the EFB is the fallback only when there is no XFB (`R2_NO_XFB=1`). The log
says `[dk] present from XFB 640x450`. Ryujinx capture: the full legal screen at 640×450.

### 8.2 Present rate and frame time on hardware

Hardware (build 06:54): ≈15 presents/s (later ~6), stalls up to 2 s, **game thread busy 92–100 %**,
while deko itself costs ~1.5 ms per present (fence 0.04, record 1.05, submit 0.25). So the limit is
**the game thread (CPU)**, not the GPU. Two causes found, both fixed in build 08:38, both unmeasured
on hardware:

- **No thread was placed.** Aurora's placement code is Linux/Windows only, `melee_nx_perf_init()` was
  never called, and the only `THREAD` line was the deko worker: the game thread shared a core with
  Aurora's render worker. Now Aurora's `thread::set_current` calls `melee_nx_perf_thread()` (deko seam)
  and `main` calls `melee_nx_perf_init()`: policy `tuned` puts `Main thread` alone on core 1 and the
  render worker on 0/2 (`THREAD ... actual` lines in the log).
- **The profiler could not run** (nothing called it, frame marks missing). `boot.cfg R2_PROFILE=60`
  now samples for 60 s and writes `logs/profile-NNN.txt`; `gx_bridge.cpp endFrame()` marks the frame
  phases. Read it with `py -3 switch/tools/prof_report.py <profile> --elf debug/RL-NX-HHMM.elf`.

**Hardware profile (build 08:56, `E:/RL-NXLOGS/LOGS 11232026-0910/profile-002.txt`, title/attract):**
97 ms of game thread per game frame (~10 fps), and the profile is flat. By category: recompiled game code
60.6 ms (62 %), kernel waits and service calls 9.9 ms (disc reads, `audout`), GX bridge 8.4 ms, Aurora
command processing (inline) 7.6 ms, other runtime 6.0 ms, MusyX/DSP 2.9 ms. The render worker and the
shader compiler are idle more than 95 % of the time. The disassembly of the hottest function shows about
20 instructions per guest store: the `R2_WATCH` flag, the RAM base and the guest registers are reloaded
after every store, because a store through a byte pointer may alias anything.

**Changes in build 09:25 (unmeasured on hardware, each switchable):**

- `switch/tools/patch-gen-restrict.py` (run by `build.sh`) marks the context of each recompiled function
  `Ctx& __restrict c`, so guest stores no longer force register reloads. This is safe for calls: every
  function passes `c` to `traceEnter` on entry, so Clang still assumes out-of-line calls (page-fault
  handler, I/O, safepoints) can touch it. The one setjmp function is skipped. `RL_NX_NO_RESTRICT=1
  switch/build.sh` builds without it.
- `ctx.h`: the `R2_WATCH` store check is compiled out on the Switch (`-DRL_NX_WATCH` restores it).
- GX processing runs on Aurora's FIFO thread by default, placed on core 0 by the tuned policy (the log
  says `GX processing: ...`). `boot.cfg R2_GX_THREAD=0` puts it back inline.

Earlier note: a hardware profile, in a mission, decides the next fix (likely candidates from the Ryujinx
profile: `svcSendSyncRequest` = SD/service I/O on the game thread, the GX command decode
`onCommand`/`physPtr`, the MusyX DSP mail path). (Since 09:25 the GX thread is the default; `R2_GX_THREAD=0` is the inline A/B. It moves Aurora's processing, not the bridge decode,
off the game thread.)

### 8.3 Emulator-only oddities (not port bugs)

- Ryujinx cannot take deko3d's swapchain (`nwindowSetDimensions` answers `0xF59`), so the renderer runs
  without presenting there and output is seen only through `COS_DK_CAPTURE=1`.
- Ryujinx's VI presents at 1–15 images/s while the game runs 60 fps; do not read that as the port's
  frame rate.

### 8.4 Audio cadence under emulation

With `R2_AI_LOG=1` every AI boundary is 5–30 ms late (1029 of them in 30 s) and the stream arrives
16–20 % short, so the device starves and output is gappy. The resampler measures the *arriving* rate
instead of trusting the DMA registers (clamped ±25 %, smoothed). Ryujinx distorts that measurement, so
this needs one hardware run before it is called right.

### 8.5 Others

- **Memory headroom** is ~740 MiB unused after setup; large frames (3500+ draws) and new renderer
  features will eat into it. The 2-generation cap for >16 MiB buffers is a throughput compromise.
- **`staging: reusing a generation whose fence has not signalled`** appears under load; correctness is
  untested under sustained slowness.
- **Save states / achievements / gyro** are no-ops on the Switch; a menu item that calls them must not
  be offered.
- **`RL_NX_DEKO_DEBUG=1`** (checked deko3d, `libdeko3dd`) hangs after the swapchain skip. Use it only
  for a single failing call, not for a session.
- **Tooling papercuts** that cost time and are worth fixing once: symbolizing needs
  `syms.tmp` regenerated per build; the build regenerates the Aurora stagings every run (~5 min).

### 8.6 No controller input on hardware — fixed 2026-11-23 (build 08:56), confirmed on hardware

**Symptom (hardware, build 08:38, `E:/RL-NXLOGS/LOGS 11232026-0847`):** the title screen ignored every
button, and the log had no `input` line at all.

**Cause:** `aurora::update()` calls `gamepad::initialize()` (`SDL_Init(JOYSTICK | GAMEPAD | HAPTIC |
SENSOR)`) only when the surface is not `AURORA_SURFACE_HEADLESS`. The console build is headless, so SDL's
Switch joystick driver was never started and Aurora's PAD never saw a pad. `logPort1()` was silent
because it only logged changes from "none".

**Fix:** `switch/tools/patch-aurora-switch-input.py` (run by `build.sh` while staging
`aurora-patched`, and part of the staging stamp) starts the controllers under `__SWITCH__` whatever the
surface. `SDL_INIT_JOYSTICK` also brings up `SDL_INIT_EVENTS`, so `window::poll_events()` delivers the
gamepad-added events. `input.cpp logPort1()` now logs the starting state too.

**Ryujinx (08:56):** `Added controller 'Nintendo Switch Controller'`, `port 1 controller: 'Nintendo
Switch Controller'`, and `R2_PAD_LOG=1` shows port 0 `err 0` (was none). Button presses are unchecked
(nobody pressed one during the smoke test).

**Hardware (08:56, `E:/RL-NXLOGS/LOGS 11232026-0910`):** the controller works (the user's report);
`port 1 controller: 'Nintendo Switch Controller'` at 0.6 s.

### 8.7 Menus drawn twice — real fix in build 10:38, unconfirmed on hardware

**Symptom (hardware 08:38 and 08:56):** the title screen (Start / Options) shows the whole picture once
in the top half and again in the bottom half.

**What it is not:** the XFB composition. Ryujinx (08:56, `R2_COPY_LOG=1`) shows the two-pass 3D frames of
the attract demo copied to lines 0 and 224 and composed correctly.

**Cause (most likely, not yet seen in a log):** `gx_bridge.cpp fillScreen()` sent Aurora a pass shift of
0. Draws inside `hle_CutsceneVideoDraw` (movie/backdrop quads) are measured by Aurora *less the NDC shift
the scissor box offset put into the projection*, so it can tell whether the quad covers the whole frame.
The shift was dropped during the viewport-translation experiment and never restored when the projection
shift came back. In the bottom pass (box offset 238) the quad's edges then stop inside the band, so Aurora
stretched it to fill that band, which put the full picture in each half.

**Fix:** `fillScreen()` sends the same NDC shift `sendProjection()` adds (`-offset / viewport scale`, per
axis), and logs `fill-screen draw in a pass with box offset ...` the first four times. If the title is
still doubled and that line is absent, the title does not draw through this path; then capture it with
`R2_COPY_LOG=1`.

**Hardware 10:33 (build 09:25):** still doubled, and also on Options, the mode select and the results
screen, although the fill-screen line was logged. The user's screenshots settle it: the 3D backdrop
continues across the halves, but the 2D text repeats 238 EFB lines lower. So the 2D (orthographic)
draws of the bottom pass carry no box-offset shift. The fill-screen fix above was correct but not the
cause.

**Real cause:** the game's own XF projection/viewport loads (`gx_bridge.cpp`, XF register load case) were
forwarded unchanged, and the shifted projection was sent only when the box offset *changed*. The attract
scenes load their projection before setting the offset; menus and the HUD load their 2D projection after it.
Probably also why the player's craft was missing in mission 2: a projection loaded mid-pass puts it
outside the pass's band.

**Fix (10:38):** after forwarding an XF load that touches 0x101A-0x1026 while a nonzero offset is active, the
bridge calls `sendProjection()` again. `R2_NO_PROJ_REFOLD=1` restores the old behaviour. Ryujinx 10:38:
a two-pass menu frame (frame 501, copies to lines 0 and 224) shows its 2D sprite emblem continuous through
both halves.

**Hardware (user, after 10:38):** the title is no longer doubled. That leads to the next issue, 8.8.

### 8.8 Movies and CPU-drawn images missing (loading screens, title backdrop) — fix in build 11:18, unconfirmed on hardware

**Symptom:** intro and loading movies don't play. The title shows only START / OPTIONS on black. Dolphin
shows a tiled metal backdrop, an R2-D2 movie and the logo behind them.

**Cause:** the EFB-copy tracking in `gx_bridge.cpp` (`CopyDest`, `prepareTextures`). Here copies live only on
the GPU, so guest memory never changes because of a copy. When memory at a copy's address changes, the
CPU wrote a new image there. The bridge dropped the copy only if the new texture's size or texel depth
differed. A same-size image kept the old copy, which was black or stale. The hardware 10:16 log shows this
at every loading screen: `texture at 00CDF7A0 (640x480, 16 bits): memory changed since an EFB copy
(640x480, 16 bits) went there: still the copy`, repeated as each movie frame is decoded into that buffer.

**Fix (11:18):** any change to the memory drops the copy, as Dolphin does. Display copy (XFB) destinations
are the one exception: they still keep their copy. The hash now covers only the last copy's bytes, not the
largest copy ever made there. `R2_COPY_KEEP_SAME=1` brings back the old rule.

### 8.9 Locked-cache loads and stores went to the wrong place — fixed 2026-11-24, unconfirmed on hardware

**Symptom:** the pattern behind 8.8 (and docs/PERF_HANDOFF.md §9): art built at run time is drawn with a zero
matrix or not at all, while static art renders. Title backdrop and R2 movie missing, Select Game's rebel
background and spacecraft missing, movie strips invisible with 0 lit pixels.

**Cause:** `ctx.h`. On the Switch `hp()` maps the locked cache (`0xE0000000-0xE03FFF`) to its own buffer, but
`hpr()`/`hpw()` — every recompiled load and store — fell through to `memPtr`, which on the console is the
arena: `0xE0000000 + off` became `g_ram + 0x20000000`, i.e. **512 MiB into a 256 MiB arena**. The game stages
XF matrix and display-list data in the locked cache every frame, so those stores went into unrelated heap and
read back as zeros, and `lockedCacheDma` (which uses `hp()`) copied the still-zeroed cache into the object's
matrix array in main memory. The PC build keeps the cache inside its 4 GiB `memPtr` reservation, so the two
paths agreed there and the bug could not show — which is why the same recompiled code renders correctly on PC.
This also explains why a day of `R2_WATCH`/`ATS`-style store watches on that matrix array found no writer: the
stores never reached guest RAM.

**Fix:** `hpr`/`hpw` now return `hp(a)`. Nothing else changes; the PC maps are untouched.

**Ryujinx (11-24, 300 s boot):** no fatal, 60 fps, and `COS_DK_CAPTURE=1` reads `lit=287600..288000/288000`
through the late intro where the broken build measured 3221/288000 (title) and 0 (transition). The LucasArts
logo, the full clone army, the X-wing and the Death Star explosion with the ship all render now — every one of
them was in §8.8's missing list. Hardware still to confirm.

### 8.10 In-level flicker of the world — open 2026-11-24, nothing changed for it yet

**Symptom:** in a mission the buildings on the surface and the other craft flicker out and back; HUD,
starfield and the engine glows stay.

**State:** not fixed, and no fix is in the tree. docs/PERF_HANDOFF.md §9.20 has the analysis: the level
rejects nothing (0 skipped, no dropped copies, no buffer overflow), the scissor box offset is `0,0` by the
time the level starts, and the 12:56 profiler is the intro, not the level. Two measurements were added to the
`R2_STATS` line and they describe the level: it submits 5600-9000 draws, 800-3000 of them indexed XF loads,
and leaves 0.9-1.2 MiB of command stream undecoded at frame end, where menus and cutscenes have zero of both.
Snapshotted indexed XF loads (a fix candidate for that backlog) produced pixel-identical frames in an A/B, so
it was reverted; the backlog is not corrupting those loads.

**Next:** `R2_STATS=1 R2_DRAW_BOUNDS=1` and a level, to see the scissor and the box of the largest draw it
rejects - half of a level frame's draws measure wholly outside it, which is the remaining lead.

**On video (2026-11-24, `RL_Flicker1/2.mp4`):** the missing frames lose the *whole 3D pass* - Flicker1 frame
36 has the starfield, the HUD and the engine-glow sprites but no X-wing hull, no trench surface and no TIE
fighters, while frame 578 has all of it. The survivors are exactly what does not depend on the depth test
(2D HUD, particles, the far-plane starfield), so the lead is now the depth buffer across the two half-frame
passes, not transforms or submission. A level frame is two EFB passes (`scissor box offset 0,238` and `0,0`)
composited into one 640x450 XFB in some missions, and in others one full-height `640x450` copy from EFB
`y=16..466` per frame - the layout is the scene's, not the port's (9.26 has both from the same session). A
display copy clears depth only when its rect is the whole target (`resume_pass_after_copy`), so in practice
both layouts clear by a scissored clear draw. See `docs/PERF_HANDOFF.md` 9.23 for the frames, the emulator
check and the test below.

**Per-half depth clear, logged (2026-11-24):** `R2_DEPTH_LOG=1` in `boot.cfg` makes
`resume_pass_after_copy` (`extern/aurora/lib/gfx/recording.cpp`) print one `[depthclear]` line per pass a copy
ends - i.e. one per half frame - naming the frame, the half (0 = top, 1 = bottom, measured from the XFB line
the copy lands on), the copy rect and target, how many draws that half had, what the game asked to clear, and
what the pass will actually do: `passDepthClear` (the depth attachment's own `LoadOp::Clear`), `depthDraw` (a
scissored clear draw inside the pass) and the two colour equivalents. A half that loses its depth clear then
reads `req depth=true -> passDepthClear=false depthDraw=false` in the console log by itself, with no
`zalways` A/B needed. Verified on a Ryujinx level run (497 lines, frames 1-283): every half reports
`passDepthClear=false depthDraw=true`, because a 640x226 copy never covers the 640x480 target, so the clear
is always the scissored draw; the level's halves carried 638-784 draws each. `docs/PERF_HANDOFF.md` 9.24.

**Hardware log, 17:16 (`LOGS 11242026-1716`):** 14,940 `[depthclear]` lines over 203 s, level included
(first bottom-half copy at frame 374). **No half loses its clear.** All 10,881 depth requests are satisfied -
164 by the attachment's `LoadOp::Clear` (every one a `full=true` copy) and the rest by the scissored clear draw
inside the new pass; not one line reads `req depth=true -> passDepthClear=false depthDraw=false`, and no level
half reads `req depth=false`. That rules the depth-clear hypothesis out for this run: `zalways` would relax a depth test that is not looking at stale depth. (It stays useful as a test of the compare instead - see below.)

**Run 1 on the console, 08:43 (`LOGS 11252026-0843`): the world still vanishes, and no dump caught it.** All 24
break pairs the run wrote are the attract mode (frames 15-1740) - the budget was spent before the level loaded -
and of the 544 verdicts written for frames with a >=1000-draw pass, the only four that mention lit pixels are
frames whose lit count was 0. `break_lit` counts *non-zero* pixels and the trench is lit edge to edge, so a
frame that loses the world there still reads 230400 of 230400: the trigger was blind in exactly the scene the
flicker happens in. Replaced by a one-frame change trigger: each presented frame is reduced to a 64x36 luma grid
and its difference from the frame before is compared with the difference of the frame before that; a jump of
`R2_FLICKER_THRESH`/255 (default 20) that is also >=2.5x is a flicker edge and writes the matched pair
`flk-before-NNNN.ppm` / `flk-after-NNNN.ppm`, each with its own depth PGM. `R2_CAPTURE_MIN_DRAWS=1000` keeps the
attract mode (632-704 draws a pass) out of the budget and the trench (1,100-1,900) in it; `R2_CAPTURE_GAP=0` and
`R2_CAPTURE_DUMPS=30` keep every frame of a run. Ryujinx exercised the pair, its depth and the no-gap path
(forced with `R2_FLICKER_THRESH=0`); the console boot is `build-switch/sdcard/switch/RL-NX/boot.cfg` (NRO built
09:02). `docs/PERF_HANDOFF.md` 9.28 has the run-1 numbers and the three questions run 2 has to answer.

**Run 2 on the console, 09:24 (`LOGS 11252026-0924`): the pair exists, and the depth says the loss is not a
rejection.** The change trigger caught the trench - three matched pairs (5669, 5700, 5712) at 1,006-1,047 draws
a top pass, each a 36-41/255 jump from a 1-3/255 frame - and the dumps are unambiguous: `flk-before-5699` is the
starfield only (mean luma 10.55) and `flk-after-5700` is the whole trench (51.81). The depth beside them is the
finding: `depth-5699` holds **two values**, `0x80` over the world's rect and `0` below it, while `depth-5700`
holds 256 - the world. So a world-less frame does not hold a world that a depth test rejected (that would leave
the cleared buffer) and it is not a frame of skipped draws either: 5699 and 5700 have the same 6 passes, the same
`2 submits, 1 split`, the same targets, the same family split and the same 760 KiB of copies. The run also says
the artifact is not a 1-8 frame flicker here: of the 48 frames dumped between 5668 and 5717 only 5696, 5700 and
5712 have the world, i.e. ~3.5 s at a time with the world switched off and single-frame returns. One break frame
(5670) is a second mode - a real ground-plane depth gradient with a world-less picture, so there the geometry
rasterised into depth and only the colour is missing. Run 3's boot adds `R2_CLEAR_LOG=1` (one line per frame for
the game's own depth-clear quad, whose z was in no log before) and puts a per-frame depth signature on the
`picture:` line, so every frame of the run says whether its depth had a world. NRO built 09:42.
`docs/PERF_HANDOFF.md` 9.29 has the numbers.

**Run 3 (`LOGS 11252026-1001`) is where that "0.502" was decoded, and it was wrong.** That log came from the
09:02 NRO, so it has neither the clear log nor the depth signature - but its dumps still held the value, and the
EFB's depth target is **Depth32Float** (`switch/deko/aurora/dk_objects.cpp` maps it to `DkImageFormat_ZF32`),
while the readback took byte 2 of each 4-byte pixel as if it were the top byte of a Z24. Byte 2 of `1.0f` is
`0x80`. So the world-less frames' depth is **1.0 over the whole 640x450 picture** (rows 16-465), not 0.5 in the
middle of the range: the far plane everywhere, which rejects every depth-tested draw while the undepth-tested
stars, sprites and HUD still paint - the flicker's picture exactly, and the reason the draw counts and `lit`
counts never move. It is not a clear either: with `R2_FRAME_DEPTH_CLEAR=all` and `R2_DEPTH_CLEAR_AFTER_CLEAR`
the patch re-clears the whole image to 0.0 after the game's clear quad (`scissor 0,16 640x450, viewport 640x480`,
logged), yet the picture rect still ends at 1.0 while rows 0-15 and 466-479 are 0.0 - which a whole-image clear
cannot leave behind. **A draw scissored to the picture writes it.** The pass's own clear value is 0.000 in every
frame of the run. Run 4's boot adds `R2_ZFREEZE_LOG=1` (zfreeze is the only thing in the pipeline that can write
one constant depth across a quad - `clamp(plane(x, y), 0, 1)` - and a saturated plane is exactly this) and
`R2_SKIP_DRAW_RANGE=a:b` to bisect the writer over a few launches. Run 4's NRO is the 10:18 build (64,045,910
bytes) with the 33-line `boot.cfg` (md5 `1a45bc081190c248b8947c1f14e9bfbd`). `docs/PERF_HANDOFF.md` 9.30 has the
numbers, including what the emulator did and did not exercise.

**Run 4 (`LOGS 11252026-1025`) corrected 9.30 and found two things.** The EFB depth binds as **Z24** (the
signature now prints `depth(z24)`), so the constant really is **0.502 = 0x800000**, held over **rows 0-263** and
`0` over 264-479 - a row band, not a rect, and 9.30's "1.0" was a misreading (corrected there). And
`R2_ZFREEZE_LOG=1` shows the zfreeze reference plane is **not a plane**: `-3.19e12 -1.44e29 5.47e35`, then
`-5.55e32 -inf inf`, refit constantly (gen 3801, 3805, 3807…). A slope of 1e29 a pixel clamps to 1.0 or NaN over
the whole viewport, which rejects every depth-tested draw behind it while stars, sprites and HUD still paint -
the flicker's picture. **A plain wrong-number bug of exactly the locked-cache kind.** Run 5 therefore ships
`R2_ZFREEZE_GUARD=1` (reject a fit that is not a depth function; write the flat far depth 0.0 instead), prints
**every** depth-clear quad of a frame with its target (the old log printed one, and a level frame has three, so
the world's own quad was never seen), and runs `R2_PROFILE=900`.

**The black screen after level 1's first phase is a hang, and it is now instrumented.** The guest's last real frame
is 4,775; after that the port presents 59.9 fps for another 29 s (18,240 presents) with `aurora: 1 draws` per 120
frames, audio alive, no fatal. The guest is blocked or looping, not dead. `R2_PROFILE=<seconds>` starts the port's
1 kHz pc/lr sampler from boot and writes `logs/profile-NNN.txt` every two minutes;
`py -3 switch/tools/prof_report.py "logs/profile-*.txt" debug/RL-NX-HHMM.elf --top 12` names the guest function
it is sitting in (it resolved `svcSignalProcessWideKey`, `rt::…` and even `fsFileRead` callers on an emulator
run). NRO built 10:38, 64,050,006 bytes, `boot.cfg` 33 lines md5 `5e764e56f0587e8ea4e607580dea95ad`.
`docs/PERF_HANDOFF.md` 9.31.

**The flicker is fixed (run 5, `LOGS 11252026-1104`).** The zfreeze guard rejected 11 fits (`vertex off the screen or
w near 0`, `degenerate reference triangle`) and replaced them with flat far depth, and the world never left the frame
again. `R2_ZFREEZE_GUARD=1` is the fix; it stays.

**The black screen is now measured, not waited for, and it is bisected to the 10:18 build.** Across every log on disk
the sessions on builds up to 09:02 (528 s and 327 s of level 1 at 1,380-1,420 draws a frame) never produced a frame
with no lit pixels; 10:18 and 10:38 each end with one, then the guest submits nothing for 28 s and 54 s while the port
presents 59.9 fps and audio keeps running. The boot configs differ only by two log flags, and diffing the archived
`debug/RL-NX-0902.elf` and `debug/RL-NX-1018.elf` string literals shows the code delta between those builds is
diagnostics only - which does not explain a guest stall, so run 6 measures the stall itself rather than guessing.
`switch/src/host_switch.cpp`'s frame hook now watches Aurora's draw-call total and, after 4 s with no new draw call,
logs `stall guest quiet: no Aurora draws for 4 s ...` and takes a profiler report on the spot (`rlProfDumpNow()` in
`switch/src/prof_switch.cpp`), then one every 30 s of continued quiet, and `stall guest drawing again after N s quiet`
if it recovers - so a load is reported as a load and a hang is captured in the second it happens. NRO built 11:16,
64,050,006 bytes, `boot.cfg` 26 lines md5 `37c2aa1353d014938273d9bc40434736`. `docs/PERF_HANDOFF.md` 9.32.

**Run 6 (`LOGS 11252026-1130`): the black screen did not happen, and the depth probes are the leading suspect.** The
only boot.cfg change since the hang was dropping `R2_DEPTH_SHOT`, `R2_FRAME_DEPTH_CLEAR` and
`R2_DEPTH_CLEAR_AFTER_CLEAR`, and the guest never stopped submitting in 268 s; three frames still went dark without
the guest stopping, so a dark frame is a symptom, not the trigger. One run, so a correlation, not a verdict.

**The on-screen frame counter** (top middle, `FPS 14`, `R2_FPS=0` hides it) counts **real presentations**, not the
guest's retraces. That distinction is the whole performance story: the guest says 59.9 fps in level 1 while the port
presents **10-17 frames a second**. The render worker's own numbers say why - `record 12.73 ms`, `submit+present
0.23 ms`, `frame fence 0.00 ms` - so the GPU has headroom and the port is CPU-bound, with the **guest's own thread at
100% busy** (`game 75.2 ms/frame`). The hotspots are almost entirely the recompiled game (`drawBoundsEfb` 2.4%,
`onCommand` 1.6%, `musyxDspMail` 1.0%); the biggest port-side entry is the texture cache's XXH3 hash at 0.7%
(~1.9 ms a frame). Threads are separate and mostly well placed: guest on core 1, the render worker and the GX FIFO
processor sharing mask `0x5` (cores 0 and 2), and the profiler's sampler pinned to core 2 - one of the two the
renderer may use. NRO built 11:35, 64,050,006 bytes, `boot.cfg` the minimal performance pack (17 lines, md5
`b45bd078f97a9799e989ef67877a0b38`). `docs/PERF_HANDOFF.md` 9.33.

**The texture hash cache (9.34).** The first port-side hotspot the profile found was its own: `textureIdentity()`
re-hashed **every** bound texture with XXH3 every frame (~1.9 ms of a 75 ms guest frame, 0.7% of the profile), because
its "at most 60 times a second" throttle was written for the PC build and never bites at 15-17 fps. It almost never
found a change - `changed 0` on every stats line. A full hash is now taken only when GX's own texture-cache
invalidate (BP 0x66) has happened, a cheap eight-window fingerprint of the image has moved, or a round-robin safety
net (one texture per 64 frames) reaches it; the stats line reports `hashed <full> of <all>, missed <n>`, and `missed`
is the number that says the fingerprint is enough. `R2_TEX_HASH_EVERY=1` restores the old behaviour for an A/B.
Emulator: `hashed 1 of 54` where it was 54, no misses, no crash. NRO built 11:45, 64,050,006 bytes; the performance
`boot.cfg` is unchanged (17 lines, md5 `b45bd078f97a9799e989ef67877a0b38`). `docs/PERF_HANDOFF.md` 9.34.

**Threading was not the limit; the stats measurer was (9.35).** Read per thread, the profile is unambiguous: the
GX FIFO processor spends **86.9%** of its samples in `svcWaitProcessWideKeyAtomic`, the render worker **81.7%**, the
shader compiler **99.6%** - their real work is ~9.7 ms and ~10 ms a frame, against a guest thread that is busy and has
no wait call in its top twelve. The 25%-each share of samples is an artifact of the sampler walking the thread list in
order, not four threads doing equal work. A frame is one saturated guest thread and three mostly-idle workers, and the
recompiled guest is single-threaded, so no placement can make it faster. What the per-thread view *did* find was on the
guest thread: `drawBoundsEfb`, the `R2_STATS` per-draw measurer, at **6.7 ms of a 75 ms frame (9%)** - a second decode
of every draw list purely to print how many draws fell outside the scissor. `R2_STATS=1` now keeps the cheap counters
and the breakdown is opt-in with `R2_STATS_BOUNDS=1`, so the line reads `484 draws (bounds not measured:
R2_STATS_BOUNDS=1 adds the breakdown)`. Placement fixed regardless: each Aurora thread owns one core instead of the
shared `0x5` (log: render `actual 0x4`, FIFO `actual 0x1`, guest `0x2`), background workers moved to the lowest core,
and the profiler's sampler (which pinned itself to the render worker's core) takes the lowest core too. NRO built
11:52, 64,050,006 bytes; `boot.cfg` 21 lines (the `R2_STATS_BOUNDS` note only), md5 `0d74665f4d21cf27f85a539ae6a0a5b8`.
Emulator smoke 100 s: no crash, placement and the new stats line confirmed. `docs/PERF_HANDOFF.md` 9.35.

**The guest's own -O2 (9.36).** With the stats measurer gone, ~90% of the guest thread's 75 ms is recompiled
PowerPC at nothing above 0.9 ms a function - and it had been compiled `-O2` since the guest core was brought up. Both
Clang targets (the generated game code and the runtime: `cpu/`, `interp`, `vm`, `hle_*`, `gfx/gx_bridge`) now build
`-O3` through a cache variable, `RL_NX_GAME_OPT` (`switch/build.sh` passes `RL_NX_GAME_OPT` through, default `-O3`).
Measured here: 123 TUs at `-O3`, 261 s build, NRO 64,050,006 -> 65,250,134 bytes (+1.9%), a 110 s Ryujinx run that
boots, draws 5,600 draws by frame 240 and does not crash. **Not** measured: the speed - `-O3` unrolls and vectorises,
which on decompiled code can lose to i-cache, so this is settled by the FPS counter on hardware and reverted with
`RL_NX_GAME_OPT=-O2 switch/build.sh 8`. No fast-math, `-fno-strict-aliasing` kept. `docs/PERF_HANDOFF.md` 9.36.

**The guest accessors were the frame (9.37).** The recompiled guest code expands to **35.7 host instructions per
guest instruction** (measured: `nm -S` sizes against the one generated source line per guest instruction; six hot
functions, 852 guest instructions, worst `f_7FD27418` at 70.9x), because every guest load and store inlined the whole
accessor ladder - `isIo`, `isVm` (twice), the locked-cache test, the arena mask, the byteswap - *and* the body of every
cold path behind it: 4,181 host instructions for 59 guest ones, 199 call sites to `vmMiss`/`lockedCachePtr`/`ioWrite`/
`wrSplit`/`rdSplit` in one function. The Switch branch of `ctx.h` now tests the arena once (`inArena`/`arenaPtr`) and
sends IO registers, the VM region (page split included), the locked cache and anything else to one out-of-line
`rdSlow`/`wrSlow` in `memory_switch.cpp`, in the same order and with the same meanings; the desktop build keeps its
old accessors. Measured: expansion **35.7x -> 20.6x**, NRO **65,250,134 -> 40,833,878 bytes (-37.4%)**, build 162 s,
and an 110 s Ryujinx run with identical draw counts to the previous build (3/484/484 at frames 0/60/120). Not measured:
the frame rate - a guest load went from ~15 host instructions to ~7, which is tens of percent, not the 4.5x 60 fps
needs. Also in this build: `-fwrapv` on both Clang targets, which 9.36's `-O3` shipped without. `docs/PERF_HANDOFF.md`
9.37.

**The GOT load on every guest access (9.38).** After 9.37 the host code of a leaf guest function still fetched
`g_ram`'s address out of the GOT on **every** load and store - `adrp` + `ldr GOT` + `ldr g_ram`, three instructions
and two dependent loads - because the toolchain compiles every TU `-fPIC` and an NRO never `dlopen`s anything.
`-fno-pic` and archive-wide `-fvisibility=hidden` both fail to link here (`ld.exe: read-only segment has dynamic
relocations`: the ELF is linked `-fPIE`), so the fix is `__attribute__((visibility("hidden")))` on `g_ram` alone:
GOT references in `f_80032738` 3 -> 1, NRO 40,833,878 -> 40,706,902 bytes, build 162 s, 110 s Ryujinx run with no
crash, identical draw counts (3/484/484/484) and 0 dropped audio buffers. Not measured: the speed. Also fixed: a
`CACHE` variable that `build.sh` did not pass through, which silently repeated a failed experiment. `docs/PERF_HANDOFF.md`
9.38.

**Hot-call clustering, and what 60 fps would take (9.39).** What is left of the 20.6x expansion is the call
convention: guest registers live in `Ctx`, so a call to another guest function forces the caller to write them back and
reload them, and the 96 generated files mean a hot caller and its callee are almost never in the same TU. The profile
says the top 60 guest functions hold **66.8%** of guest samples; with the 145 functions they call, that is a 205-function,
42k-line cluster - `switch/tools/hot-unity.py` writes it as `gen/code_hot.cpp` and cuts those definitions from their 29
files. At the default threshold Clang inlines nothing (NRO byte-identical: clustering alone is free and inert); with
`-mllvm -inline-threshold=2500` (now the default) the cluster grows 295k -> 321k host instructions and the NRO
40,706,902 -> 42,099,542 bytes (+3.4%), no crash, identical draw counts. A regenerated `gen/` is detected by `build.sh`,
which drops the stale cluster rather than letting duplicate definitions through. **The verdict on the goal:** 60 fps
needs 4.5x and the remaining 70 ms is the recompiled guest code itself - the port-side items left (audio thread, display
list caching, the duplicated GX decode) total ~10 ms. Reaching 60 requires changing the recompiler's call convention
(registers in host registers across calls) or whole-program inlining; `r2recomp.exe` is prebuilt with no source in the
tree and the PIE link cannot take LLVM bitcode. `docs/PERF_HANDOFF.md` 9.39.

**Forcing the hot chains inline (9.40).** 9.39's cluster plus a raised `-inline-threshold` only inlined ~14% of the
callees it could reach - the cost model was still declining them. `__attribute__((flatten))` on the 60 hot functions
orders it instead, and the emitted code shows the difference: `f_7FCBAD28` goes from 2 guest calls to **0** (6,198 ->
8,220 instructions), the top-sampled `f_8004B8EC` pulls in two levels of callees (139 -> 935 instructions). That is the
condition under which `Ctx& __restrict c` lets Clang keep guest registers in host registers across the call boundary,
which 9.39 identified as the wall. Cost: all guest code +6.3%, NRO 42,099,542 -> 43,864,918 bytes (+4.2%), no crash,
identical draw counts, audio clean. `hot-unity.py` applies it by default (`--no-flatten` to disable). Speed still
unmeasured; this is the first A/B to run on hardware - `RL-NX-1246.elf` (flatten), `RL-NX-1238.elf` (cluster only),
`RL-NX-1224.elf` (neither). `docs/PERF_HANDOFF.md` 9.40.

**Widening the flattened cluster (9.41).** `hot-unity.py --top 120` lifts coverage from 66.8% to **80.2%** of guest
samples (1,067 sampled functions; the cluster is 165 functions, 2.1 MB of source). The top-60 functions are unchanged
from 9.40; functions 61-120 are now flattened too: all guest code 7.41M -> 7.87M host instructions (+6.2%), NRO
43,864,918 -> 45,691,734 bytes (+4.2%), 110 s Ryujinx run with no crash, 1,319 draws / 4,534 merged, audio clean.
The four builds to A/B on hardware are `RL-NX-1224.elf` (no cluster), `RL-NX-1238.elf` (cluster, inlining inert),
`RL-NX-1246.elf` (flatten top 60) and `RL-NX-1258.elf` (flatten top 120) - if the larger ones are slower, the i-cache
cost outweighs the register keeping and the answer is `--top 30` or `--no-flatten`, not abandoning the approach.
`docs/PERF_HANDOFF.md` 9.41.

**CPU and GPU balance (9.42).** Measured per frame from the last hardware log: guest thread 75.2 ms with **0% idle**,
GPU ~14 ms idle (`frame fence 0.00 ms`, `acquire 0.16`, `submit+present 0.22` at 13.9-14.7 presents/s), render worker
`record 9.98-10.97 ms` and ~85% idle, GX FIFO processor ~9.7 ms and ~87% idle, shader compiler 99.6% idle, audio thread
unused (its 4.5 ms runs on the guest thread). The port is GPU-rich and CPU-poor: every GPU-shaped job is already on the
GPU, and what is left on the guest thread is the guest's own instruction stream (~70 ms) plus 12.7 ms of emulation (GX
decode 5.6, audio 4.5, display-list replay 2.6) that cannot be split across the idle cores because the game is a
single-threaded PowerPC program. `-fno-unroll-loops` was tried against the flattening's +12% code growth and saved
0.13%, so the default build stands (`RL-NX-1314.elf`, same code as 12:58's). `docs/PERF_HANDOFF.md` 9.42.

**Also measured in that run, open, and not the flicker:** ~380 draws of one blend family (`71146`) were skipped
for shaders on every frame from f5908 to f6236 (~23 s) while one uam compile took 22,014 ms, then all returned in
a single frame. A 22-second hole, not a 1-8 frame flicker - it is a pop, and a separate bug.

**The clear band is dead (2026-11-25).** The lead above was tested and killed. Three things settled it. The
clear draw's rect is clamped from the *same* copy rect whose pixels the display copy reads, so a half clears
exactly the rows it shows: measured in a Ryujinx level (`R2_STATS R2_DRAW_BOUNDS R2_DEPTH_LOG`), the `0,238`
pass draws inside EFB `y=0..241` while its band is `y=2..228`. The 3,059 single-pass frames in the 17:16 log are
not "much of the level" - every level frame there is two-half; they are the attract loop, the menus and a
640x304 crawl. And the mission the flicker was actually reported from - *assuming* `LOGS 11242026-1256` is that
mission, which that log cannot confirm by itself - composes differently again: in `LOGS 11242026-1256` the level
(68-193 s) is one `640x450` copy per frame from EFB `y=16..466`, so it has no
per-half band at all, with 3,904-12,818 draws a frame at 13.2 fps and its two XFB buffers carrying the same
draw count to within 0.05% over 162 frames each. **That link is an inference, not a fact:** the log names no
pursuit, carries no frame capture and no `[depthclear]` line, so "the flickering mission is single-pass" rests
on the report coming from this session, not on anything the log says. What is fact is only that *this*
session's level is single-pass; if the reported mission is two-half, the band still cannot be the cause (it
clears and copies the same rows), so the ranked list in 9.26 is unchanged either way.

**Lead now (revised 2026-11-25, `docs/PERF_HANDOFF.md` 9.27):** not depth first any more. A frame with the HUD,
particles and starfield but no world does not prove its 3D pass ran with a bad depth buffer - in the band layouts
those rows can be left over from an earlier copy, and a stale HUD looks exactly like a fresh one. Two things the
logs already settle: Aurora's per-frame buffers never fill (`frame buffers full` appears in no hardware log, so
no draw is being dropped for capacity) and the per-frame draw count never collapses (the world reaches Aurora
every frame). What is left is the stage Ryujinx never runs - Aurora's recorded frame turned into deko3d commands. The first
suspect there, the frame being cut into several submits mid-pass (128 GPFIFO entries, 13-124 `list splits` per
30 s of level in 1256), has since been checked against deko3d's own source and does not hold: deko3d has no
render pass object at all, `CmdBuf::finishList` resets no state, and the next list continues the same channel -
and in this build a split cannot even fall inside a pass body, because every split point (`barrier()`, `used()`)
is outside one. That mid-pass claim was an inference from the split counters, not something the logs showed. The
order is now a draw skipped because its shader is not loaded yet, a pass that clears what it should have kept,
Aurora's merging, then depth (`R2_GX_OVERRIDE=zalways` remains a last resort), with the split kept only for its
ordering effect. `build-switch/sdcard/switch/RL-NX/boot.cfg` is set up and the build made for the next run
(`R2_STATS`, `R2_FRAME_LOG`, `R2_DEKO_FRAME_LOG`, `R2_COPY_LOG`, `COS_DK_CAPTURE` from 60 s); `R2_DEKO_FRAME_LOG`
adds one `deko frame N:` line per presented frame with passes begun, draws replayed and skipped, submits and
splits with where the last cut fell, the bytes copied into the XFB, and the frame's four biggest passes as
`label:draws->target`. A second pass over the same logs and the code behind each suspect (9.27) then killed most
of the list: render compression is off in these runs, the shader budget is unlimited and nothing is ever queued
(`pipelines 0 queued`, 381 -> 393 created all run), command memory peaks at 768 KiB of a 4 MiB slice with 0
overflows and 2-2.9 submits a frame, the `list splits` figure is 0.4-4 a second, the display copies pair up
perfectly (2,776 full-frame copies in one run, 118 `half@0` + 118 `half@224`, no lone half), and the descriptor,
staging, image-heap and present-texture lifetimes all check out in the source. What is left is a hardware
behaviour inside a pass's replay or a pass that is invisible despite its draws, which the per-pass field of the
new line is built to name. 9.27 says what to look for and what each result means.

**Since then (2026-11-25, same day):** the `deko frame` line has been run in Ryujinx - 331 frames of the attract
demo, every line coherent and agreeing with the bridge's own copy counts, `last cut` always between passes - and
that run found one thing the line could not do: every target read `Render texture`, Aurora's label for all of
them, so it could not say which target a frame drew into. Targets (and copy destinations) now print label, size
and a texture creation id. The cadence's five-second sampling was replaced by `R2_CAPTURE_ON_BREAK=1`, which reads
every presented frame back, keeps its pass-shape numbers, compares them with the median of the last six good
frames and writes a `break-NNNN.ppm` + `prev-NNNN.ppm` pair when passes, draws, skipped draws, XFB copies, copy
bytes or lit pixels break (eight frames in a row = a scene change, so the baseline restarts instead). Run in
Ryujinx: 355 frames, 19 breaks, three matched pairs, no fatal - the demo's cuts to black and its boot-screen ->
demo transition were caught and a transition no longer burns the captures. `boot.cfg` has the switch on; on a
console it is the record of the flicker frames themselves. 9.27 has the details and the numbers.

**And the recordings read, same day:** `RL_Flicker1.mp4` decoded frame by frame settles what the counters could
not. In the bad frame the world's draws rasterise **nothing** - the starfield covers the whole frame and stars show
through where the trench and hull were, so it is not black geometry - while the same frame's radar (a render
texture) shows the same 3D scene freshly and correctly, and the textured HUD text is sharp: matrices, vertices,
textures and descriptors were all good that frame, the world pass ran, and only its depth-tested content is
missing. That puts depth back first on evidence (HUD, sprites, starfield and engine glows are drawn without a
depth test, which is exactly what survives). Events last 1-8 video frames and every bad frame's world brightness
is the same value - deterministic, not noise. Two switches came out of it: `R2_SKIP_DEPTH_CLEAR=1` (skips the
pass's own depth clear) which did **not** reproduce it, because this game's depth clear is a clear draw rather
than a load op, and `R2_FRAME_DEPTH_CLEAR=1`, the candidate fix that clears each depth target once per frame in
the replay and logs when a pass had loaded instead: 0 firings over 165 s of the Ryujinx demo, so the passes do
clear depth there and a console boot counts the frames where one does not. `boot.cfg` carries it; if the flicker
survives, `R2_GX_OVERRIDE=zalways` (never yet run on a flickering frame) says whether the per-draw depth state is
the mechanism. 9.27 has the numbers.

**Tested, same day:** `R2_SLOT_NO_WAIT=1` forces the console's GPU-behind condition by making `frame_begin` take
the frame slot without waiting for its fence - it fired and changed nothing (Ryujinx's GPU never actually lags,
so it cannot reproduce a lag race; inconclusive, not a fix), and the hardware logs carry none of `GPU stalled`,
`has not finished frame` or `staging: reusing a generation`, so those guards never fired on the console either.
The `deko frame` line now also prints the first XFB copy whole, `XFB copy Render texture 640x480#1 -> XFB
640x450#7`, and in a good frame that source is the same texture the world's two half-passes draw into. So one
flicker frame says which of the remaining theories it is without a rebuild: world target == copy source (depth as
those draws see it), target != source (the frame drew where the picture never reads), `XFB 0` (never copied out),
or a collapsed draw count (never issued). And `R2_DEPTH_SHOT=1` now writes the broken frame's own depth beside
the pair (`depth-NNNN.pgm`, 0 = near, 255 = far), which settles the one question left: the world's shapes in the
depth with no world in the colour means the draws rasterised and their depth was wrong; an all-far depth means
they never rasterised at all; the previous frame's shapes mean the buffer was never cleared. Validated in Ryujinx
at frame 296's break - `depth-0296.pgm` holds the scene's own depth. The pass field also counts the two states
that decide whether a fragment can paint at all, `[dN bM]` (depth test on / blending on), which names a world
pass that lost either, with the pass's blend modes and their counts (`b71841x1,71081x397,71146x234` is Ryujinx's
level pass: one const-multiply quad, 397 opaque writes, 234 alpha blends - the good-frame signature a flicker frame
is compared against), and the depth compare functions (`z7` GreaterEqual, `z8` Always - this port's depth is
reversed, 0 = nearest, and the game clears to 0.000). That convention is why `R2_FRAME_DEPTH_CLEAR` must clear to 0
and not 1.0: it cleared to 1.0 until 2026-11-25, which under reverse depth would have blanked the world the moment
it fired. `R2_DEPTH_CLEAR_ZERO=1` forces a pass's clear to 0 and was verified identical to the baseline. Dropping the game's
own depth clear quad (`R2_SKIP_CLEAR_DRAW=1`, aimed at `Render texture 640x480#1` and confirmed by its log) or
forcing far depth (`R2_DEPTH_POISON=1`) leaves the emulator frame byte-identical: the attract demo's camera barely
moves, so a stale depth buffer still accepts its geometry. That is why the defect never reproduces in Ryujinx and
why the console's depth dump, not the emulator, is what decides it.

The one way the emulator *can* show it is a poison written after the game's depth clear quad
(`R2_DEPTH_POISON_AFTER_CLEAR=<stride>`, 2 alternating frames inside one run): the break trigger then reports
`lit pixels 0 vs 32753` for adjacent frames, so a non-empty depth buffer removes what the depth test rejects, up to
a whole frame. The matching clear placed at that point (`R2_DEPTH_CLEAR_AFTER_CLEAR=1`) has **not** been shown to
restore it - it is in `boot.cfg` as a candidate, and 9.27 records why the first claim of a proven repair was
withdrawn. And the console's GPU-bound condition was attempted in Ryujinx by rendering at 4x internal
resolution: it still ran at 60 fps, so the emulator cannot be pushed into the hardware's timing and the config was
restored. `boot.cfg` carries all of it.

### 8.11 A mission chosen from the menu aborts at the transition into play — open 2026-11-24

**Symptom:** pick a mission and a craft, the crawl plays, and the app crashes on the hand-over to level
play (two reports, 13:59 and 14:03, identical PC). The attract demo, which streams the level's textures
while the crawl plays, still enters the same level and plays.

**State:** not fixed. The crash is deko3d's own abort, not a fault in our code: `2359-0001` is
`RaiseError(DkResult_Fail)` from `submit_list`/`submit_readback` in `switch/deko/device.cpp`, i.e. the queue
was already in its error state (a GPU fault reported earlier in the frame) when the next submit or flush
ran. deko3d's release build compiles its error report out, so the log ended silently and the fault itself
was never recorded. Also found and fixed: the `nvGpuChannel*` diagnostic wrappers in
`switch/deko/host/gpu_submit_diagnostics.cpp` were documented as linked with `--wrap` but had no such flag,
so they were dead code - that is why the stats line always read "at most 0 entries per kickoff". They are
wrapped now, and a third one (`nvGpuChannelGetErrorNotification`) prints the driver's error info and the GPU
breadcrumb before deko3d aborts.

**Reproduction (user, 2026-11-24):** it only happens when a button is pressed to cut the crawl/text short
while the level is still loading - i.e. the load is jumped past rather than run to its end. That is the
difference from the attract demo (which never skips), and it means the trigger is a re-entered/abandoned
transition, not the level itself.

**The fault, from the wrapped build's log (14:27):** `[gpu-submit] kickoff failed: rc=0x00000d5c`
(`2348-0006`, the channel timeout this port already knows from scene loads, `switch/deko/device.cpp`) at frame
1971, right after the level-load block burst and the first in-level frame's display copy. The GPU stopped at
breadcrumb serial 277709 (CPU was at 277751) - the middle of the level's texture uploads (a 512/256/128/64/
32/16/8 mip chain plus a 184x104 upload). No error notification ever arrived and
`nvGpuChannelGetErrorInfo` refused (`rc=0x275c`), so this is a timeout, not a page fault or method error.
The attract demo sends the same uploads - while the crawl plays - and survives, so what differs is *when*
they are submitted, not what they contain.

**Not reproduced since (user, 2026-11-24 17:16 run):** the latest NRO ran 203 s including the level with zero
`kickoff failed`, `2348-`, `2345-`, abort or panic lines - the timeout is intermittent, not tied to every
mission start. Nothing was changed for it: the `--wrap` submit diagnostics and the `[gpu-breadcrumb-copy]`
copy record stay armed, and the next occurrence names the upload that stopped the channel. If it recurs, keep
`sdmc:/switch/RL-NX/logs/log.txt` from that boot - the breadcrumb is only printed at the abort.

**Next:** the buffer-to-image uploads now keep a copy record of their own (`gpu_operation_buffer_image_copy`,
`switch/deko/backend_commands.cpp`), so the next timeout prints the image, its format, mip, destination
origin and the staged extent (the `[gpu-breadcrumb-copy]` pair) instead of only the copy converter's last
bound target. No rebuild-only test: `perf.cfg` with `gpu_steady_v9 1` flushes every 32 operations, i.e. sends
the same burst in smaller batches - it tells whether the timeout is about how much of the load is queued at
once (the SFA-NX note on `kartpad_gpu_transition_guard` says every 2348-0006 it recorded came while a course
loaded, and its fix was to complete each operation during the load; that guard has no implementation in this
build, `switch/src/transition_guard.cpp` is absent).

**2026-11-25 (build 18:42):** three more reports of the same abort (10:08, 13:29 with build 13:14,
17:22 with build 16:42; the 10:08 one matches no saved ELF), each `2359-0001` raised inside
`dkQueueSubmitCommands` -> `Queue::onCmdBufAddMem` -> `flush()` ("flush queue in error state") during a
mid-frame `submit_readback`: the GPU had already faulted or timed out on earlier work. No log came back
with them. From build 18:42 the release deko3d library calls `rlnx_deko_release_error()`
(`switch/deko/backend_commands.cpp`, patch 4 in `switch/third_party/deko3d/PATCHES.md`) before it aborts,
which prints `[dk] FATAL: deko3d release error`, the breadcrumb history (`[gpu-progress]`,
`[gpu-breadcrumb]`, `[gpu-breadcrumb-copy]`) and flushes the log to the card synchronously. So the next
crash's `logs/log.txt` names the operation the GPU stopped on, whichever path raised the error.

**2026-11-25 (build 18:45 logs, `E:\RL-NXLOGS\LOGS 11252026-1854` and `-1856`): Tatooine Training, every time.**
Selecting it goes to black and aborts; other missions load. The hook worked. Both runs show
`kickoff failed: rc=0xd5c (2348-0006)` and then `[dk] FATAL: deko3d release error 1 (queue not in error state)`.
- **The GPU was not hung.** In 1854 and 1856 the GPU's last report sits just before the same upload (mip 6,
  8x8, of a 512x512 7-level RGBA8 texture), exactly one kickoff (~22 uploads) behind the CPU. In 1427 the GPU
  completed 20 more uploads while the failure dump was printing. deko3d's layout for that mip checks out
  (offset 1397760 of 1398272 bytes), so the copy is not malformed.
- **What is unusual is the frame.** It makes 2371 / 3641 / 4276 submits (`[gpu-submit] frame=... submits=`)
  against about 2 for a normal frame. `gpfifo_entries_recorded` (`switch/deko/device.cpp`) submits and
  flushes every 128 GPFIFO entries, and never waits for the GPU. Each kickoff carries ~109 entries and 42
  syncpoint increments, because every upload signals its own flushing fence (`Allocation::used`). The driver
  refuses one of these kickoffs with a timeout, and deko3d aborts at the next flush. The attract demo sends the
  same uploads spread over the crawl and survives.
- **Fix (build after 18:45, hardware untested):** every `R2_GPU_BURST_WAIT` batches within a frame (default
  4; `0` = the old behaviour) the render worker waits for the GPU to go idle (`dkQueueWaitIdle`), so the
  kickoffs in flight stay bounded. `R2_GPU_BATCH_ENTRIES` (default 128) sets the batch size. This only
  touches frames with more than 4 batches, which are load bursts; a normal frame never reaches one.
- **New log lines:**
  - `GPU bursts: a submit every N GPFIFO entries...` once at start.
  - `GPU burst: frame F: B batch submits, W GPU waits (ms, longest)` for each frame with more than 8 batches
    (the first 64, then each new maximum).
  - `GPU bursts (30 s): ...` summary.
- **If it still crashes:** read the `GPU burst:` line for the failing frame. Then try `R2_GPU_BURST_WAIT=1`
  (wait after every batch). If that still fails, try `R2_GPU_BATCH_ENTRIES=512` with `R2_GPU_BURST_WAIT=1`
  (4x fewer kickoffs). A crash with the wait at 1 rules out in-flight depth and points at the fence count per
  kickoff: one shared fence per batch instead of one per upload.

---

## 9. Remaining technical work

1. **Confirmed on hardware:** fullscreen picture (08:38, 8.1), controller (08:56, 8.6), menus drawn
   once and the craft present in missions (8.7). **Next:** the `-` menu itself (7.2) - one run with
   its draw call compiled in against one without.
2. **Game-thread speed** (7.1): measured. In level, 47.35 ms a frame = 25.6 guest + 17.4 port. Next,
   in the plan's order: finish phase 1b and read it in level, move audio off the guest thread
   (~3.4 ms), cache the display-list replay (~2.3 ms), then phase 3's LLVM backend for the guest's
   25.6 ms. Nothing else in the port reaches 60 fps.
3. **Exit / restart ×3 and a 30-minute session** (`HOME`, sleep, dock) with a flat heap line — do this
   before more features, per the skill.
4. **Make the port's own tooling honest**: `boot.txt` + run-delimited capped log, import Atmosphere
   crash reports at start, wire `check_log.py`/`ryujinx_smoke.py`, retarget `pre-push` and drop
   `sync-upstream.sh`.
5. **True 720p handheld** (and the docked decision), then re-measure — after §8.1, not before.
6. **Features**: finish the `-` menu (7.2) and grow it into the options menu (aspect, resolution,
   clocks via the official profile only), widescreen using Red Two's own HUD anchoring, HD texture
   packs (`mods/hd/`), updater (GitHub `releases/latest`, SHA-256, header check, restart through one
   `request_relaunch()`), shader cache seeded in RomFS.
7. **Release**: NRO-only asset, player README (title takeover, logs, credits, licences), version bump,
   and every line of the done bar in `references/new-game-survey.md` part C met on hardware.
8. **`git init`** with the port's identity (`digdat0`, no `Co-Authored-By`), when the user asks.

---

## 10. Diagnostics reference

Set these in `sdmc:/switch/RL-NX/config/boot.cfg` (`KEY=VALUE`, one per line; it moved from the folder root
on 2026-11-26, and normal play needs no file) - every line that is set is
logged. All of them are off by default, so a normal run is unaffected, except the on-screen frame
counter, which is on unless `R2_FPS=0`.

| Switch | What it gives you |
|---|---|
| `R2_DRAW_LOG=1` | each distinct draw's scissor + viewport + `bp59` + projection; every distinct deko scissor rect; every pass with its target; the texture presented; the viewport the renderer is given; uniform dumps of the first draws after 25 s |
| `R2_DRAW_BOUNDS=1` | per draw, the EFB rectangle its vertices cover (renderer and "hardware" placement) + the scissor sent; per frame, the union split 2D/3D; and *why* a draw could not be measured |
| `R2_COPY_LOG=1` | every display copy and where it lands (`line N of M`), scissor box offset changes, screen-space 2D brackets |
| `R2_GX_SKIP=<range>:<filter>` | drop draws (`v<packet>`, `p<prim>`, `t<texaddr>`, `b<blend>`, `z<zmode>`, `i<first>-<last>`, `*`) in a vblank/second window |
| `R2_GX_OVERRIDE=zalways,aalways,nocull,nofog` | relax per-draw tests to see what rejects geometry |
| `R2_NO_MERGE`, `R2_NO_FILL`, `R2_NO_SS2D`, `R2_NO_XFB` | disable draw merging, screen-fill stretching, screen-space 2D, the emulated XFB |
| `R2_COPY_KEEP_SAME=1` | old EFB-copy rule: a same-size texture written over a copy keeps showing the copy (8.8) |
| `R2_CAPTURE_ON_BREAK=1` | read every presented frame back and write a frame that breaks the pass shape or the lit count as `break-NNNN.ppm` + `prev-NNNN.ppm` |
| `R2_CAPTURE_FLICKER=1` | write a frame whose picture jumped from the one before it (a flicker edge) as `flk-before-NNNN.ppm` + `flk-after-NNNN.ppm`; `R2_FLICKER_THRESH` (20/255), `R2_CAPTURE_MIN_DRAWS` (0), `R2_CAPTURE_GAP` (40), `R2_CAPTURE_DUMPS` (24), `R2_FLICKER_LOG=1` prints the change per frame (8.10) |
| `R2_DEPTH_SHOT=1` | add `depth-NNNN.pgm` for a dumped frame (0 = near, 255 = far) |
| `R2_CLEAR_LOG=1` | one line per frame for the game's own depth-clear quad: the z it writes, the rect it covers, and whether the pass had already cleared depth |
| `R2_ZFREEZE_LOG=1` | every zfreeze draw's plane and the depth it gives at the viewport's centre and edges (1.0 = far under reversed depth, so a saturated plane rejects everything behind it) (9.30) |
| `R2_SKIP_DRAW_RANGE=a:b` | drop draws a..b of every pass - bisects which draw leaves the depth buffer holding one value, without a rebuild (9.30) |
| `R2_ZFREEZE_GUARD=1` | reject a zfreeze reference plane that is not a depth function (non-finite, \|slope\| > 1 a pixel, \|offset\| > 2, w near 0) and write the flat far depth 0.0 instead; logs each rejection. **This is the in-level flicker fix** - run 5 on the console kept the world in frame for the whole session with it on (9.31) |
| `COS_DK_CAPTURE=1` | PPM of the presented picture every 5 s from 20 s (`R2_CAPTURE_FULL=1` for full size instead of a downscale) in `logs/deko-capture/` |
| `COS_DK_TEST_PATTERN=1` | draw the test pattern instead of the game (proves the present path) |
| `R2_GX_THREAD` | where GX is processed. Default: Aurora's own FIFO thread (core 0 under the tuned policy); `R2_GX_THREAD=0` puts it back inline on the guest thread, the A/B |
| `R2_FPS` | the on-screen frame counter, top middle: **on by default**, `R2_FPS=0` hides it. It counts real presentations, not the guest's retraces, and that gap is the performance story (8.2) |
| `R2_STATS=1` | the per-frame `[stats]` counters: draws, merges, texture binds and changes, copies, and what the frame left undecoded. Cheap counters always; the per-draw bounds breakdown is `R2_STATS_BOUNDS=1` (9.35) |
| `R2_STATS_BOUNDS=1` | add the per-draw "did it land in the scissor" measurement to `[stats]` - ~6.7 ms a frame, 9 % of the guest thread, so opt-in (9.35) |
| `R2_TEX_HASH_EVERY=1` | hash every bound texture every frame (the pre-9.34 behaviour, for an A/B); `R2_TEX_HASH_NET=<n>` sets the round-robin safety net, default 64 frames (9.34, 9.49) |
| `R2_MENU_OPEN=1` | open the `-` panel on the first frame, so a run proves its draw path without a hand on the controller (7.2) |
| `R2_MENU_DRAW=0` | the `-` panel is not drawn (bisect; default on, and a closed panel records nothing) (7.2) |
| `R2_MENU_PAD=0` | the `-` panel gets no input (bisect; default on) (7.2) |
| `R2_GPU_BURST_WAIT=N` | wait for the GPU every N batch submits in one frame (default 4, `0` = never: the old behaviour) (8.11) |
| `R2_GPU_BATCH_ENTRIES=N` | GPFIFO entries per batch submit (default 128, min 16) (8.11) |
| `R2_PROFILE=<s>` | 1 kHz sampling profile of the game thread for `<s>` seconds → `logs/profile-NNN.txt` (`switch/tools/prof_report.py`). From build 19:49 a report is written every 30 s (was 120 s) (PERF_HANDOFF 9.52) |
| `-` menu **PERF LOG** | **on by default** (`R2_PERFLOG=0` starts it off). Every second `[pm]`, `[pm-mem]`, `[pm-gx]`, `[pm-dk]`, `[pm-thr]`: frame time split, the game thread's timed spans (idle, HLE DSP, disc, audio, locked-cache DMA), slow-path guest memory accesses by region, GX/Aurora/deko3d figures, per-thread CPU %. A `[hitch]` line for each game frame over 50 ms (6 a second at most). `switch/tools/perf_report.py <log.txt>` summarises a run (PERF_HANDOFF 9.52) |
| `-` menu **GPU TIMING** | off by default. Per-pass GPU timestamps on one frame in 120 (`gpu time #N`), and per-draw costs every 5th sample (`gpu draws #N`; those probe frames run slower). Hardware only: Ryujinx reads timestamps as 0 |
| `R2_ICOUNT=1` | `[meter]` line every 2 s: M guest instructions a frame, **ns per guest instruction**, M insn/s. The codegen metric - it does not move when the scene does. Needs a metering build (`RL_NX_ICOUNT=1 switch/build.sh 8`); ordinary builds carry no counter. |
| `R2_STATE_HASH=<frame>[,...]` | boot sanity probe: at those frames, `[state] frame N hash <xxh3> draws N` over the guest's registers and all 256 MiB of RAM. It cannot compare two builds - the guest's timeline varies run to run (PERF_HANDOFF 9.46) - so use the oracle test for that. |
| `R2_ORACLE=<n>` | **the codegen gate.** n guest functions are run under the interpreter and under the recompiled code from identical bytes with the devices muted; `[oracle] <entry> ok` or `MISMATCH` per probe. Needs a probe build (`RL_NX_ORACLE=1 switch/build.sh 8`); costs ~100 ms a probe (two 256 MiB copies and hashes). |
| `R2_ORACLE_FAULT=1` | proves the gate can fail: flips one bit of `r3` in the recompiled result, so every probe must report MISMATCH. |
| `R2_DETERMINISTIC=1` | disabled: a frame-driven guest clock deadlocks the boot (PERF_HANDOFF 9.47). Setting it only logs that. |
| `config/perf.cfg` | thread placement policy: `baseline`, `main`, `split`, `tuned` (default), `game`; `THREAD` lines in the log show the result |
| `R2_AI_LOG=1`, `R2_PAD_LOG=1`, `R2_INPUT=3000:START*4`, `R2_AURORA_DEBUG=1` | audio cadence, pad, scripted presses, Aurora's own debug log |

Useful greps on a log: `swapchain:`, `window:`, `present from`, `pass into`, `bounds frame`,
`scissor `, `display copy`, `staging:`, `presents/s`, `perf  `.

---

## 11. Paths

```
RL-NX/
  src/                  Red Two runtime + recompiler (upstream, MIT); our edits marked // RL-NX:
  config/GSWE64/        symbols, hooks, layout for the recompiler
  patches/aurora/       Red Two's 33-patch Aurora series (graphics correctness — required)
  extern/aurora         pinned Aurora (f77492f + the 33 patches)
  switch/               the Switch layer: src (log, memory, audio, boot), deko, uam,
                        third_party/deko3d, builder, cmake, tools, licenses
  docs/SWITCH_PORT.md   the survey, every measurement, and the bring-up checklist (§E) in full
  build-switch/         build output + generated game code (git-ignored, never shared)
    RL-NX.nro           the artifact for the SD card
    rl-nx.elf           symbols (regenerate syms.tmp after each build)
    logs/build-*.log    build logs
    capture/            analysis images of the current picture bug
  README.md             upstream Red Two (PC) — untouched apart from a pointer to this file
```

Emulator used for every non-hardware check: a portable Ryujinx at `/e/ryujinx_test`
(`portable/sdcard/switch/RL-NX/` is its SD card, `logs/log.txt` its log).

---

## 12. Licences

Red Two: MIT (upstream). Aurora: MIT. Dawn: Apache-2.0. deko3d: zlib (patched build). Switch layer
(`switch/src`, `deko`, `uam`, `builder`, `tools`): GPL-3.0 own work with **MPL-2.0** file-level pieces
from KartPadNX / Melee-NX / SwitchWaker — keep the notices in `switch/licenses/` and
`switch/deko/THIRD_PARTY.md`. dlmalloc: public domain.

Nothing here ships game assets, and the port must not be contributed back upstream without the
upstream author asking.

**NRO icon:** the "Rogue Leader" grid art by **davy** on SteamGridDB
(https://www.steamgriddb.com/grid/544663), used as the app's icon with thanks. `icon.png` (1024x1024, at the
repo root, not tracked) is downscaled to the 256x256 `switch/icon.jpg` that `nx_create_nro` embeds in the
NRO.
