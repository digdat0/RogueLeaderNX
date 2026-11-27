# RL-NX - working instructions for the next agent

This is the handover brief for whoever picks up the Rogue Leader Switch port next. It is not the
record of what was done: `README.PORT.md` (status, decisions, known issues) and
`docs/PERF_HANDOFF.md` (numbered evidence, sections 9.x) are. This file is the *how*, written to be
executed rather than read for pleasure.

**Two tasks are queued, in this order:**

1. Finish the `-` in-screen menu so it draws, responds, and is safe (section 4).
2. Continue the optimisation queue (section 5).

Do not start either before the check in section 8. The tree is in a known, working state; confirm
that before you change it, so a later failure is yours and not inherited.

---

## 1. State of the tree, 2026-11-26

| thing | value |
|---|---|
| Current NRO | `build-switch/RL-NX.nro`, **40,706,902 bytes** - the A baseline the hardware numbers were taken on (`debug/RL-NX-1224.elf`) |
| `gen/` | Stage 12 (2026-11-27 build 10:16, `debug/RL-NX-1016.elf`, copy `build-switch/tmp/RL-NX-1016.nro`; `build-switch/host/r2recomp-v13.exe`; the v12-equivalent gen (default switches, identical to the old live gen bar `__restrict`) is in `build-switch/tmp/gen-v13ctl`, the live one in `build-switch/tmp/gen-v13`): **`R2RECOMP_VMFNS=0`** ignores the 510 `F <entry>` lines of `config/GSWE64/vmsites.txt`, so only the 459 listed instructions get the inline VM forms (920 instructions both passes, against 176,314; 425 V lines against 19,572 in the generated files). VM accesses elsewhere in those functions take `rdSlow`/`wrSlow`. Hardware **unmeasured**: A/B against `build-switch/tmp/RL-NX-1005.nro` (same runtime, old gen). Oracle (Ryujinx, `RL_NX_DEKO=1 RL_NX_ORACLE=1`, `R2_ORACLE=300 R2_ORACLE_EVERY=41 R2_NATIVE_MTX=0`): **300 passed, 0 failed, 19 skipped**; `R2_ORACLE_FAULT=1` 40 of 40 MISMATCH; oracle NRO `build-switch/tmp/RL-NX-oracle-v13.nro`. Plain `RL_NX_ORACLE=1 switch/build.sh 8` (no DEKO) no longer links (`swdk::overlay_fps_*` undefined in menu_switch.cpp). |
| `gen/` | Stage 11 (2026-11-26 build 20:04, `debug/RL-NX-2004.elf`, copy `build-switch/tmp/RL-NX-2004.nro`; `build-switch/host/r2recomp-v12.exe`; the 18:02 gen is in `build-switch/tmp/gen-1802`, this one in `build-switch/tmp/gen-v11b`; pre-change sources in `build-switch/tmp/pre-stage11`): **LIVE** (`R2RECOMP_LIVE=0` off): per-statement dirty (forward) / live and Ctx-need (backward) analysis in emit.cpp; entry loads only the live-in slots, spills and returns store only dirty slots, reloads only live-after slots, and the summary read sets are a least fixpoint of what each function really reads from Ctx (13.3 slots average): 48,167 entry loads, 209,831 stores, 203,735 reloads left out. **SO0** (`R2RECOMP_SO0=0` off): XER[SO] compiled as 0 (no o-forms in the game; mtxer goes through ctx.h soCheck, logs once). **SDAFAST** (`R2RECOMP_SDAFAST=0` off): r2/r13 accesses take the unchecked stack forms in functions that never write that register (23,384 sites). Oracle (build 19:13): 299 passed / 1 failed (the known f2 NaN payload 80034C34) / 47 skipped, same as v10. **FASTMEM** (`R2RECOMP_FASTMEM=1`, off by default): every other access takes ctx.h's F forms, except in functions that build an IO/uncached/EFB address (lis 0xC000-0xC1FF, 0xC800-0xC8FF, 0xCC00-0xCDFF) and at `config/GSWE64/fmchecked.txt` sites (vm_census.py --fastmem); F forms are the tested forms unless the NRO is built with `RL_NX_FASTMEM=1` |
| `gen/` | Stages 8+9 (2026-11-26 build 17:09, `build-switch/host/r2recomp-v10.exe`; v9 gen in `build-switch/tmp/gen-v9`, v8 gen in `build-switch/tmp/gen-v8`): **ABI2** (`R2RECOMP_ABI2=0` off): a function that saves a callee-saved slot with rA = r1 (stw/stmw r14-31, stfd f14-31, psq_st both halves) has it out of its summary's read and write sets, and indirect calls (dispatch) count as ABI calls (args r1 r2 r13 r3-r10 f1-f8 cr1 in, volatiles out) instead of opaque; opaque functions 4582 -> 2585, summarised sites 18988 -> 24595, gen 57 -> 54 MB; Ctx copies of callee-saved regs may be stale by design (oracle zeroes the 64 KiB below entry r1). Oracle 299 passed / 1 failed (the known f2 NaN payload, 80034C34) / 47 skipped. **VM sites** (`R2RECOMP_VMSITES=0` off): `config/GSWE64/vmsites.txt` (from `switch/tools/vm_census.py`, Ryujinx attract census, `--min 20000`: 58 instructions + 75 `F` functions, 99 % of VM traffic) get ctx.h rd32V/wr32V/psqLdFV... (VM TLB test inline). **abi hooks** (hooks.txt kind `abi`): the three matrix natives keep their callers' summaries |
| `gen/` | Stage 4 (2026-11-26, `build-switch/host/r2recomp-v8.exe`): functions that build a gather pipe address (`lis rX,0xCC01`) use ctx.h's wr8P/wr16P/wr32P, which write straight into the gather buffer (`R2RECOMP_PIPEFAST=0` off; runtime switch: the - menu's FAST PIPE row, `R2_FAST_PIPE`); the v7 gen is in `build-switch/tmp/gen-v7`. Stage 3 (2026-11-26, `build-switch/host/r2recomp-v7.exe`): r1-based accesses use ctx.h's unchecked stack forms (rd32S...; `R2RECOMP_STACKFAST=0` off, `-DR2_STACK_CHECKED` in ctx.h too); ctx.h tests one arena window inline (uncached view moved to rdSlow/wrSlow, arena + kArenaSlack; `-DR2_TWO_WINDOW` for the old test). The v6 gen (Stage 2) is in `build-switch/tmp/gen-v6`. Stage 2 (`r2recomp-v6.exe`): paired singles and fadds-family in float when all operands are float-typed (`R2RECOMP_PSFLOAT=0` off), inline fctiwz; the v5 gen is in `build-switch/tmp/gen-v5`. Stage 1 (`build-switch/host/r2recomp-v5.exe`): v4 + ABI-only write-back at returns (`R2RECOMP_ABI=0` off) + GQR0 as a constant (`R2RECOMP_GQR0=0` off); ctx.h converts singles by hardware (`-DR2_EXACT_FP` for the exact forms); oracle compares ABI-live registers only (`R2_ORACLE_ABI=0` for all). The v4 gen is in `build-switch/tmp/gen-v4`. Before that: locals v2 + call-site summaries (PERF_HANDOFF 9.57) + float FPR typing (9.58), from `build-switch/host/r2recomp-v4.exe` (`R2RECOMP_FLOATFPR=0` turns typing off; the 9.57 gen is in `build-switch/tmp/gen-v3`, the 9.55 gen in `build-switch/tmp/gen-v2`); the old `c.r[]` form is in `build-switch/tmp/gen-nolocals` (`R2RECOMP_NOLOCALS=1` regenerates it) |
| In-level frame | **47.35 ms (~21 fps)** = 25.6 ms guest (54 %) + 17.4 ms port (37 %); 6,100-6,800 draws a frame |
| Measured ceiling | **~39 fps** on this codegen (port at zero, guest unchanged) - 60 is not reachable by port work alone |
| Correctness gate | the oracle test works and can fail (`PERF_HANDOFF` 9.48): 6/6 `ok`, fault control 4/4 `MISMATCH` |
| `-` menu | **done in build 18:45** (tap Minus, d-pad, A); seen working on the console 2026-11-25. Build 19:49 adds **PERF LOG** (on by default) and **GPU TIMING** rows (README 10, PERF_HANDOFF 9.52) |
| Perf instrumentation | build 19:49: `[pm*]` lines every second + `[hitch]` lines; read a drop with `py -3 switch/tools/perf_report.py "<drop>/log.txt"` and `prof_report.py` for the profiles. **The user changes settings only from the `-` menu** (they cannot edit files), so every A/B switch needs a menu row |
| SD layout (2026-11-26) | root = `RL-NX.nro` + disc only; `config/` (`settings.cfg` = the one options file: menu rows written by the game plus any other `R2_*` line, kept as written; `boot.cfg`/`menu.cfg` are gone; Aurora `input.json`/`imgui.ini`), `cache/` (`deko/`, `aurora/`, `update.nro`), `logs/`, `saves/`. No migration: the user moves files on the console, we move them in Ryujinx |
| FRAME OVERLAP (Stage 7) | `-` menu row, on (`R2_FRAME_OVERLAP`): `extern/aurora/lib/gx/fifo.cpp` two buffers + boundary queue, `aurora.cpp` end_frame_async (from SFA-NX); the bridge worker no longer waits for the FIFO thread at frame end. Backups `build-switch/tmp/fifo.cpp.pre-overlap`, `aurora.cpp.pre-overlap`. Ryujinx ok; hardware unmeasured; Hardware 17:09, level 22: 35.2 fps on vs 34.0 off (about -1.1 ms/frame) |
| NATIVE MTX (Stage 9) | `-` menu row, on (`R2_NATIVE_MTX`): `src/runtime/hle_mtx.cpp` PSMTXConcat 7FC90610, PSMTXMultVec 7FC8F9F0, PSMTXInvRigid 7FC904DC on host pointers, same rounding as the gen; non-contiguous operands run `_orig`. Turn off for oracle runs; Hardware 17:09: matrix time 0.84 -> 0.43 ms/frame |
| VM CENSUS (Stage 9) | `-` menu row, off (`R2_VM_CENSUS=1`): rdSlow/wrSlow record return addresses, `logs/vmcensus.txt` every 30 s; `py -3 switch/tools/vm_census.py <vmcensus.txt> --elf <that build's elf> --out config/GSWE64/vmsites.txt` then regenerate gen. A hardware level census beats the Ryujinx attract one; Hardware census 17:53 (641 s, 3 levels) merged at --min 20000: 969 sites (gen 18:02, 176314 instructions with V forms) |
| Code audit | `py -3 switch/tools/code_audit.py <log folder> <elf> <gen dir> <top N> <out>`: main-thread samples of the hottest gen functions by bucket. 17:09 (log 1729, top 300 = 88 % of gen, 10.1 host instructions per guest): guest memory access 41 %, ABI boundary (ctx loads/stores at entry, exit, call sites, s2d/d2s of float regs) 19 %, host spills 13 %, guest branches 6 %, VM TLB test 6 %, CR/fcmp 6 %, pipe writes 4 %, real ALU/FP 3 %. Samples sit on loads (ldr/ldp/ldur 49 %): memory-latency bound, not instruction-count bound. The elf has no inline chains (addr2line -i gives the innermost line only) |
| NATIVE MOVIE (Stage 10) | `-` menu row, on (`R2_NATIVE_MOVIE`): `src/runtime/hle_movie.cpp` replaces the .movie frame functions MovieFrame512 80055DA0 / MovieFrame640 80055E30 / MovieFrameAny 80055EC0 (r7 width) (hooks.txt hle+abi) with libjpeg (devkitPro libturbojpeg.a, raw_data_out) and tiles Y/Cb/Cr into the guest's GX I8 planes. Factor 5 frames are NOT byte-stuffed (bare FF in entropy data, so a bare FF D9 can occur mid-frame): the source manager reads guest memory from ctx+0x0C, inserts 00 after each FF past ctx+0x6C, and jpeg_abort_decompress ends without EOI. Any libjpeg warning, non-4:2:0, size not x16, unmapped plane -> `_orig`. `R2_MOVIE_CHECK=1` (boot.cfg) decodes both and logs [movie] diffs (+ frame 30 dump to logs/movie_*.bin). Ryujinx title movie 512x336: 99 % of samples within +-1 of the game, bias -0.25 (game truncates). Build 17:48; hardware unmeasured |
| `-` menu tabs (2026-11-27) | Two tabs, L/R or d-pad left/right to change: **Game** (FRAME RATE, INVERT FLIGHT, RUMBLE, HD TEXTURES, GAME LOGGING, UPDATE) and **Debug** (every other row), Debug hidden unless `R2_DEBUG_MENU=1` (boot.cfg, or menu.cfg where the menu keeps it once set; SFA-NX's `debug_menu=1` pattern). A switches the row (right no longer does). Panel is centred, double border, footer naming the buttons; scale follows the swapchain height (`swdk::surface_size`). Rows saved in menu.cfg are unchanged except `R2_HD_LIMIT` is gone and `R2_INVERT_FLIGHT`, `R2_RUMBLE`, `R2_GAME_LOG` are new |
| Game rows (2026-11-27) | INVERT FLIGHT (off): `input.cpp` negates port stickY while `gyro::flying()` (hle_game's flight flag). RUMBLE (on): off sends every pad `PADControlMotor(0)` and drops game rumble. GAME LOGGING (off) = SFA-NX's Extra Logging, `rl_log_set_extra` (leak finder, 2 s LOGIO lines). Hardware unconfirmed for all three |
| In-game updater (2026-11-27) | `-` menu Game tab UPDATE row, `switch/src/updater_switch.cpp` (SFA-NX's, GitHub API `digdat0/RogueLeaderNX` releases/latest, asset `RL-NX.nro`, tag vs `RL_NX_VERSION`; no release or not newer = nothing to download). A: check, then download to `cache/update.nro`, then install + restart. Restart = SFA-NX's: `rlnxRelaunchRequested` -> `ensureFrame` (frame boundary) -> `bridgeShutdown`, `relaunch_switch.cpp` (`input`/`events`/`audio` threads stopped, `aurora_shutdown`, `kartpad_end_threads()==0`, `aurora_switch_dk_release_all`, `rlUpdaterReleaseNet`, `kartpad_exit_to_loader`); any thread left, or the profiler's libnx threads ran = plain exit (update installed, start RL-NX again). **Relaunch never exercised on hardware** (Ryujinx has no hbloader) |
| ASYNC VBLANK (audit 2026-11-27) | `-` menu row (Debug tab), **on by default since the 10:44/10:50 hardware A/B: 41.4 vs 38.1 fps, `gxsync` 5.14 to 0.13 ms** (`R2_ASYNC_VBLANK=0` off; a menu.cfg saved with the old default 0 keeps it off until the row is switched): `vblank()` no longer drains the bridge ring every frame (that was the 3.6 ms of level-22 gxsync, all in `[pm-drain] state`); the game thread's hotkeys/input and the worker's `pollEvents` share `s_inputMx` instead. A pending menu switch, an open menu, a fired hotkey (`hotkeys::fireCount`) still drain (cause `vblank` in `[pm-drain]`). Thread-safety unproven on hardware; Ryujinx smoke only |
| DSP FAST (audit 2026-11-27) | `-` menu row, on (`R2_DSP_FAST=0` off, `R2_DSP_VERIFY=1` runs both and logs `DSP FAST: MISMATCH`): `musyx_dsp.cpp` resampleFast/fetchFast, the same arithmetic as resample/fetch with the voice's PB words in locals, plus a split-free bus ramp. Host test: 2,000,000 random subframes (ADPCM/PCM16/PCM8, sel 0-2, loops) 0 mismatches; Ryujinx verify run 0 mismatches. Hardware gain unmeasured |
| CP LAG (Stage 11) | `-` menu row, on (`R2_CP_LAG`): with GP RAW on, CP register reads no longer drain the bridge (that drain was the +3 ms gxsync that made GP RAW neutral on hardware, 1532 vs 1537); `gx_fifo.cpp` records (ring position, read pointer) per chunk handed to the worker and a CP read reports the read pointer the worker has consumed (`gxSetGpProgress`). **The 17:29 log had `R2_GP_RAW=0` saved in menu.cfg**, so its walker (cmdLength/walkBuf/cpRun ~1.8 ms) ran on the game thread; gxInit now logs when R2_GP_RAW overrides the bridge's start state |
| VM census columns (Stage 11) | `logs/vmcensus.txt` lines are now `ra vm_rd vm_wr other_rd other_wr` (other = uncached view, IO, gather pipe through a plain store; the locked cache is left out because FASTMEM maps it); `vm_census.py` reads columns 2-3 as before and columns 4-5 with `--fastmem` (writes `config/GSWE64/fmchecked.txt`, min 1) |
| FASTMEM NRO (Stage 11) | `RL_NX_DEKO=1 RL_NX_FASTMEM=1 switch/build.sh 8` with a `R2RECOMP_FASTMEM=1` gen: `switch/src/fastmem_switch.cpp` reserves a 4 GiB window (virtmem) and maps the arena at +0x80000000 and the locked cache at +0xE0000000 through code memory (MapOwner RW); ctx.h memBase() = window, F forms = window + address, no test. Other addresses fault: `switch/src/fastmem_entry.S` replaces libnx's weak `__libnx_exception_entry`, saves the context below the faulting sp, resumes in `rl_fm_tramp` (ordinary context) which calls memory_switch.cpp `rl_fm_slow` (ESR syndrome or instruction decode -> rdSlow/wrSlow), then a `udf` restores the context past the access. Undecodable -> the instruction re-faults into libnx's handler (crash report). `[fastmem]` lines every 30 s + `logs/fmcensus.txt` (feed it to vm_census.py --fastmem --merge). Relaunch unmaps it (`rlnx_fastmem_teardown`). F forms only in the paged game modules (0x7E000000-0x7FFFFFFF; the DOL's SDK reaches CP/PE/PI through pointer globals). **Not shipped**: Ryujinx maps the window but kills the process at the first fault (InvalidMemoryRegionException), so the fault path is untested; and the game modules read their own paged globals through VM pages, which cannot be aliased into the window (code memory makes the source pages inaccessible), so every VM site a census missed faults on each use (Ryujinx 19:53 died at guest 7FDEE000 at boot). Builds in `build-switch/tmp/RL-NX-fastmem-19{44,49,53}.nro`; `config/GSWE64/fmchecked.txt` = other + all VM sites (hardware 17:53 + Ryujinx 19:32 census, min 1) |
| HD TEXTURES (Stage 12) | `-` menu row HD TEXTURES (Game tab, on, `R2_HD_TEXTURES`); the HD TEX 2048 row was removed 2026-11-27, the 2048 limit stays the default and `R2_HD_LIMIT=0` in boot.cfg lifts it: Dolphin-format packs (tex1_*.dds/.png, any layout) under `sdmc:/switch/RL-NX/mods/hd` go to Aurora's replacement registry (`gx_bridge.cpp` hdApply, load_replacement_directory). HD TEX 2048: `extern/aurora` texture_replacement.cpp `set_replacement_max_size` drops top mips above 2048 px (the GSWE64 pack is 1,184 DDS, 6.1 GB, up to 4096x4096 DXT5/BC3 with full mips, 22 MiB each). RL-NX: `switch/tools/hd_pack_shrink.py <pack> <out>` cuts it offline without loss (keeps the pack's own mip levels up to GameCube size x4, max 1024; opaque textures BC3 -> BC1): 6.05 -> 0.78 GiB, output verified level-identical; converted pack at `E:/RogueSquadronII/HD-Switch` (hardware load time unmeasured) |
| GP RAW (Stage 6) | `-` menu row, on by default (`R2_GP_RAW`, build 14:45, `debug/RL-NX-1445.elf`): with the GP thread on, cpRun hands the raw CP bytes to the bridge worker, whose parseChunk splits them (s_cmd/s_vtx) and runs called display lists from guest memory; the game-thread walker (walkBuf/cmdLength/walkDl) is skipped. Every gxCpRead drains the worker first (gxSetGpDrain = bridgeDrain), so the game never sees the read pointer past a list the worker has not run. Off = the 9.57 walker. Pre-change files in `build-switch/tmp/pre-stage6` |
| GPU clock (2026-11-26) | **CPU BOOST row removed** by the user's rule (no CPU past retail in the port; players use their own OC tool). `switch/src/clock_switch.c` `rlnxClockInit` (first game frame, menuDraw) asks apm for the retail handheld profile 0x92220008 (CPU 1020 / GPU 460.8 / EMC 1331.2), fallback 0x00020004 (GPU 384), Normal mode only (docked untouched), logs a `[clock]` line; `rlnxClockRestore` puts the saved configuration back and closes apm on both exit paths. Always on, no menu row (documented in `switch/README.md` Clocks) |
| `settings.cfg` (was `boot.cfg` + `menu.cfg`, merged 2026-11-27) | `config/settings.cfg`, **not needed for play**: the zfreeze guard is on by default (`R2_ZFREEZE_GUARD=0` = off) and FPS is a saved menu row. Use it only for diagnostics (oracle, `R2_PROFILE`) |
| Git | none, by the user's instruction. No commits, no pushes, ever, unless asked |

The `R2_PROFILE=200` on the card is left over from the last hardware run: it profiles for 200 s on
every boot and writes `logs/profile-NNN.txt`. Strip it before handing the card back for play.

---

## 2. Build, run, verify

Everything runs in Git Bash on Windows, from the project root. devkitPro/devkitA64 is installed.

```bash
# correctness build (headless: guest + Aurora, no screen) - fastest, use for oracle/emitter work
switch/build.sh 8

# the build that goes to the console (deko3d present path) - ~4 min incremental, longer cold
rm -f build-switch/librl_entry.a && RL_NX_DEKO=1 switch/build.sh 8 > build-switch/logs/build-deko.log 2>&1
tail -3 build-switch/logs/build-deko.log     # must end 0 errors

# diagnostic builds (compile a hook into the generated code; ordinary builds carry neither)
RL_NX_ORACLE=1 switch/build.sh 8             # R2_ORACLE probes
RL_NX_ICOUNT=1 switch/build.sh 8             # R2_ICOUNT metering
```

Keep the `rm -f build-switch/librl_entry.a` in that command: it is part of the recipe every recorded
number was built with. The Clang entry library is the one that includes `ctx.h`, and a stale archive
has shipped old code before, which is how the recipe got the `rm`.

`switch/build.sh` re-creates `build-switch/aurora-patched` and `build-switch/aurora-deko` from
`extern/aurora` + the staging scripts on **every** run. Those two directories are build products:
never edit them, edit `extern/aurora`, `switch/deko`, or `switch/builder/integrate-deko.py`.

**Emulator check** (correctness only, see section 3):

```bash
py -3 -I C:/Users/Steve/.claude/skills/gc-decomp-switch-port/scripts/ryujinx_smoke.py \
  --ryujinx E:/ryujinx_test --app RL-NX \
  --nro build-switch/sdcard/switch/RL-NX/RL-NX.nro --seconds 60 --out build-switch/tmp/smoke
```

What a good smoke looks like: `[frame]` lines with draws climbing (`1,267 draws / 4,2xx merged` in
the attract loop by 60 s), `hashed 0 of N, missed 0`, audio up with 0 dropped buffers, no fatal. It
does **not** present (Ryujinx's layer refuses deko3d's swapchain), so the frame rate it shows is
meaningless.

**Hardware** is the user's: they copy `build-switch/sdcard/switch/RL-NX/` to the console and send
back `logs/log.txt`. Only their runs count as speed measurements.

---

## 3. Rules that keep the port honest

**Code**

- Every edit is commented, and edits inside upstream files are prefixed `// RL-NX:`. Never put
  comments inside Aurora's WGSL raw strings (they are hashed into the shader cache).
- Never hand-edit `build-switch/aurora-patched`, `build-switch/aurora-deko`, or `gen/`'s structure.
  Aurora changes go in `extern/aurora` or the staging scripts; `gen/` is written by the recompiler.
- Any behaviour change that is not proven goes behind a switch that defaults **off**.
- Do not break three fixes: the locked-cache `hp(a)` mapping in `ctx.h` (`hpr`/`hpw` must stay on
  `hp`), `R2_ZFREEZE_GUARD` (the in-level flicker fix), and audio on the guest thread (a standing
  decision - see section 5).
- `switch/src/menu_switch.cpp` may not include `gx_bridge.h` or `runtime.h`: both pull `ctx.h`, which
  needs Clang (`__builtin_rotateleft32`) and this file is built by the other compiler. It declares
  the two entry points it needs itself. Keep it that way.

**Claims**

- A performance claim names a build, a scene, fps **and** ns per guest instruction. Without a
  hardware log it is a hypothesis, and it is written down as one.
- Ryujinx proves correctness, never speed: it emulates the CPU and cannot time the guest. Measure
  hashing in **bytes** (`hashed KiB first/net/probe/copy`), work in **counts**, and leave the
  milliseconds to the console.
- Run-to-run state hashes do not compare: the guest's timeline is wall clock, so two runs of one NRO
  disagree at the same frame (`PERF_HANDOFF` 9.46). For codegen changes use the oracle.
- Scripted `R2_INPUT` does not leave the attract scene in Ryujinx (tried: 4,080 frames, still 1,267
  draws), so in-level behaviour cannot be reproduced there. Say so instead of claiming a level test.

**Process**

- No git repo, no commits, no pushes, no deploys, no uploads. No game data in the repo, ever.
- Do not modify the upstream Red Two files (`src/`, root `CMakeLists.txt`, `README.md`) beyond the
  `// RL-NX:` marked edits that already exist.
- Keep temporary scripts and captures in `build-switch/tmp/`, and delete them when done.

---

## 4. Task A - finish the `-` in-screen menu

> **2026-11-25, build 18:45: done except the on-screen check.** The two edits below are in, plus four bugs
> they would not have caught (static-init `R2_MENU_OPEN`, Minus being the save-state modifier, edges lost
> between game frames, rows starting at OFF). The stall hypothesis 1 is confirmed: the stalls precede the
> panel. What remains is one hardware run with `COS_DK_CAPTURE=1` and a hand on the pad. Details:
> README 7.2 and PERF_HANDOFF 9.50, which also reviews the section 5 queue (read it before B4/B5).
> The user confirmed the panel works on hardware (18:45).
>
> **2026-11-25, build 19:08: the Tatooine Training crash (README 8.11).** It was a kickoff storm during the
> level-load upload burst (thousands of submits in one frame, no GPU wait), not a GPU hang. The render worker
> now waits for the GPU every 4 batches in a frame (`R2_GPU_BURST_WAIT`, `R2_GPU_BATCH_ENTRIES`). Ryujinx smoke
> passed. **Hardware untested:** the next step is the user loading Tatooine Training. Read the `GPU burst:` lines
> in that log. Details: PERF_HANDOFF 9.51.

### What already exists (do not rewrite it)

| piece | location | state |
|---|---|---|
| Panel, rows, state, counter, `R2_MENU_OPEN` | `switch/src/menu_switch.cpp` | written, compiles |
| Per-frame hook (input + FPS window) | `gx_bridge.cpp:445`, `menuDraw()` in `endFrame()` | **live** |
| Draw hook | `switch/deko/device.cpp:1303`, `if (false) rl_nx_menu_draw();` | **disabled** |
| Panel drawing primitives | `switch/deko/pattern.cpp`: `draw_text` (3x5 font, 768-cell budget: `rows = min(count, 768/columns)`), `draw_rect` | work (the boot report uses them) |
| Pad edges | `src/runtime/hotkeys.cpp`: `padEdge()` reads `s_padEdge`, filled only by `pollMenuPad()` (line 307) - **which nothing calls** | dead |
| Input mute while open | `runtime.h:189 bool menuOpen()`, used by `input.cpp:779` and `hotkeys.cpp:337` | live |
| CMake | `switch/CMakeLists.txt:181` already lists `menu_switch.cpp` | done |

Rows today: FRAME RATE, STATS LOG (`rt::gfx::setStatsEnabled`), PROFILER (`rlProfSetEnabled`). Minus
toggles the panel, d-pad moves, A/right switches. All of it is unverified on a screen.

### The two edits

**1. Give the edges exactly one SDL query per frame.** `hotkeys::tick()` already builds the frame's
pad set:

```cpp
const bool enabled = !gfx::menuOpen() && !s_cap.active && (!automated() || test);
const Set held[kDevices] = {enabled ? heldPad() : Set{}, enabled ? heldKeys() : Set{}};
```

`heldPad()` walks every gamepad (`hotkeys.cpp:95`). The old menu version called it a second time from
the present path and the guest stalled. Do not add another call and do not add a second libnx
`PadState` (that measurement is in the file's comments). Instead: query once, share it -

```cpp
const Set padNow = heldPad();                 // one query for both
pollMenuPad(padNow);                          // edges for the - menu, even while it is open
const Set held[kDevices] = {enabled ? padNow : Set{}, enabled ? heldKeys() : Set{}};
```

`pollMenuPad()` takes the set instead of calling `heldPad()` itself, and the stale comment at
`hotkeys.cpp:321` ("the - menu polls once a frame on its own path") goes away - there is no such
path. `menu_switch.cpp` needs no change here: it reads `hotkeys::padEdge` already.

**2. Turn the drawing on, and do not leak its state.** In `device.cpp`:

```cpp
rl_nx_menu_draw();
set_view(0, 0, R.width, R.height);   // the panel sets its own viewport; restore the frame's
```

That call sits inside `picture(user, 1)` .. `picture(user, 2)`; if `picture(user, 2)` records more
draws rather than only submitting, draw the panel **after** it instead. Whichever you choose, say
which in the commit-equivalent note, because it is the difference between a panel over the game and a
panel that clips the next pass.

**3. Make both halves switchable for the bisect, then remove the switches.** Add
`R2_MENU_DRAW` (guards the `device.cpp` call) and `R2_MENU_PAD` (guards `pollMenuPad`), each
`0` = off. One build then covers the whole matrix through `boot.cfg`:

| draw | pad | what it isolates |
|---|---|---|
| 0 | 0 | baseline - run this **first** and write the numbers down |
| 1 | 0 | the panel's recording (stream, viewport, text shader) |
| 0 | 1 | the input path alone |
| 1 | 1 | the feature |

Compare, per 60 s smoke: the `[frame]` draw count at 30 s and 60 s, the `[stall]` lines and their
timestamps, `aurora: N draws` in the final line, `hashed N of M`, and presents/s. The panel never
needs a hand to be open: `R2_MENU_OPEN=1` opens it on the first frame.

### Why this is not just "flip the bool"

The first build polled a second libnx `PadState` and the guest went quiet for 4 s at a time - three
times in 65 s. Moving to SDL edges did not remove the stalls, and neither did disabling the draw;
both reverts still ended smokes on `aurora: 1 draws` with `[stall] guest quiet`. So the menu is
neither convicted nor cleared, and the honest position is that the cause is unknown. Three
hypotheses, cheapest test first:

1. **The `[stall]` line is the attract scene, not the menu.** Black transitions between demo scenes
   also submit nothing for 4 s. Test: the baseline row of the matrix above; if it stalls too, the
   menu is innocent and the watchdog needs `R2_CAPTURE_MIN_DRAWS`-style filtering, not a fix.
2. **The extra pad query.** `heldPad()` allocates and sorts a set per call, and the old code called
   it after the frame's commands were recorded. Test: the `0/1` row, with one shared query as above.
3. **The panel's transient stream or pipeline.** `draw_text`/`draw_rect` call `stream_alloc` and
   `bind_2d_state` inside the pass, at the point where the frame's stream is closest to full. Test:
   log `[menu] draw` once per frame with the stream offset, and try drawing at the start of the frame
   instead of the end. If `stream_alloc` returns nothing there, it will draw nothing - so log that
   too.

Do not report the stall "fixed" on the strength of one run. It is intermittent, so it needs two runs
of the fixed build and the same two of the baseline.

### Acceptance

- `COS_DK_CAPTURE=1` writes a PPM in `logs/deko-capture/` (or `R2_CAPTURE_ON_BREAK=1` a
  `break-NNNN.ppm`) in which the panel is visible and legible - that is the screenshot, since there
  is no other way to see the deko path off a console.
- Minus opens/closes (`[menu] perf: panel open` / `closed` in the log), d-pad moves the cursor, A
  toggles, and each row produces its own line plus its effect (`[stats]` lines appearing/disappearing,
  `profile-NNN.txt` starting).
- While open, the game does not react to the pad; after closing it does again.
- Two 60 s smokes: no new `[stall]` and draw counts within a percent of the baseline.
- Both builds still end `0 errors`; record the new NRO size.

### After it works

Rows the port actually needs, in this order: the texture-hash A/B (`R2_TEX_HASH_EVERY`), the oracle
status of the build, the thread placement policy (`perf.cfg`), and `R2_FPS` - decide whether the
panel's counter replaces the top-middle one or they coexist (today they would duplicate).

---

## 5. Task B - continue the optimisation

### The arithmetic that decides the order

47.35 ms a frame at 6,100-6,800 draws. Port 17.38 ms (36.7 %): GX decode + replay 8.44, texture
hashing 2.35, audio 2.11, IPC 1.87, FIFO copy 0.75, the rest small. Guest 25.61 ms (54.1 %): 1,097
sampled functions, the largest 1.263 ms - a long tail, nothing hot enough to fix one function at a
time. Remove the port entirely and it is ~39 fps; remove the guest and it is ~57 fps. **60 fps needs
a 2x codegen improvement, so the target to hold is a stable 30** (Phase 1+2 land ~32.5 ms).

**Audio stays on the guest thread.** `PERF_PLAN` Phase 1 says so by decision, and
`switch/src/audio_switch.cpp:8` is built that way (its `push()` runs on the DSP's block thread; there
is no audio thread to wire). 9.44's table lists "audio thread ~3.4 ms" as a candidate; that list is
not the plan. Do not move audio without the user asking.

### The queue

**B1. Read phase 1b in level (no code, do this first).** `R2_STATS=1` on the console, level 1, read
`hashed N of M` and `hashed KiB first/net/probe/copy` from `[stats]`. Expected after 9.49: most frames
take only the 512-byte fingerprint, the net (1 in `R2_TEX_HASH_NET` = 64) does the full hashes, and
`first`/`copy` are the only large numbers. If `copy` is large, EFB copy destinations are still hashed
every frame - that is the next candidate, and 9.49 already instrumented it for exactly this. Update
`PERF_HANDOFF` with the numbers either way.

**B2. Phase 1a - display-list translate cache** (`PERF_PLAN` phase 1, ~2.3 ms). Translate a guest
display list once, keyed by address plus a validity token, and replay it with patched uniforms.
Sources: `execute`/`cmdLength`/`cpRun` in `src/runtime/gfx/gx_bridge.cpp` (the 8.44 ms total; those
three are 2.3). Acceptance: draw counts identical to the baseline on the same scene, guest thread
~2 ms lighter per frame, `[stats]` unchanged otherwise. A wrong cache shows up as missing or stale
draws, so compare a 60 s smoke's per-frame draw series before and after.

**B3. Phase 1c - FIFO copy** (~0.75 ms plus part of `onCommand`): the guest fills its own linear
buffer, the FIFO thread drains it. This is also the item that makes a mid-frame cut cheaper.

**B4. Phase 2 - a Switch-native GX -> deko3d per-draw path.** The big one: cached display lists
become cached deko3d command buffers, one pipeline per TEV configuration from a precompiled shader
cache, no per-draw validation. Acceptance in the plan: port <= 6 ms; with B2/B3 the frame lands
24-26 ms (~39 fps), which is today's ceiling.

**B5. Phase 3 - the LLVM backend.** The only route past 39 fps. Emit LLVM IR per guest function: GPRs
as SSA, one reserved host register for the RAM base, carry/CR as plain i32s, `-flto` across the guest
archive with `lld` (GCC's `ld` cannot take bitcode; NRO packaging is unchanged). **Gated on the
oracle**: `RL_NX_ORACLE=1 switch/build.sh 8` + `R2_ORACLE=<n>` must keep reporting `ok`, and
`R2_ORACLE_FAULT=1` must still fail. Milestones in the plan: parity -> <=17 host insns/guest insn
(-20 %) -> <=12 (-40 %).

### Do not re-open these (measured, with the number)

| idea | result |
|---|---|
| hot-call clustering + `__attribute__((flatten))` | 0 to -3 % on hardware (9.40-9.43); the code is still there, off by default |
| raised `-inline-threshold` | 1.34 MB of `.text` for no measured gain; default removed (9.46) |
| locals emitter (guest GPRs as locals) | two builds booted but never submitted a draw; parked (9.45) |
| frame-driven deterministic clock | deadlocks the boot at 4.18 s; switch left inert (9.47) |
| run-to-run state hash as a gate | same NRO, different hashes at one frame; use the oracle (9.46) |
| "the per-draw cost is the port's fault" | `onCommand` is 0.69 us a draw and was 4.5 ms at both 836 and 6,600 draws: it charges for vertex bytes, not commands (9.44) |
| threading | the guest thread is 100 % busy with no wait in its top twelve; FIFO and render wait 87/82 % (9.35, 9.42) |

If a new idea lands in the same family as one of these, expect it to measure zero and say why it is
different before spending a build on it.

---

## 6. Recording the work (required, not optional)

Every change, in the same turn:

1. **`docs/PERF_HANDOFF.md`** gets the next `## 9.5x` entry: what changed, why, what was measured
   (build size before/after, the exact log lines, draw counts), and an explicit **"verified / not
   verified"** split. Existing entries are the template - they are valued for saying what they did not
   prove.
2. **`README.PORT.md`** gets the status it changes: the section 7 table row, and the prose in 7.1
   (optimisation) or 7.2 (the menu). Its section 10 switch table gets any new `R2_*`.
3. **`docs/PERF_PLAN.md`** changes only when the *order* changes, not per result.

Numbers to always write down with a build: NRO bytes, `[stats]` line, `[frame]` draws, `presents/s`,
whether audio underran, and the ELF name kept in `debug/` for an A/B (`debug/RL-NX-HHMM.elf`).

---

## 7. Traps that have already cost days

- **The band, the double menus, the missing movies, the locked cache.** All four were "the picture is
  wrong" with different causes (`README` 8.1, 8.7, 8.8, 8.9). The lesson: a rendering symptom is
  usually a wrong number in the port, not a GPU behaviour - check the guest-side data path first.
- **`R2_STATS_BOUNDS`** costs 6.7 ms a frame (9 % of the guest thread) and was measured on for weeks.
  It is opt-in now; do not put it back in the default diagnostics.
- **`draw_text` has a 768-cell budget.** Lines longer than `768/rows` characters are dropped silently.
  The panel fits; a longer row will not.
- **`switch/build.sh` does not forward every variable** - `CACHE` was silently dropped once and
  repeated a failed experiment. Read the variable you set in the log's CMake line.
- **The deko staging is regenerated on every build**, so a "build did nothing" is usually a stale
  archive, not a cache hit (`rm -f build-switch/librl_entry.a`).
- **Ryujinx never presents deko3d's swapchain**; `COS_DK_CAPTURE=1` is the only view of the picture
  there, and `--wrap` diagnostics (`switch/deko/host/`) only exist on hardware.
- **The SD copy can be out of step with the build.** The smoke script and the console both use
  `build-switch/sdcard/switch/RL-NX/RL-NX.nro`; `build-switch/RL-NX.nro` is what the build writes.
  Compare the two sizes after every build, and never assume the card has the code you just built.
- **Hardware logs are the only speed data.** Ask the user for `logs/log.txt` rather than reasoning
  from the emulator.

---

## 8. First hour

1. Read `README.PORT.md` sections 7, 7.1, 7.2 and `PERF_HANDOFF` 9.42-9.49. Skim the rest.
2. Build the current tree (`RL_NX_DEKO=1`, command in section 2) and confirm `0 errors` and
   40,706,902 bytes. If the size differs, `gen/` changed - find out why before going on.
3. Run one 60 s smoke and save the log. Write down: `[frame]` draws at 30/60 s, `[stall]` count,
   `hashed`, audio dropped, and the last line. This is the baseline every later A/B is against.
4. Run the same smoke once more and compare. If the numbers move, the tree is not the state described
   here, and the difference matters more than either task.
5. Only then start section 4 (the matrix's `0/0` row is step 3, already done).

---

## 9. Definition of done for this handover

- The `-` menu opens, draws legibly, switches its three rows, closes, and the game's pad works again;
  the stall question is either answered in the log or explicitly still open, with the two-run
  comparison written down.
- The optimisation queue has a measured step recorded in `PERF_HANDOFF` 9.5x, with the next step
  named.
- `README.PORT.md` 7.1/7.2 and section 10 match the tree, the card's `boot.cfg` is back to the
  minimal performance pack, and no temporary files are left outside `build-switch/tmp/`.
