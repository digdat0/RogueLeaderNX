# RL-NX: the Switch layer (state, 2026-11-23)

Rogue Leader (Red Two) on Nintendo Switch homebrew: `.nro`, libnx/devkitA64, deko3d.
Read `../docs/SWITCH_PORT.md` first — it holds the survey, the measurements and the bring-up plan.

**Status lives in `../README.PORT.md`.** Short version: the game boots and runs, the deko3d build
creates a swapchain and presents on hardware, and the picture is still wrong (a 640x30 band of the
frame) - that file's sections 7-9 hold the evidence and the remaining work. This file stays the
layer's own notes: what came from where, prerequisites, options and the black-screen hand-over.

## What came from where

| Part | Origin | Licence |
|---|---|---|
| `src/` (log, dlmalloc, threads/exit, updater, vsync, perf, profiler, SQLite VFS) | SFA-NX via `kit/new_port.py`, renamed `sfa*`/`SFA*` → `rl*`/`RL_NX*` | GPL-3.0, MPL-2.0 pieces — see `licenses/` |
| `deko/` (kd shim, direct recorder, pipelines, present, shader cache) | SFA-NX (Melee-NX / KartPadNX / SwitchWaker) | MPL-2.0 + MIT — see `deko/THIRD_PARTY.md` |
| `uam/` (Mesa GLSL front end + Maxwell codegen) | Melee-NX | see `uam/LICENSE` |
| `third_party/deko3d` | patched build (see `references/deko3d.md` in the skill) | zlib |
| `builder/`, `cmake/`, `tools/` | SFA-NX | GPL-3.0 |

`switch/tools/hooks/pre-push` and `switch/tools/sync-upstream.sh` still describe Foxhollow's upstream;
retarget them at `gminustwo/red_two` (or drop `sync-upstream.sh`, since we follow a 4-day-old upstream
by hand) before the first push.

## Prerequisites (unchanged from SFA-NX)

devkitPro with `switch-dev switch-tools switch-zlib switch-curl`, LLVM (Clang, for the game code),
Python 3 as `py`, and `ref/dawn` + `ref/SDL-dusklight` prepared as `README` in SFA-NX describes:

```bash
git clone --depth 1 https://github.com/encounter/dawn ref/dawn
git clone --depth 1 -b release-3.4.10 https://github.com/libsdl-org/SDL ref/SDL-dusklight
bash switch/build-dawn.sh   # Dawn Null + Tint
bash switch/build-sdl.sh    # SDL3
bash switch/build.sh        # -> build-switch/RL-NX.nro
```

## What the build expects

- **Aurora pin.** `extern/aurora` is `encounter/aurora` @ `f77492f` with `patches/aurora/0001..0033`
  applied in order (they are a series, `33 of 33`). `build.sh` stages it into
  `build-switch/aurora-patched` and applies the port's own fix-ups there: `io-atomic`, and
  `tools/patch-aurora-dawn-status.py` (our Dawn pin returns `wgpu::Status` where this Aurora expects
  the newer `ConvertibleStatus`).
- **Renderers.** The build is headless by default: Aurora is configured with
  `AURORA_SURFACE_HEADLESS` and Dawn's Null device, so the whole GX path runs with no swapchain.
  `RL_NX_DEKO=1` builds the native deko3d renderer instead (`switch/deko`, `switch/uam` and
  `switch/third_party/deko3d`; ~5 min more): `builder/integrate-deko.py` stages `build-switch/aurora-deko`
  from the patched tree and adds the three seams (deko init in `webgpu::initialize`, shutdown in
  `webgpu::shutdown`, `kartpad_deko_present()` in `end_frame`), `cmake/Deko3D.cmake` adds `--wrap` for
  every symbol in `switch/deko/wrap-symbols.txt`, and `switch/src/deko_glue_switch.cpp` provides the
  four SFA-NX kit symbols it links against. Add `RL_NX_DEKO_DEBUG=1` to link `libdeko3dd`, which checks
  every call and reports a failing one (its message names the call) instead of aborting inside it.
  State: the deko build initialises, renders the game and presents when the window can take a swapchain;
  on an emulator (whose layer owns the window's buffers) it runs without presenting, and
  `COS_DK_CAPTURE=1` in `boot.cfg` writes the presented picture to `logs/deko-capture/` as PPM, one image
  every five seconds from 20 s on. See `docs/SWITCH_PORT.md` §E 4b, 4c and 7.
- **Memory.** A buffer larger than 16 MiB keeps two staging generations instead of four (18 buffers of
  107 MiB had reached ~1.9 GiB); small buffers keep four, so uploads stay pipelined.

## Clocks

RL-NX asks for one of the console's own retail handheld profiles at start: **CPU 1020 MHz, GPU 460.8 MHz,
memory 1331.2 MHz** (apm configuration `0x92220008`, the one official games use; 384 MHz if that is refused).
It is not an overclock, and it is not a setting: it is always on in handheld, docked stays at the system's
768 MHz, and the system's own profile is put back when the game exits. The run's log shows the clocks in use
(`[clock]` line). The port never raises the CPU past retail; players who want more can use their own
overclocking tool (sys-clk, Horizon-OC) - those set the clocks themselves and win over this profile.

## Running it on a Switch

1. Put the NRO at `sdmc:/switch/RL-NX/RL-NX.nro` and your own disc image next to it
   (`game.iso`, or any single `.iso`/`.gcm`/`.rvz`/`.wia` in that folder - nothing is bundled).
2. Start it **with title takeover** (hold R while starting any game); applet mode has too little
   memory for the 256 MiB guest arena.
3. The screen shows the boot report (build stamp, launch mode, the disc it found), then the renderer
   takes the window and draws the game. Everything after the boot report goes to
   `sdmc:/switch/RL-NX/logs/log.txt`.

### If the screen stays black

The console screen and the renderer share one window, and the two have to take turns:

- The boot report is written through a descriptor saved before the log captures stdout (libnx's console
  draws nothing until `consoleUpdate`), so text appearing and then the screen clearing is the renderer
  taking over - that is the normal hand-over, not a crash. A screen that never showed the boot report
  means stdout went to the log before the console drew (the old behaviour, a black screen with a
  perfectly healthy game behind it).
- libnx's console framebuffer holds the default window's buffer slots, and deko3d's swapchain needs
  them. `consoleExit` before `switchInit` gives them back; `init_swapchain` also releases a window that
  still has slots configured, and logs `swapchain: the window had buffers configured ... released them`.
- No swapchain means nothing is presented and the screen is black: grep the log for `swapchain:` and
  `window:` - they say whether the window was usable and whether the swapchain was created.

## Options (`boot.cfg`)

A console has no shell, so Red Two's `R2_*` environment variables are read from
`sdmc:/switch/RL-NX/config/boot.cfg` (in the folder root before 2026-11-26; optional, diagnostics only) - one `KEY=VALUE` per line, `#` comments. Every line that is set is
logged. Useful ones: `R2_INPUT=3000:START*4` (scripted presses, in retraces), `R2_GX_THREAD=1`
(Aurora's GX thread), `R2_AURORA_DEBUG=1`, `R2_PAD_LOG=1`, `R2_NO_XFB=1`, `R2_DEPTH_LOG=1` (one line per
EFB pass a display copy ends: which half frame it is, and whether its depth clear became the pass's own load
op or a clear draw) and `R2_DRAW_LOG=1` (the
scissor/viewport each distinct draw uses) and `R2_DRAW_BOUNDS=1` (the EFB rectangle each draw's
vertices cover, transformed on the CPU - a picture that appears as a band of the frame is either
geometry covering only that band or a clip there, and this says which).
