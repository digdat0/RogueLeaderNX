# RL-NX performance and playability hand-off (from build 09:25, 2026-11-23)

These are small, separate tasks for a coding agent. Do them **in order, one task per build**. Each task
names the file and line, the exact change, how to check it, and how to undo it. Line numbers are from
build 09:25. If a line has moved, search for the quoted code instead of trusting the number.

---

## 0. Rules (read before any task)

1. **No git repository.** Before editing a file, copy it to `build-switch/tmp/bak/<name>.<task>`.
   Undoing a task means copying that file back.
2. **Comment every edit** with a `// RL-NX:` comment that says what changed and why, e.g.
   `// RL-NX: T1 fast path - normal floats convert exactly in hardware (hardware 08:56: s2d 4.9% of main thread)`.
3. **Never edit** these folders:
   - `build-switch/aurora-patched`
   - `build-switch/aurora-deko`
   - `build-switch/gen`

   The build regenerates or re-stages them.

   - Aurora changes go in `extern/aurora`.
   - Generated-code changes go through a script like `switch/tools/patch-gen-restrict.py`.
4. **Never** put comments inside WGSL shader strings (`R"(...)"` raw strings in Aurora or `kSeamShader`). That
   changes shader hashes and silently invalidates shader caches.
5. **Keep 1280x720 in handheld.** Do not lower resolution, and do not change CPU/GPU clocks by default.
6. **Do not commit, push or publish anything.** Do not add game files (disc images, extracted data) to the repo.
7. **Make risky changes switchable** with an environment flag read once, e.g.
   `static const bool off = envFlag("R2_NO_T2");`. Say the flag name in the comment and in §3 of this file.
8. **Ryujinx proves correctness, never speed.** Speed is judged only from a hardware profile that the user
   supplies.

## 1. The build-and-check loop (do this after every task)

1. Build from Git Bash:
   ```
   cd "/e/Red Two/RL-NX" && RL_NX_DEKO=1 switch/build.sh 8 > build-switch/tmp/build.log 2>&1; echo rc $?
   ```
   - `rc 0` is required. On failure, read `grep -n "error" build-switch/tmp/build.log | head`.
   - A change to `src/runtime/ctx.h` rebuilds all 96 generated files and takes several minutes. Run it in
     the background.
   - The output is `build-switch/sdcard/switch/RL-NX/RL-NX.nro`, plus `debug/RL-NX-HHMM.elf` (keep it).
2. Smoke test in Ryujinx (120 s, boots to the attract mode):
   ```
   py -3 -I C:/Users/Steve/.claude/skills/gc-decomp-switch-port/scripts/ryujinx_smoke.py --ryujinx E:/ryujinx_test --app RL-NX --nro build-switch/sdcard/switch/RL-NX/RL-NX.nro --seconds 120 --out build-switch/tmp/smoke
   ```
   Pass means all of the following:
   - `no crash or fatal reports`;
   - the log's last line is not a `fatal`;
   - the log contains `GX processing: Aurora's FIFO thread`;
   - the `deko-capture` frames look like the previous build's (same picture, nothing doubled, black or
     stretched).

   Profile warnings ("frames over 20 ms") are normal in Ryujinx. Ignore them.
3. For tasks marked **[HW]**, the gain must be confirmed on the console by the user.
   - Ask the user to set `R2_PROFILE=60` in `sdmc:/switch/RL-NX/boot.cfg`.
   - Ask them to play the same route (title → first mission).
   - They put the logs in a new folder `E:/RL-NXLOGS/LOGS MMDDYYYY-HHMM`.
   - Read the profile with:
     ```
     py -3 -I switch/tools/prof_top.py "E:/RL-NXLOGS/LOGS <folder>/profile-002.txt" debug/RL-NX-<HHMM>.elf
     ```
     The elf must match the `build` line at the top of the profile.
   - Compare against the baseline table in §2. Record the result in §3.

## 2. Where the time goes (hardware, build 08:56, title/attract, ~10 fps)

The main thread spends ~97 ms per game frame. From `prof_top.py`, as a share of main-thread samples:

| Share | Function | Where |
|---|---|---|
| 4.9% | `rt::s2d` (single→double for every `lfs`/`psq_l`) | `src/runtime/ctx.h:156` |
| 4.5% + 3.8% + 1.1% | `dkCmdBufBindColorState`, `CmdList::moveTo`, `dkCmdBufBindColorWriteState` | `switch/deko/backend_commands.cpp:431-433` (now on the GX thread since 09:25) |
| 2.5% | GX FIFO `execute` | `src/runtime/hw/gx_fifo.cpp:130` |
| 2.2% + 1.3% + 1.3% | `memPtr`, `isIo`, `crossesPage` (every guest load/store) | `src/runtime/ctx.h:47-136` |
| 1.7% | `hle_GameFrameDelta` | `src/runtime/hle_game.cpp:129` |
| 1.6% | `seamDraw` (menu seam pass) | `src/runtime/gfx/gx_bridge.cpp:1256` |
| ~60% | recompiled game functions `f_XXXXXXXX` | `build-switch/gen/code_*.cpp` |

Build 09:25 already added:
- `Ctx& __restrict c` in the generated code;
- the store-watch check compiled out on Switch;
- Aurora's GX thread as the default.

So the first hardware profile of 09:25 is the new baseline. Write its numbers in §3 before judging any
task below.

---

## 3. Results log (fill in as you go)

| Task | Build | Ryujinx pass | HW main-thread ms/frame | Flag to turn off | Notes |
|---|---|---|---|---|---|
| baseline | 09:25 | yes | hardware 10:33 log: mission 2 (level 22) 14-24 fps, game thread busy 100%, 100-5100 draws/frame | `RL_NX_NO_RESTRICT=1` at build, `R2_GX_THREAD=0` | profile-004.txt in `E:/RL-NXLOGS/LOGS 11232026-1033` is the first 09:25 profile |
| menus doubled fix | 10:38 | yes (menu frame 501 correct) | (user) | `R2_NO_PROJ_REFOLD=1` | not a speed change; README §8.7 |

---

## 4. Tasks: game thread CPU

### T1. `s2d` fast path for normal floats  (expected 3-4% of main thread) [HW]

- **File:** `src/runtime/ctx.h`, function `inline double s2d(u32 x)` (line ~156).
- **Why it is safe:** for every float whose exponent field is 1..254, the AArch64 `fcvt` from single to double
  is exact and gives the same bits as the existing bit-twiddling. Only zero/denormal (exp 0) and
  Inf/NaN (exp 255) need the slow path; NaN needs it to keep SNaN payloads.
- **Change:** insert as the first line of the body, after `const u32 exp = ...` is computed. Move that line
  up if needed.
  ```cpp
  if (exp - 1u < 254u) [[likely]] return double(bflt(x));  // RL-NX: T1 normal floats: hardware fcvt is exact
  ```
  Keep the rest of the function unchanged.
- **Check before building the NRO:** write a scratch PC program (not in the repo) that includes a copy of the
  old and new `s2d` and compares bits for all 2^32 inputs. It must report 0 mismatches. Use `py -3` or
  any host C++ compiler available, e.g. `clang++ -O2`. Then do the §1 loop.
- **Undo:** delete the line.

### T2. One-compare fast path for guest RAM loads/stores  (expected 3-5%) [HW]

- **File:** `src/runtime/ctx.h`, functions `rd8`, `rd16`, `rd32`, `wr8`, `wr16`, `wr32` (lines ~101-138).
- **Why:**
  - Today every access does `isIo`, `isVm` + `crossesPage`, then `hpr`/`hpw`, which tests `isVm` again,
    then `memPtr`'s subtract-and-mask.
  - Almost all accesses are in the cached arena 0x80000000-0x8FFFFFFF. A single range check can go straight
    to `g_ram + (a - kGuestRamBase)`.
- **Change:**
  1. Add this helper after `memPtr` (Switch section only, inside `#if defined(__SWITCH__)`):
     ```cpp
     // RL-NX: T2 the cached arena (0x80000000-0x8FFFFFFF) needs no IO/VM/alias checks
     inline bool inArena(u32 a, u32 n) { return a - kGuestRamBase <= kGuestRamSize - n; }
     ```
     For the non-Switch branch, add `inline bool inArena(u32, u32) { return false; }`, so the old path runs
     there.
  2. Make this the first line of each function, after the `RT_WATCHING()` line in the `wr*` functions.
     - `rd32`:
       ```cpp
       if (inArena(a, 4)) [[likely]] { u32 v; memcpy(&v, g_ram + (a - kGuestRamBase), 4); return __builtin_bswap32(v); }
       ```
     - Same for `rd16` (2 bytes, `bswap16`) and `rd8` (`return g_ram[a - kGuestRamBase];`).
     - `wr32`:
       ```cpp
       if (inArena(a, 4)) [[likely]] { v = __builtin_bswap32(v); memcpy(g_ram + (a - kGuestRamBase), &v, 4); return; }
       ```
     - Same for `wr16` and `wr8`.

     The old code stays below as the slow path. It still handles 0xC0000000 (uncached), IO, VM and the
     locked cache.
  3. On non-Switch builds, `g_ram` does not exist. Guard each new line with `#if defined(__SWITCH__)`, or
     make `inArena` false and use `memPtr(a)` instead of `g_ram + ...`. The non-Switch build must still
     compile, but it is not built in this loop.
- **Check:** the §1 loop. Also watch the first mission load in Ryujinx (Battle of Hoth reads past MEM1, which
  is inside the arena, so this is fine).
- **Undo:** remove the inserted lines.

### T3. `rd64`/`wr64` as one access  (expected ~1%) [HW]

- **Do after T2.**
- **File:** `src/runtime/ctx.h`, `rd64` (line ~119) and `wr64` (line ~139). They are used ~10,000 times
  (`lfd`/`stfd`/`psq` paired).
- **Change:**
  ```cpp
  inline u64 rd64(u32 a) {
      if (inArena(a, 8)) [[likely]] { u64 v; memcpy(&v, g_ram + (a - kGuestRamBase), 8); return __builtin_bswap64(v); }  // RL-NX: T3
      return (u64(rd32(a)) << 32) | rd32(a + 4);
  }
  ```
  Do the same for `wr64`: `bswap64`, then one 8-byte memcpy. Keep the `RT_WATCHING()` behaviour by calling
  it first, as `wr32` does.
- **Undo:** restore the two functions.

### T4. `hle_GameFrameDelta`: compute per-frame values once  (expected <1%)

- **File:** `src/runtime/hle_game.cpp:129-200`.
- **Change:**
  - `playerCraft()` is called three times per frame (lines ~152, ~183, ~187). Call it once into
    `const u32 craft` (already exists at line ~152) and use `craft` in the two later calls.
  - Leave the craft check inside `gfx::setHudAnchoring(...)` / `setMenuTiling(...)` logically identical:
    `playerCraft() != 0` becomes `craft != 0`.
  - Do not reorder anything else. `keepHealth`/`keepAmmo` may change the craft state.
- **Check:** §1 loop. On hardware, the HUD still sits at the screen edges in flight with widescreen on.

### T5. Find and remove debug work still on the hot path (investigation, no code until confirmed)

1. Run `prof_top.py` on the newest hardware profile. List every function at or above 0.3% whose name
   contains `probe`, `dump`, `log`, `trace`, `Disassembl`, `tint::` or `texdump`.
2. In the 08:56 profile:
   - `texdump::dump` was 0.4%. It runs only when `texdump::directory()` is set
     (`src/runtime/gfx/gx_bridge.cpp:661`). Check whether the user's `boot.cfg` sets `R2_TEX_DUMP`; if so,
     tell them to remove it. Do not change code.
   - `tint` disassembler sorting (0.6%) means a shader was disassembled at runtime. Find who calls Tint's
     disassembler:
     ```
     grep -rn "Disassemble\|disassembl" extern/aurora/lib switch/deko
     ```
     If it runs for logging only, gate it behind an existing log flag.
3. Report what you found in §3 before changing anything.

## 5. Tasks: rendering (GX thread / render worker)

### T6. Skip redundant fixed-function state binds in deko3d  (expected 5-8% of the GX thread) [HW]

- **File:** `switch/deko/backend_commands.cpp`, `bind_pipeline()` (line ~410). The block
  `if (state.boundPipeline != p.get()) { ... }` (lines ~424-442) rebinds raster, depth, color, color-write
  and blend state every time the pipeline changes, even when those states are identical. That is the
  `dkCmdBufBindColorState`/`moveTo` cost.
- **Change:**
  1. In `switch/deko/backend_internal.h`, next to `Pipeline *boundPipeline = nullptr;` (line ~292), add:
     ```cpp
     // RL-NX: T6 last fixed-function state bound in this pass (skip identical rebinds)
     const void *lastRaster = nullptr, *lastDepth = nullptr, *lastColor = nullptr, *lastWrite = nullptr, *lastBlend = nullptr;
     ```
  2. In `bind_pipeline()`, replace each bind with a compare-then-bind. For example:
     ```cpp
     if (!state.lastColor || memcmp(state.lastColor, &p->color, sizeof p->color)) {
       dkCmdBufBindColorState(R.cmd, &p->color); state.lastColor = &p->color; }
     ```
     Do the same for `raster`, `depth`, `write` and `blend`.
     - Pointers are safe here because pipelines live in a cache and are not freed mid-pass. If you are not
       sure, store copies (`DkColorState lastColorV; bool haveColor;`) instead of pointers. That is the
       safer choice.
     - Always keep `dkCmdBufBindShaders`, `SetDepthBias`, `BindVtxAttribState` and `BindVtxBufferState`.
  3. Clear all five `last*` fields everywhere state can be lost:
     - wherever a render pass begins: search `begin_pass_native` (line ~667) and every place that sets
       `state.boundPipeline = nullptr` or resets `state`;
     - in `device.cpp:814` and `pattern.cpp:269`, which bind their own color state.

     Add `state.lastColor = state.lastWrite = ... = nullptr;` right after those binds.
  4. Flag: `R2_NO_T6=1` makes the compare always fail, so every state is bound as before.
- **Check:** §1 loop. Compare `deko-capture` frames from before and after; they must be identical. Any
  wrong blending (menus missing transparency, black boxes) means a missed reset point.
- **Undo:** `R2_NO_T6=1`, or restore the files.

### T7. `seamDraw`: avoid the hash lookup per call  (small, <1%)

- **File:** `src/runtime/gfx/gx_bridge.cpp:1256` has `Pipe& pp = pipes[layout.key];` on every call.
- **Change:** add `static u64 lastKey = ~0ull; static Pipe* last = nullptr;`. If `layout.key == lastKey`, use
  `*last`; otherwise do the lookup and store both. The map never erases, so the pointer stays valid
  (`std::unordered_map` references are stable on insert).
- **Do not touch** `kSeamShader`.

## 6. Tasks: I/O and loading

### T8. DVD read prefetch on a worker thread  (cuts load stalls; helps "level loads but not playable") [HW]

- **File:** `src/runtime/hw/hw.cpp`.
  - `diStart()` (line ~285) computes a fake drive latency.
  - `onDi()` (line ~266) then does a **synchronous** `discRead()` on the game/event thread when the latency
    expires. Large reads from an RVZ/ISO on SD block the game for milliseconds.
- **Change:**
  1. Add one worker thread. Use the port's existing thread helpers; check how other runtime workers are
     created with `grep -rn "threadCreate\|std::thread" src/runtime switch/src`. Give it a priority lower
     than the main thread, and a core other than 1 (the main thread is on core 1).
  2. In `diStart()` for `cmd == 0xA8`:
     - copy `off`/`len` into a request;
     - have the worker `discRead()` into a private `std::vector<u8>` staging buffer;
     - set an atomic `done`.
  3. In `onDi()`:
     - if a prefetch exists for the same off/len, wait for `done`. Use a condition variable or a
       semaphore, not a spin loop.
     - Then `memcpy` from staging to `dst`.

     Keep the rest of `onDi()` unchanged: `watchHost`, the registers and the IRQ.
  4. At exit, the worker must end itself. See `docs/SWITCH_PORT.md` / the skill's exit rules: wait in slices
     of ≤100 ms that check a quit flag. Otherwise relaunching from hbmenu breaks.
  5. Flag: `R2_NO_T8=1` keeps the synchronous path.
- **Check:**
  - §1 loop, plus a Ryujinx run of at least 300 s that reaches a mission load.
  - Exit to hbmenu and relaunch 3 times on hardware (user). There must be no
    "closed because an error occurred".

### T9. Find the kernel waits on the main thread (investigation)

- **Background:** build 08:56 showed ~10 ms/frame of kernel waits/services on the main thread (`fsFileRead`,
  audout). Find which.
- **Steps:**
  1. In the profile, role-0 samples whose pc symbolizes to `svc*`/`__libnx`/`fsFileRead`/`audout*` have an
     `lr` column. Symbolize the `lr` values the same way (edit a copy of `prof_top.py` to key on column 4).
     That gives the calling runtime function.
  2. Known candidates:
     - `switch/src/sqlite_vfs.cpp:140`, where `fsFileRead` is used by SQLite. Who opens SQLite?
       `grep -rn sqlite3_open src switch`. A pipeline/shader cache read per frame would be a bug.
     - `switch/src/audio_switch.cpp:92`, where `audoutAppendAudioOutBuffer` waits if the queue is full.
  3. Write the call chains and their ms/frame into §3. Propose a fix; do not implement it in the same task.

## 7. Playability checklist (user runs on hardware; agent reads the logs)

After each [HW] build, ask the user for one run with `R2_PROFILE=60` and `R2_COPY_LOG=1` that does
**title → main menu → first mission → 60 s of flying**. Check:

1. **Title drawn once.** The log has `fill-screen draw in a pass with box offset 0,238: NDC shift -0.000,0.992
   sent`, and the user confirms one title image.
2. **Mission reached.** No `fatal` at the end of `log.txt`. Run
   `py -3 -I C:/Users/Steve/.claude/skills/gc-decomp-switch-port/scripts/check_log.py "<log folder>" --elf-dir debug`
   and get no FAIL lines.
3. **Frame rate.** Read the `frames` and `slow_frames` lines in each `profile-NNN.txt`.
   - Target: `seconds × 30` or more game frames, i.e. 30 fps, before anything else.
   - Record frames/s per profile part in §3.
4. **Game speed.** With `R2_DT_LOG=1`, the `dt` lines every 10 s show game seconds against real seconds.
   Below 1.0 means the game runs in slow motion (it caps each frame at 1/29.97 s). That is expected until
   the frame rate is ≥30. Report the ratio and do not try to fix it by changing the cap.
5. **Audio.** `audio_underruns` in the profile header should be 0.

## 8. Do not try (already ruled out or against the rules)

- Lower resolution, docked-only fixes, or default overclocks (rule 5).
- Editing staged Aurora trees or `build-switch/gen` by hand (rule 3).
- Removing the `JmpBuf jb` exclusion in `switch/tools/patch-gen-restrict.py`. That function's context is
  rewritten by `OSLoadContext`.
- Turning on `-ffast-math` or `-fstrict-aliasing` for the game code. Guest RAM is accessed through byte
  pointers, and the game relies on exact float behaviour.
- Judging any speed change from Ryujinx.
- Skipping the per-frame hash of EFB copy destinations in `prepareTextures` (`gx_bridge.cpp`, `CopyDest`) to
  save time. That hash is how the bridge sees the CPU writing a new image over an old copy, which movies and
  loading screens do every frame. Without it they show black (README.PORT.md 8.8).

## 9. Open bug: missing images, movies and models (hardware build 11:18, logs `E:\RL-NXLOGS\LOGS 11232026-1157`)

### 9.1 What the player sees

| Scene | Shows | Missing |
|---|---|---|
| Credits | everything | - |
| Clone trooper intro | some clones, slowly | many clones, LucasArts logo |
| Factor 5 intro | the explosion | the ship, the Factor 5 logo |
| Title | START / OPTIONS text | background, R2 movie, logo; the attract demo never plays |
| Select game | text, medals | rebel background, spacecraft image |
| Select mission | text | middle image/movie (black) |
| Select craft | text, button images | pilot/ship graphic, hangar launch movie |
| "A long time ago" | text | star background |

Gameplay was not reached. **Ryujinx reproduces the missing Factor 5 ship and the bare title**, so the
investigation can run there (correctness only, rule: Ryujinx never proves speed). For a reference picture use
**Dolphin with the ISO**; `red_two.exe` does not run on the user's PC.

### 9.2 Ruled out (each tested, no change)

- The CPU drawing into the XFB (R2_COPY_LOG XFB hash diagnostic: no memory changes between display copies).
- Cull, Z, alpha and fog rejection; the fill-screen, screen-space 2D and projection-refold rewrites
  (`R2_GX_SKIP` filters). Depth is also out on the evidence in 9.3: the passes that should hold the
  pictures are **empty before the composite**, so there is nothing for a depth test to hide.
- Widescreen HLE (`hle_CameraSetup`; the setting is at its 4:3 default; `R2_GAME_CULLING` bypasses it).
- Emboss texgens (`R2_NO_EMBOSS=1` debug option in `gx_bridge.cpp` XF load handler: no help).
- Display-list inlining (`gx_fifo.cpp` runDisplayList), indexed XF loads, Aurora NBT3 vertex sizing.
- **GSYS virtual memory / ARAM paging (old 9.4).** `R2_TEX_DUMP` decodes 171 textures straight out of guest
  memory and they are the right pictures (the Factor 5 logo, the title's panel mosaics, 1024x1024 CMPR
  atlases): code that pages through the VM runs, and asset data is intact. Also 3D scenes (clone army,
  hangar floor) render, so vertex arrays and textures out of that memory are read correctly.

### 9.3 What is established (Ryujinx captures and draw lists, title and Select Game)

- **What the player sees on the title is exactly START/OPTIONS on pure black**: the presented frame has
  3221 of 288000 pixels lit (`pixel capture frame ... lit=3221/288000`). Dolphin's title
  (`E:/dolphin_ref/shots/dolphin_012/014/018.png`) has the mosaic bands, the central movie and the logo.
- **The backdrop, movie and logo are not submitted at all.** `R2_DRAW_BOUNDS=1` transforms every submitted
  draw on the CPU: at the title all of them together cover only `EFB x 128..320 y -184..240`
  (`bounds frame 900: 146 draws cover ...`), i.e. the text box and its animated icons. Nothing covers the
  rest of the 640x480 frame, so 9.3's original reading stands: this is game-side state, not the renderer.
  (An earlier note here blamed the renderer after counting 111 draws/frame; those 111 are the glyphs.)
- The title's pass chain, for completeness (`R2_CAPTURE_PASS=1`, frame 1110): `EFB 0 (0 draws) ->
  TexCopyConv 640x2 -> EFB 1 (1 draw) -> TexCopyConv 640x16 -> EFB 2 (54 draws, samples the 32x32
  "Copy Conv Texture") -> EFB 3 (1 draw) -> TexCopyConv 640x16 -> EFB 4 (54 draws) -> EFB 5 (1 draw) ->
  present`. The text pass is the only one with content (3 % lit); the single-draw passes and every
  copy-conversion input read black, and the conversions are 640x2 / 640x16 strips. With nothing submitted
  over the frame, this emptiness is a consequence, not the cause - but it will still hide the picture once
  the game does submit it (see 9.5).
- **Guest memory holds the right art.** `R2_TEX_DUMP` decodes 171 textures out of it at the title: the
  Factor 5 logo, the title's panel mosaics, 1024x1024 CMPR atlases. So the assets are loaded and the GSYS
  VM / ARAM paging is not the fault (the old 9.4 hypothesis is dead).
- **Select Game shows text, medals and nothing else** (captures `frame-1200`, `frame-1380` of the run with
  `R2_DRAW_BOUNDS` on - 379-403 draws/frame - and `frame-1238`, `frame-1260` of the 16:36 run): SELECT GAME,
  the three slot medals, ERASE GAME and BACK appear; the rebel background and the spacecraft do not. The
  draw list below says what happens to them.
- **A screen-filling quad *is* submitted whenever the game wants one** (this corrects the earlier claim that
  none ever was: the old `R2_DRAW_LIST` only logged frames whose index was a multiple of 600, so it saw one
  frame of each screen). The fade between the intro and the title is two `op 80`, 4-vertex quads of a
  **32x32 IA8** texture covering `EFB x -3..643 y -2..482` and `x -3..643 y -240..244` (frame 1005): a
  640x480 overlay from a small texture, so full-frame 2D quads are submitted, drawn and visible. What the
  title plateau lacks is any such quad: its largest draw is a 64x64 icon at `x 0..128 y 32..160` (frames
  1152 and 1200: 254 and 152 draws, none over 40 % of the EFB, no 1024x1024 atlas bound, `R2_DRAW_LIST`
  now flushing any frame with 200+ draws or a 40 %-of-EFB draw, and `R2_DRAW_LIST_AT=<frame>` to force one).
  So the title's backdrop is not submitted after the transition, and it is not a dropped or blackened draw.
- **The title's backdrop is submitted once, at the transition, and never again.** Frame 600 of the run at
  16:22 is 12 draws and none of them is measurable: those are the title's 1024x512 / 512x512 CMPR mosaic
  atlases and the 512x336 I8 movie quad, drawn while the screen changes and not afterwards. Everything
  after that (frames 638-1200) is the 2D text/icon layer only.
- **At Select Game the image layer *is* submitted - with a garbage transform.** Steady-state Select Game
  (frames 1148 and 1208, 744 draws each; captures `frame-1238`/`frame-1260` of the 16:36 run show SELECT
  GAME, the medals, ERASE GAME and BACK with no background): draws 0-110 are the text, icons and medals
  (64x64 / 128x128 CMPR quads, placed exactly), then draws **111-123 are the image layer**: `op 80`,
  4 vertices, CMPR textures 48x64, 96x48, 96x192, 128x64, 80x96, 256x64, 64x128, 128x80, 256x64, 144x128,
  64x96, 96x48 (the rebel collage's tiles) - and every one of them transforms to nonsense: `EFB x 320..320
  y -4.0e14..4.0e14` (zero width, infinite height), `x -9.8e33..9.8e33`, `x 728788916654346777594194558976
  ..728788916654346777594194558976`. That is why 46 of the frame's draws are counted as covering 40 % or
  more of the EFB (see the bullet above): not because a big quad is drawn, but because the extent is
  garbage. So this screen is **submitted and thrown away by a degenerate transform**, not "never drawn".
  The transition into it (frame 1120 of the 16:22 run) additionally binds 1024x1024 CMPR atlases
  (`0144DC80`, `013A31E0`) whose bounds are not measurable at all.
- **Correction: the `1e14`/`1e33` "extents" were numbers the *measurer* produced, never anything the
  renderer did, and the "degenerate transform" lead of 9.4 is dropped.** `drawBoundsEfb` read the position
  matrix with `&s_xfMtx[(s_cpMtxA & 0x3F) * 4]` on a 256-float array, so its last reachable word, 252, left
  `m[8..11]` spanning past the end of the array (and a per-vertex index byte could reach word 1020 outright).
  `xfPosMatrix()` now refuses a matrix it cannot fit and counts the draw as unmeasurable (`kMissMtx`) instead of
  placing it at a number read from whatever followed the array; `R2_NO_MTX_GUARD=1` restores the old read.
  Two 400 s runs of the same build differing only in that switch gave 82 comparable captures, **78 of them
  pixel-identical (maximum channel difference 0)** and the four that differ (frames 630, 648, 1024, 1130) single
  animation frames - so the guard cannot change the picture, as expected of CPU-side measurement. Note the
  mechanism is *not* fully proven: the guard run and the unguarded run both report **0 absurd** draws at Select
  Game (the `1e14` numbers appeared only on transition frames), so the out-of-bounds read is the leading
  explanation, not a reproduced one. What is proven is that the quads' geometry was never the fault: nothing
  on screen is drawn from those numbers.
- **What the live state looks like on the draws that *are* dropped** (`R2_DRAW_RAW`, frame ~1200 of the guard
  run, the Select Game image layer): `vertex -25.6 16.8 0`, `matrix z row 0 0 0 0`, projection `2.06061 0
  2.74748 0`, array `005C10A0` stride 12. A sane 2D vertex, a **zero z row in the position matrix**, and a
  projection whose z terms are zero: `ez` comes out exactly 0 for every vertex, `ez >= 0` drops the whole draw,
  and the renderer's own projection (`w = -ez`, `xf_load_projection`) would divide by zero for the same
  vertices. That is a projection-state question (the type word at XF 0x1026 reads 0 = perspective for a matrix
  that has no perspective terms), not a vertex-layout one - the live type must be compared against Dolphin.
- **Who submits the draws** (`R2_DRAW_CALLER=1`; `curFn` is 0 on the fiber that writes the GX stream, so the
  histogram keys on the return address and its one-time line prints the back chain). At the title plateau
  (windows up to frame 1020) almost every draw command comes from SDK glue (`800375DC` display-list calls,
  `80033114`, `800331E0`, `800331C0`) with the game function `7FD28EB4` at 6-9 commands a frame; from about
  frame 1050 the game function **`7FD3F9A4`** writes 38 commands a frame, the first game-side per-object
  submitter - the scene renderer this bug lives in.
- Depth, cull, alpha and fog are out: at the title nothing is submitted over the missing area at all, and
  at Select Game what is submitted has a degenerate extent, so no test can be hiding a correct quad.
- Directly drawn art works: the loading screen's full-screen   mosaic (no copy involved) appears in colour,
  as do text, icons and the 3D scenes.
- **Coverage caveat: every placement number in this section covers only the draws `drawBoundsEfb` could
  place.** It returns nothing for a packet it cannot decode, so "nothing covers the rest of the frame" and
  "draws 111-123 transform to nonsense" are statements about the *measurable* part of the frame, not about all
  of it. At Select Game it places 353 of the frame's 718 draws (frames 1140 and 1200); the other 365 were
  counted as "unmeasurable" without a reason, which is why the two readings contradicted each other across
  runs. Classification added this session (`BoundsMiss` in `gx_bridge.cpp`, printed by `R2_STATS`): those 365
  are **330 draws whose every vertex is behind the eye** plus **35 empty strips** - and **0 undecodable
  packets**, at Select Game and everywhere else in the run (17 distinct miss lines, all of them one of those
  two). So the measurer is not blind to a vertex layout; what it drops is geometry the renderer drops too
  (Aurora's perspective projection is `w = -z`, `lib/gx/regs.cpp` `xf_load_projection`, the same test the
  measurer makes). The numbers below stay as measured, with this caveat attached; the consequence is drawn in
  9.8.

### 9.4 Working hypothesis: one gate, one bad transform

The **title** is gated game-side: the backdrop is drawn once at the transition (frame 600: 12 draws) and
never again while the screen stays up; the attract demo never starts. Everything else missing there is an
image or a movie.

**Select Game** does not fit that story: it submits the image layer every frame (draws 111-123 of frame
1148, CMPR tiles, four vertices each) and the quads transform to nonsense, so nothing lands. That is a
renderer or data fault, not a gate: either the vertex data at that array is garbage in guest memory - the
same unfilled buffer as the title's gate, one level down - or the port decodes that vertex format or its
matrix wrongly.

What the two share is the data: the buffers this layer is built from (movie planes, panorama tiles, the
dynamic vertex array) live in the memory the XFB and the EFB copies use, and the port's copy path is still
known-broken (the `002F8EE0` stale copy and the 640x2 / 640x16 `TexCopyConv` strips, 9.5 item 4).

**Correction to the second half (this session).** "The quads transform to nonsense" does not hold: those
extents came from `drawBoundsEfb` reading the position matrix past the end of `s_xfMtx` (see the correction in
9.3). With that read guarded, Select Game reports 0 absurd draws and the title and Select Game pictures are
pixel-identical to the unguarded build. What remains is what the measurer *does* drop, and its live numbers
are ordinary: a 2D vertex at `-25.6 16.8 0`, a position matrix whose z row is all zeros, a projection with no
z terms (`2.06061 0 2.74748 0`), i.e. `w = -ez = 0` for every vertex. So the Select Game image layer is not
mistransformed - it is submitted with a projection that divides by zero, and the question is why the type word
(XF 0x1026) reads 0 (perspective) for a matrix that is orthographic in everything but its type. That is
Dolphin's ground truth to settle, and it is the same class of question as the EFB copy path: state that the
port reads differently from the hardware.

### 9.5 Next steps, in order

1. Two separate faults. **Title**: find where the game decides not to redraw the backdrop after the
   transition (frames 638-1200 are text/icons only). **Select Game**: find out why draws 111-123 of frame
   1148 transform to 1e14..1e33 (item 3 below). Useful levers: `R2_INPUT="<poll>:START*6,..."` (script presses count *polls*, not retraces -
   `poll()` advances `scriptTime` itself, so times run ahead of guest seconds; a press every 400 from 8000
   onward is enough to get past the title); `R2_DRAW_LIST=1` (first draws of a frame with their texture);
   `R2_DRAW_BOUNDS=1` (where each draw lands); `R2_TEX_DUMP` (what is uploaded); `R2_TEX_LOG=1` (which
   texture maps are bound each frame - a backdrop texture bound but never drawn narrows the gate to the
   game's own state).
2. Whatever gates the title is on the game's side of the asset/movie pipeline: the streamed movie buffer
   (the same buffers the XFB lives in - see the `002F8EE0` note below), the panorama/backdrop upload, or
   the video-decode thread. Instrumenting those (a log when a backdrop texture is uploaded, when the movie
   player reports a frame) is cheaper than guessing.
3. **Find why the Select Game image quads transform to garbage** (draws 111-123, `op 80`, 4 vertices, CMPR
   tiles, extents of 1e14..1e33). Three ways to tell a game-side array that was never filled from a vertex
   format we decode wrongly: `R2_GX_DUMP=<frame>` at 1148 dumps that frame's command stream (`R2_GX_DUMP`
   fired at frame 480 before, so the path works when the number is right); `R2_FIFO_LOG=<n>` prints the
   command bytes as they are executed; and `R2_DRAW_LIST`'s layout makes the draw to look at unambiguous
   (draw 111 of frame 1148). What separates the cases: plausible vertex data with a wrong matrix (renderer
   reads it right and the picture should show), plausible data with a wrong format (renderer draws a
   garbled picture, not nothing), or garbage in guest memory (the game wrote nothing - the same gate as
   the title, one level up: whatever fills these arrays).
4. **Fix the copy path anyway, before the image layer comes back**: (a) the `TexCopyConv` passes are
   640x2 / 640x16 strips - compare the game's copy rect (`R2_COPY_LOG`) against the target size; (b) at the
   title transition the hardware log has `texture at 002F8EE0 (640x452, 16 bits): memory changed since an
   EFB copy (640x450, 4 bits) went there: still the copy` - an XFB address keeping a boot-time copy, which
   would hide any image decoded there. A rule "a display destination keeps its copy only when the bound
   texture matches the copy's size and depth" was tried this session and made no difference to the title
   (`EFB 1` still `lit=0/307200`) because nothing is submitted; it was reverted, and it is the change to
   apply together with 1-3.
5. Then a hardware build (stamp it in `boot.txt`) and the section 7 checklist.

### 9.6 Debug additions to keep or remove when done (all marked `// RL-NX:`)

- `gx_bridge.cpp`: `R2_NO_EMBOSS` in the XF load handler (case 0x10); XFB CPU-write hash under
  `R2_COPY_LOG` in the 0x52 display-copy handler; `parseGxDump()` + sdmc dump path (keep these two).
- `switch/deko/device.cpp`: `R2_CAPTURE_PASS` (pass images, added this session) and `R2_CAPTURE_FROM`
  (start the capture window after N guest seconds) next to `COS_DK_CAPTURE`; keep them while this bug is
  open, remove with the rest.
- `gx_bridge.cpp`: `R2_DRAW_LIST` (a frame's first 40 draws plus its 12 large/absurd draws, flushed for
  frames with 200+ draws or a 40 %-of-EFB draw, at most every 60th frame and 40 flushes; a `sample` line
  every 300 frames reports the largest draw). `R2_DRAW_LIST_AT=<frame>` forces frames `<frame>..<frame>+15`
  but was never exercised - the 60-frame cadence made it unnecessary; test it before relying on it.
- `gx_fifo.cpp` / `gx_fifo.h`: `R2_DRAW_CALLER`, the draw-submitter histogram (`gxDrawCallerLog`, logged
  from `endFrame` every 30 frames) added this session for the gate hunt.
- `gx_bridge.cpp`: `R2_STATS` (per-frame draw/bind outcome counters), `R2_TEX_FLAT` (every texture sent as a
  single 8x8 tile), `R2_STRICT` (no merging, rehash every bind, palette in the identity, no display-copy
  ownership), `R2_DRAW_RAW`/`R2_DRAW_RAW_FROM` (the vertex data behind an absurd placement) - section 9.8.
- `gx_bridge.cpp`: `R2_STATS` splits the unplaced draws into empty strips / behind the eye / undecodable
  (`BoundsMiss`) and `drawBoundsMiss` prints one line per *distinct* unplaced layout (up to 40) instead of the
  first six packets; `R2_DRAW_RAW` also logs a perspective draw the measurer drops as behind the eye (vertex,
  view-space z, matrix z row, projection). `xfPosMatrix` keeps the measured position matrix inside
  `s_xfMtx[256]` (see 9.8: it was read past its end).
- `gx_bridge.cpp`: `R2_DRAW_LIST`'s logged draws now carry their sampling - texture address, size, format,
  texgen 0's type and source, the coordinate format and `frac`, the raw and texel-space UV range, the wrap
  modes and the decoded texel at each UV corner (`drawUv`, `texelImage`, `texelAt`, decoding through
  `texdump::decodeRgba`). This is what separates "wrong UV / empty atlas space" from "wrong texture object".
- `extern/aurora/lib/gx/texture.cpp`: `R2_COPY_CACHE_MB` / `R2_CONTENT_CACHE_MB` override the texture cache
  budgets (applied once, from `end_frame`), added with 9.8.
- `gx_bridge.cpp`: `R2_MATWATCH=<phys>,<len>` (hex): samples a guest block at every frame end and logs it
  when its content changes, plus a 120-frame heartbeat with its nonzero word count (9.13). Read-only, no store
  watch needed - `R2_WATCH`/`RT_WATCHING` is compiled out of the guest stores on the Switch, and `R2_PEEK`
  lives in `main.cpp`, which the Switch entry point never runs (both were tried and produced nothing here).
- `gx_bridge.cpp`: `R2_ZEROMTX=keep` drops a 12-word position-matrix `LOAD_INDX` whose block is all zero, so
  slot 0 keeps its previous matrix (`XF 0000 x12` usually left the camera there). Diagnostic only - it changes
  placement in every scene, not just the broken one (9.14).
- `R2_DCBZ=<phys>,<len>` (+`R2_DCBZ_BT`): log the guest call chain of every `dcbz` whose 32-byte block
  overlaps that range (9.17). `R2_TEX_DUMP` now resolves a relative directory beside `logs/` on the Switch and
  logs the resolved path on the first write, plus a line on every create/write failure.
- `switch/CMakeLists.txt` + `switch/build.sh`: `RL_NX_WATCH_BUILD=1` compiles the guest store watch into the
  Switch build (ctx.h `RL_NX_WATCH`), OFF on every ordinary build. `ctx.h` tests a store against the watched
  block inline by *host pointer* (`RT_WATCH_HIT`), so cached 0xC-alias and VM-mapped writes match too; a
  watched word store no longer pays a call (9.16).
- `hw.cpp`: a leftover blank line in `viNoteFb` from a removed VI-flip diagnostic.
- Current NRO contains these debug options and is not for the user.

### 9.7 Remaining items after this bug

- Clone trooper intro is slow (separate perf issue; see sections 4-6).
- Gameplay has not been checked since build 11:18; run the section 7 checklist once visuals are fixed.
- README.PORT.md needs a note on the outcome of this bug.

### 9.8 The step back: where the assets are lost, architecturally

The per-asset chase reached its limit (sections 9.1-9.6): some screens show no image layer at all, others submit
the right 1024x1024 CMPR atlases and show text only, and one run even measured the same quads with 1e33-pixel
extents while the next run measured them sanely. That is not a picture problem, it is a **resource-identity**
problem, and it is worth naming the layers before touching anything else.

```
guest (GSYS/GSW) -> src/runtime/gfx/gx_bridge.cpp      own FIFO parse, BP/XF/TEV state,
                                                      own texture identity + copy-dest map
                 -> Aurora lib/gx command_processor   GX state machine, texture object caches,
                                                      content cache, copy cache, pipelines
                 -> Aurora gfx recording              draws, pipelines, offscreen caches
                 -> Dawn Null + Tint                  WGSL -> GLSL (Null device at runtime)
                 -> deko3d                            screen
```

The bridge emits the same `GX_AURORA_LOAD_TEXOBJ` / `LOAD_TLUT` / `DESTROY_COPY_TEX` sub-commands Aurora's
own Dolphin GX path emits, so **two GX state machines and three resource caches** sit in series, and the
bridge, not Aurora's tested `lib/gx` cache, decides what a texture *is*.

What is ruled out by evidence, so it is not re-examined: **deko3d** (text, glyphs, medals, 3D scenes and the
explosion movie render, so the backend draws what it is handed), **Dawn/Tint at runtime** (Null device; a
translation failure fails pipeline creation, which Aurora counts), and **the game's data** (`R2_TEX_DUMP`
decodes the right art out of guest memory, and the draws are submitted).

Where it does point - three defects in the bridge's identity model, which together produce exactly the pattern
"static, uniquely-addressed art renders; streamed, re-paletted or copy-aliased art does not":

1. **`textureIdentity()` is palette-blind.** The cache key is `(addr, image0, mips, levels)` and the version is
   `XXH3` of the *index data only* (`textureBytes(fmt, w, h, levels)`, i.e. level 0..n indices). TLUT address and
   palette bytes are in neither, so a texture whose palette the game reloads resolves to the first palette
   forever, and two paletted images sharing address/size/format collide.
2. **Render-target addresses short-circuit the identity**: `if (s_copyDests.count(addr)) { id = version = 0;
   return; }`, and display-copy destinations keep their copy *permanently* (`s_dispDests`). Any address that
   ever took an EFB or display copy answers with the old copy after the game has loaded real art there - the
   documented Millennium Falcon cockpit case, one instance of a whole class.
3. **Version rehashing is a timer** (once a frame, at most 60 Hz) rather than a fact about writes, and the
   copy/content caches below it are LRUs (Aurora: 64 MiB, 32 idle frames, sweep every 16) keyed by whatever id
   the bridge invents. A streamed texture can be stale for a frame, and an evicted copy is a draw that samples
   nothing.

#### The measurement switches (this session, all `// RL-NX:`)

| Switch | What it does | The question it answers |
|---|---|---|
| `R2_STATS=1` | per-frame counters (every 60th frame): draws submitted / measurable / unmeasurable / wholly outside the scissor / absurd, and texture binds: changed / repeated / copy / flat | is the asset submitted, is it inside the frame, and did its texture come from memory or a copy |
| `R2_TEX_FLAT=1` | every `LOAD_TEXOBJ` sends a single 8x8 tile of the same address | if a screen shows flat blocks the geometry/state/placement is right and only the image content is lost; if it stays empty the draw translation is losing it |
| `R2_STRICT=1` | no draw merging, every bind rehashed, palette part of the identity, display-copy destinations no longer permanently "still the copy" | if the art comes back, the heuristics were the bug |
| `R2_COPY_CACHE_MB` / `R2_CONTENT_CACHE_MB` | override Aurora's texture cache budgets (`lib/gx/texture.cpp`) | if raising them changes a screen, eviction was the bug |

Run them on the title and on Select Game with the same scripted input as 9.5, then read the decision off this table:

| `R2_TEX_FLAT` | `R2_STRICT` | Conclusion |
|---|---|---|
| blocks appear, art absent | art appears | identity heuristics (1-3) - fix those, not the draws |
| blocks appear, art absent | art still absent | image content reaches the renderer but resolves to the wrong texture: the copy/palette path |
| nothing appears | anything | the draw translation loses the layer before any texture question (state, scissor, TEV, pipeline) |

If the third row is what the runs show, the answer is not more of the bridge: the honest next step is to stop
having a second GX state machine and let Aurora's `lib/gx` own texture identity (it already has dependency
tracking, bind generations, invalidation and unit tests), or to re-implement the draw translation for the state
combinations these draws use. Discarding the bridge's ad-hoc identity wholesale is on the table - the SFA-NX
precedent was exactly this kind of surgery on Aurora/Dawn for deko3d.

#### Measured (17:22 and 17:35 runs, scripted input as in 9.5, captures in `build-switch/tmp/flat` and `strict`)

**`R2_TEX_FLAT=1` - the title is still black; Select Game is a covered frame.**

- Title (`flat/frame-1096`): black with the START/OPTIONS glyphs as flat blocks. Nothing covers the frame, so
  no texture question arises there: the image layer's **geometry is absent** (the gate of 9.3).
- Select Game (`flat/frame-1142`): the whole 640x450 is a flat dark grey field, with the medals, ERASE GAME and
  BACK still on it. A draw **does** cover the frame there, so geometry, projection, scissor and state are
  right, and the image is lost in the **texture content** it samples.

**`R2_STRICT=1` (with `R2_COPY_CACHE_MB=512 R2_CONTENT_CACHE_MB=512`) - Select Game unchanged**
(`strict/frame-1154`, `frame-1182`: same text, medals, black background). So, for this screen, the identity
heuristics are **not** the cause: not the palette-blind key, not the 60 Hz rehash, not the display-copy
"always keep the copy" rule, not the copy/content LRU budgets, not Aurora's draw merging.

**`R2_STATS=1` at Select Game** (frame 1140, both runs): `718 draws (353 measured, 365 unmeasurable, 80 wholly
outside the scissor, 0 absurd); texture binds 215 (changed 0, repeated 188, copy 27)`. Two things stand out:

- **365 of 718 draws cannot be placed at all** by `drawBoundsEfb` (half the frame's geometry), which is why the
  "no draw covers the frame" and "quads transform to 1e33" readings contradicted each other across runs: the
  measuring model itself is blind for those packets.
- **27 binds resolve to EFB copy textures and 0 binds report changed content** during the frame - the covering
  layer samples something that is black where it samples it, while the same address's first 8x8 tile is not
  black (that is where flat mode's grey comes from). That is a **sampling/addressing** fault (UVs, atlas region,
  texgen or the wrong texture object), not a cache-content fault.

Where that leaves the search: the title needs the draw-translation/gate answer (9.5 items 1-2), and Select Game
needs one more instrument - log the covering draw's texture address, size and **UV range** (`R2_DRAW_LIST` plus a
UV extent line), dump that address with `R2_TEX_DUMP`, and see whether the sampled region is empty atlas space.
At the same time `drawBoundsEfb` has to learn the 365 unmeasurable packets (`drawBoundsMiss` prints why for the
first 6 only; raise that cap and fix the top reason), because every placement conclusion in 9.3 was drawn from
half the draws.

### 9.9 The EFB copy path, checked against Dolphin's own decode

Dolphin's `VideoCommon/BPMemory.h` (`UPE_Copy`) and `BPStructs.cpp` (`BPMEM_TRIGGER_EFB_COPY`) define what the
registers mean, and the port's `prepareCopy`/`copyFormat` can be read against them line for line. Summary of the
comparison (the port's own field extraction is in parentheses):

| field | Dolphin | port | verdict |
|---|---|---|---|
| source rect | `0x49` = left/top, `0x4A` = **width-1 / height-1** | same (`+1`) | matches |
| destination address | `0x4B << 5` | same | matches |
| destination stride | `0x4D << 5`, **copy skipped when 0** | read for the XFB, `0` not checked | latent difference |
| texture format | `tp_realFormat()` = rotate the 4-bit `target_pixel_format` (bits 3-6) right by one | `((cp>>4)&7) | (((cp>>3)&1)<<3)` is exactly that rotation | matches |
| intensity | `intensity_fmt && auto_conv` (bits 15, 16) | `((cp>>15)&3)==3` | matches |
| half scale | bit 9, halves the destination | same | matches |
| copy to XFB | bit 14 | same | matches |
| XFB height | `1 + srcWH.y * yScale` (`0x4E`, `GXSetDispCopyYScale`) | `srcWH.y + 1`, `0x4E` ignored | latent difference |
| out-of-bounds source | clamp to 640x528, or skip when wholly outside | not checked | latent difference |

So the field decoding is right, and the copy list the game actually issues at the title (`R2_COPY_LOG`, frames
540-1200) says the same: **two 640x226 halves** into XFB lines 0 and 224 of 450, stride 1280, sources `y=16` and
`y=2` (the two EFB passes with different scissor box offsets), plus a boot-time 640x450. The **640x2 and 640x16
copies are band clears** - they go to the scratch buffer `00395300`, which is never in the XFB, and their source
rects are EFB rows 0..15 / 0..1 - not the picture being downscaled. The "strips where a full-screen copy is
expected" reading was wrong: the picture copies are full width, half height, and their rects are correct.

What that leaves: the copies read an **EFB that has almost nothing in it** (frame 600 is 12 draws, all of them
"behind the eye"), so the copy path is not where the picture is lost. The three differences above are worth
fixing for correctness (they matter for a game that scales its XFB or copies out of bounds) but none of them can
black out a copy whose rect is already right.

### 9.10 Dolphin FIFO ground truth (the `.dff` recordings)

Five recordings were made with Dolphin's FIFO recorder (clone movie, intro, two parts of the title page, the
attract video). The format is Dolphin's own (`Core/FifoPlayer/FifoDataFile.cpp`): magic `0x0d01f1f0`, version 6,
a 128-byte header with offsets to the BP/CP/XF/XF-register/texture snapshots, then a 64-byte record per frame
(`fifoDataOffset`, `fifoDataSize`, `fifoStart/End`, memory-update list). The sizes in the header are in
**registers**, so each region on disk is four times the size it advertises. The FIFO bytes themselves are the
GX command stream the CPU wrote, byte-sequential: `0x61` BP register is 5 bytes, `0x08` CP register 6, `0x10`
XF register `1 + 4 + 4*cnt`, a primitive (`0x80-0xBF`) `4 + count*vertexSize` where the vertex size comes from
VCD + VAT A, `0x20/0x28/0x30/0x38` 5, `0x40` 5, `0x48` 1. Reader: `build-switch/tmp/dff/{dffstate,proj}.py`
(scratch, not part of the build). It is validated against the port on the first frame of `3.dff`: one copy,
`src 0,0 640x16, stride 1280, dst 0x00395300` - the same line the port's `R2_COPY_LOG` prints for its band
clears.

What the title-page recordings say:

- A content frame is **45 indexed quad draws** (`VCD 0x1400/0x0002`) plus 2-3 others, and the page is
  **static**: only about two frames in five issue any draws at all; the others are a single 55-byte frame that
  carries nothing but one EFB copy. A frame with no draws at the title is normal, not a lost frame.
- Three projections alternate, and the port's own shadow holds exactly these values:
  `0.003125, -1, -0.00416667, 1, -1, -1` **type 1** (2D in pixel units, 640x480), `0.003125, 0, 0.00416667, 0,
  -0.001, -1` **type 1**, and `2.06061, 0, 2.74748, 0, -2.50006e-05, -0.100003` **type 0** (perspective).
  `gx_frame_480.txt` from the port has the same three families (`2.31045 ... type 0`, `1, -0, 1, -0, -1, -1 type
  1`) - so the XF shadow and the type word are faithful, and "type 0 at these draws" is what the game wrote.
- `BP 0x59 = 0x0484AA` -> scissor box offset `(0, 238)`, the bottom pass, as recorded before.
- `CP 0x30 = 0x3CF3CF3C` (and `0x3CF3C7BC`, `0x3CF3CF00`). Only the **low six bits** are an index: `0x3C` = 60 =
  `GX_IDENTITY` (`GX_PNMTX0/3 = 0`, ..., `GX_IDENTITY = 60` in Dolphin's `GXEnum.h`), which addresses XF word
  `60 * 4 = 240` = `0x00F0`. The port's stream contains **zero** writes to XF `0x00F0`: the identity is
  hardwired in the hardware, so a shadow that only records writes has nothing there.

That last point is the answer to the "matrix index 63 / per-vertex byte 255" question: there is no 63 and no
byte 255 on these draws (their VCD has no `PNMTXIDX`), and the index that looked like garbage is 60 =
**GX_IDENTITY**, not a corrupt read. Its consequence is 9.3's mystery: `xfPosMatrix(240)` returned an all-zero
matrix, so every draw using the identity matrix measured as "behind the eye" (perspective: `w = -z = 0`) or as
an empty strip (orthographic: a zero-size box). That is the 330 draws of Select Game and the all-behind frames
of 9.5 - a blind spot in the measurer, not a renderer fault. `xfPosMatrix` now returns the identity for that
word (Aurora already does the same: `position_matrix()` returns its static identity for index 20), with
`R2_NO_IDENT_MTX=1` to restore the old read.

### 9.9 The decisive result: the picture is byte-identical either way (session 1124)

The backdrop question is now settled by comparing **matched screens**, not frames. `select game` with the
backdrop up is ~71 040-71 972 lit pixels, text-only is ~1 964-1 993, and which one a boot shows varies
(measured ~1 boot in 3), so every earlier single-run A/B on this bug - including the `R2_NO_SS2D` "fix" and
the `load_clr` fallback attempt - was reading the flake, not the change.

- `gx_frame_1162.txt` (the whole recorded command stream for that frame) is **byte-identical** between a boot
  that shows the backdrop (`clr0`, 71 972 lit) and two that do not (`clr2`, `gateA2`, 1 964 lit): all 21 582
  lines. The port submits exactly the same work either way.
- Gate state, projection and matrices do not separate them either: `R2_NO_GATES=1` vs `0` gives
  byte-identical matched captures (1993 vs 1993 lit, 0 of 72 000 bytes differ), every draw of frame 1045 uses
  the ortho projection and the identity position matrix (see the identity note at the end of 9.8), and
  `XF 0x1050` (the post matrix) is `0x0000003D` in every frame sampled.

Conclusion: the grey page is **stale EFB content**, not the backdrop being drawn on some boots and not
others. The backdrop quads are submitted identically and never paint, and what a boot shows depends on what
was in the EFB beforehand. Any further work on this bug has to explain why those specific draws produce no
pixels, and a claim about the page must be made against the *title* page (frames ~1000-1045), which is
deterministic - the Select Game page cannot serve as a test target.

Two related findings from this session:

- Those 2D page draws take their colour from the vertex (`XF 0x1009 = 0x00000001`, i.e. `matSrc = VTX`) while
  their vertex format sends no `CLR0` (`VCD 0x1400/0x00000002`, `VAT 50E16409 80000009 00000000`). The
  material colour is white (`XF 0x100C/0x100D = 0xFFFFFFFF`) and `0x100E/0x100F` hold the dark *ambient*
  values (`0x00000706`), so a shader that reads black for the absent vertex colour paints black where the
  hardware would still hold the previous vertex colour. Forcing those two registers white was tried and is
  **wrong** - they are not the material registers, and the override produced an invalid channel config
  (`diffFn` out of range) and a fatal `Tint` compile. The colour fallback in `gx.cpp` (matSrc VTX -> REG when
  the attribute is absent) was also tried and **reverted**: it changed the generated shader set and the boot
  died on the same `Tint` error within 12 s, twice.
- That `Tint` failure exposed a real robustness hole worth keeping: `lighting_func` emitted `var diff = ;`
  whenever a channel's diffuse function was not one of `GX_DF_NONE/SIGN/CLAMP`, and `Tint` rejects the whole
  shader, which loses the pipeline (and, at boot, the run). `shader.cpp` now falls back to the no-diffuse
  term for any such value instead of emitting invalid WGSL. It is a guard, not a behaviour change.
- The stale `deko-fail-*.wgsl/.txt` files in `logs/` are **not** cleared between runs and the smoke script
  copies the whole directory, so a fresh run can "report" an old failure. Clear them (as with the `.ppm`
  captures) before reading a failure out of a run.

### 9.10 Gates, blend and the missing vertex colour: all three ruled out (same session)

Against the deterministic title page (frames ~1000-1045, 0 lit pixels every boot), each of these was tried on
its own and left the page black:

- **Per-draw tests**: `R2_GX_OVERRIDE=aalways,zalways,nocull` (alpha test compare forced to "always", depth
  compare "always", culling off) - frame 1001 still 0 lit. Note that `R2_NO_GATES` never touched the alpha
  test, so this was a real gap in that bisect; `BP 0xF3 = 0x3F0000` has alpha test disabled anyway.
- **Blending**: the page's quads go out with `BP 0x41 = 0x0004BD` - blend on, type **subtract** - so a quad
  with zero alpha subtracts nothing and leaves the EFB untouched, which is exactly the shape of a draw that
  is submitted every frame, transforms correctly and paints nothing. `R2_GX_OVERRIDE=noblend,nocull,zalways`
  (new `noblend` arm in `R2_GX_OVERRIDE`: clears `BP 0x41` bit 0) changed nothing: 994/1034/1053 still 0 lit,
  and the ~765-lit frames after them are the text screen that also appears without it.
- **The absent vertex colour**: those quads ask for `CLR0` while their format sends none, and the generated
  shader substitutes `vec4f(0.0)`; `vtx_attr` was changed to substitute white (the value a hardware vertex
  colour register realistically still holds) and the page stayed black (983/1027 = 0 lit, the same ~700-lit
  text screen from ~1040 in both builds). Reverted.

So the page's own draws are not being refused by a gate, not blended away, and not merely painted black: with
their colour forced white they still produce no pixels on a black clear, which means those quads are not the
backdrop at all - the title's backdrop image is only submitted at the transition (9.4), and Select Game's grey
is stale EFB content (9.9). The next question is therefore *which* draw the missing backdrop image belongs to,
not why the 2D page quads are invisible; `R2_DRAW_LIST` with `R2_TEX_LOG` at the transition frame (600) is the
place to answer it.

### 9.11 Where the title's backdrop would have to come from (R2_DRAW_LIST + R2_TEX_LOG, 270 s run)

Sampling every 60th frame plus `R2_DRAW_LIST_AT=590` (frames 590-605 forced) gives the frame-by-frame answer:

- **The title page itself never submits a backdrop.** Frame 665 is the title screen (capture `frame-670` = 119
  lit pixels, the text only) and it is **2576 draws with not one covering 40 % of the EFB**: the largest is
  `EFB x 65..361 y -5..291` (a 296x296 corner of a 640x450 frame) on a 128x128 texture, and the rest are
  glyph-sized quads on `012F24C0 1024x1024 fmt 14` (the font atlas). There is no full-screen textured quad in
  the frame to be blackened - so no change to the 2D page draws (9.9, 9.10) can bring the backdrop back.
- **The transition does submit one**: frames 590-605 hold exactly **12 draws** - `014F8740 1024x512 fmt 14`
  (op 98, 9/14/8 verts), `0154DC80 512x512 fmt 14` (op 80, 4 verts), `015A84A0 512x336 fmt 1` (op 80, 4
  verts), twice over - i.e. a 1024x512 CMPR image, a 512x512 CMPR image and a 512x336 RGB5A3 image. These are
  the right textures for a title backdrop/logo, and every one of them measures **`EFB x 0..0 y 0..0`**: zero
  area. Draw 0 is reported as "the coordinates are indexed, not read here", so for the indexed ones that is
  the measurer's blind spot rather than proof, but the op-80 four-vertex draws report the same zero box, and
  the capture at frame 585 is 0 lit.
- The frame after the transition (624, 27 486 lit; 643, 51 320 lit) shows content again, then 670 drops to 119.

So the backdrop is a **transition-time** asset, submitted only in frames 590-605, and its draws are the ones
to instrument - not the page quads. The bridge's placement measurer does not decode an indexed position
array, so the next step is to make it read that one case: dump the position index values and the position
array's base/stride for the 12 transition draws (with `R2_GX_DUMP=590` to recover the packets offline), which
says whether the array holds a real quad that lands nowhere or an array the game never filled.

### 9.12 The transition draws are a real quad; their position matrix is all zeros (indexed-position decode)

`drawBoundsEfb` already decoded indexed positions (`VCD` position type 2/3 -> `physPtr(array + idx * stride)`) -
what it never did was **report** them, so an unmeasurable indexed draw was indistinguishable from an empty one.
`DrawBounds::PosIndex` now records the position type, the array base/stride, the first four index values and the
position each resolves to, and `logDrawIndex` prints it for every draw that placed nothing or is one of the
frame's large ones (`R2_DRAW_LIST`). `R2_GX_DUMP=590` carries the same indices and the raw array words.

`R2_DRAW_LIST=1 R2_DRAW_LIST_AT=590 R2_GX_DUMP=590`, 360 s run (frame 590 at 197 s - the instrumentation
halves the frame rate, so the transition needs a ~4 minute boot now):

- **The array is filled, with real geometry.** Frame 590 draw 11 (`015A84A0 512x336 fmt 1`), 8-bit indices,
  array `012EEA40` stride 12: `[2]=(-256, 0, -168) [3]=(256, 0, -168) [1]=(256, 0, 168) [0]=(-256, 0, 168)` - a
  512x336 quad in the XZ plane, exactly its texture's size. Draw 3 (`0154DC80 512x512`), array `012EF2C0`:
  `(-112.875, 0, -143.191) (128.743, 0, -143.191) (128.743, 0, 132.944) (-112.875, 0, 132.944)` - a 241.6 x
  276.1 rectangle. Draws 0/1/2 (`014F8740 1024x512`, op 98) index a shared array `012EEE60` and span
  x -319..319, z -225..73, i.e. the same quad split into strips. **Nothing is missing from the vertex arrays.**
- **The placement fails before the transform: the position matrix is all zeros.** Every one of the 12 draws is
  reported as `bounds: no extent for v80/v98 ...: every vertex behind the eye`, and the frame's dump shows why:
  `CP 0x30 = 3CF3CF00` selects matrix 0, and `LOAD_INDX A idx 0 len 12 dst 000 from 00D982A0` loads it from
  memory that is **all zero** - the same zeros are still there in the dump's end-of-frame `LATER 00D982A0:`
  line, so it is not a read-too-early artefact of the measurer. With `m` zero, `ex = ey = ez = 0`, so `ez >= 0`
  for every vertex and a perspective projection has no window position to compute (w = 0).
- **The same matrix is loaded for the normal-matrix and texture-matrix slots** (`dst 400` from `00D982D0`,
  `dst 078` from `00D982F4`, also all zeros), so this is one zeroed scratch block, not one bad matrix.
- Frame 590's capture is 0 lit and the frame holds exactly these 12 draws, so on this build the whole
  transition really does rasterise nothing.

So the answer to 9.11's question: **a real quad, in an array the game did fill** - the fault is upstream of
placement, in the matrix the draw is transformed by.

**Control (`R2_GX_DUMP=300`, 150 s run, an intro frame that renders 30 000+ lit):** the same `LOAD_INDX A dst 000`
zeros appear there too (854 direct `XF 0000 x12` writes in that frame as well), so matrix 0 is not where this
frame's camera lives either. What the intro has that the transition does not is the **direct path**: real
matrices arrive through `XF 0000 x12` writes (`1, -0, 0, -118.8 / 0, -3e-6, -1, 250.5 / 0, 1, -3e-6, -722.7`),
which is the transform the visible 3D draws use. Frame 590 has 16 of those writes, but **every one of the 12
transition draws sits directly behind its own `LOAD_INDX A idx 0 len 12 dst 000` from a zero block**
(`00D982A0` before draws 0-2, `00D98340` before draw 3, `00D983E0` before draw 4, `00D98480` before draw 5, and
so on - the blocks are 0xA0 apart, one per draw), so each draw is issued with matrix 0 freshly zeroed.

Next step is therefore **not** placement or Aurora state: something should be filling those 0xA0-spaced blocks
(the game's per-draw matrix scratch) and in this build it either never runs or writes elsewhere. Find the
writer of `00D982A0` in the recompiled code and check it is reached on this path; the direct `XF 0000` path
working is the proof that the rest of the chain (matrices -> projection -> EFB) is fine.

### 9.13 The matrix slots are a recycled 1920-byte pool: cleared at frame 226, never refilled

Finding the writer needed a store watch the Switch build does not have, so `R2_MATWATCH=<phys>,<len>` samples
the block at every frame end instead (read-only, one `memcmp` per frame; `R2_PEEK` is desktop-only, it lives in
`main.cpp`, and `R2_WATCH` is compiled out of the guest stores by `ctx.h RT_WATCHING`). Watching the 12-slot
position-matrix block the transition draws index (`00D982A0` + 0x780, the slots 0xA0 apart):

- **frame 0: 480/480 words nonzero** - `AA90DFFD C6C747D7 69B72325 ...`: stale heap content. The block is a
  scratch buffer, not a loaded resource.
- **frame 226 (62.3 s, the menu -> cinematic change): 480/480 words zero.** One owner clears all 1920 bytes.
- **frames 240, 360, 480, 600, 720: unchanged, 0/480.** The whole transition (590-605) runs on zeros.
- **frame 766: 102/480 words nonzero**, 775/781/781 smaller - the *same* buffer is filled and used later.

So there is **no store to trace**: nothing writes the block between frame 226 and frame 720, and nothing at all
writes it during the transition. The zero matrix is not a store that lands elsewhere, and it is not a
cache/DMA visibility problem - the fill simply never runs on this path.

Frame 590 in the same run (216.6 s): 12 draws, none measurable, largest `EFB x 1e40..-1e40` (the zero-matrix
signature), and the dump's `BP 7E = 5C5211` puts all 12 inside `hle_CutsceneVideoDraw`'s bracket - they are the
intro cutscene's video strips (1024x512 CMPR x3, 512x512 CMPR, 512x336 RGB5A3), each issued with its own
per-strip matrix slot, all twelve zero.

What this leaves: whatever should write the 12 strip matrices after the pool is cleared. The fill exists -
the same buffer is filled at frame 766 - so the transition path either skips it or feeds it zeros. Two things
to keep in mind before spending another run on it: a zero position matrix is also how this game *hides* a draw
(loading zeros immediately before it is a deliberate two-instruction hide, not an accident), so this target is
only the missing backdrop if the 766 fill turns out to be the same 12 strips. Naming the filler needs a guest
store watch (`-DRL_NX_WATCH`, off in the Switch build for speed) or an `R2_LOGFN` on the candidate function
once the branch that skips it is found in `f_7FD68C54`.

### 9.14 `R2_ZEROMTX=keep`: the backdrop comes back - the zero load is a lost matrix, not a hide

`R2_ZEROMTX=keep` makes the bridge drop a 12-word position-matrix `LOAD_INDX` when the block it reads is all
zero, out of its own shadow *and* out of Aurora's FIFO, so slot 0 keeps whatever matrix the last direct
`XF 0000 x12` write left there (the camera). Same NRO, same boot.cfg (`R2_DRAW_LIST=1 R2_DRAW_LIST_AT=590
COS_DK_CAPTURE=1 R2_CAPTURE_FROM=205`), 380 s each, only the switch differs. Captures: `build-switch/tmp/zeromtx1`
(on), `build-switch/tmp/zeromtx0` (off).

| frame | switch off | switch on |
|---|---|---|
| 590, its 12 transition draws | 0 cover 40 %, largest `EFB x 1e40..-1e40`, none measurable | **6 cover 40 %**, largest `EFB x -128..525 y -94..212` (1024x512 CMPR) |
| 607 / 619 (transition) | 0.0 % lit, black | 19.1 % lit (cockpit + the video strips) |
| title, 1034-1086 | 0.9-1.0 % lit - black plus the START/OPTIONS glyphs | **58.9 % lit - the backdrop art** |
| 621/638, 636/660, 792/790, 810/809 (the cinematic) | 38.8, 74.9, 88.9, 90.8 % | 61.6, 18.5, 87.5, 98.2 % - the movie renders either way |

The last row matters: the cutscene movie (the explosion) was already rendering, so the *movie* was never the
missing asset. What the zero load suppresses is the **backdrop layer**: the title's art (1 % -> 59 % lit at the
same title frames) and the transition's video strips. And the strips place *sensibly* with the stale camera
matrix - a quad of about 653x306 pixels, not garbage - which is the answer to the question 9.12 left open:
these draws are the missing picture, and the game zeroes the matrix because the real one was never computed.

Still open: the recovered title frame carries a solid green rectangle across the left two thirds (the art
behind it is right), and frame 507 shows a large figure over a wall of small tiles. Both need a look with
the texture instruments before anything is called fixed - a wrong-format or not-yet-uploaded texture sampled
by a quad that was previously invisible would look exactly like this. The switch is diagnostic: it drops the
zeroing load in every scene (first drops at 59 s, from the `00E28xxx` blocks of 9.12's control), and the real
fix remains filling the 12 matrix slots (9.13), not skipping their loads.

### 9.15 The green title quad and the tiled mosaic (`R2_DRAW_LIST`, same boot as 9.14)

Dump at frame 1058 (the title, with `R2_ZEROMTX=keep` so the layer is drawn at all):

- **The mosaic is the game's own collage, not corruption.** 152 draws, of which ~150 are small quads
  (64x64 up to 256x64) each with its own texture address in a run (`0072C4A0`, `0072D4A0`, `007334A0`, ...),
  tiled across the EFB at 1:1 sizes. It is in the switch-off control as well (frame 506: 37.2 % lit against
  40.9 % with the switch), so it is neither the recovered layer nor a defect - it is the title's backdrop art.
- **The green rectangle is the cutscene video layer, drawn before it has frames.** `frame 1058 draw 44`:
  texture `00EAD9A0 512x336 fmt 1` (RGB5A3), positions `(-256..256, -168..168, 0)`, EFB `x 64..576 y 72..408` -
  a 512x336 (the movie's own resolution) quad centred on the 640x480 screen. With the switch off that draw is
  not drawn at all: the game zeroes its matrix. A YUV/YCbCr surface with no frames uploaded has U = V = 0,
  which decodes as pure green, so this is the *texture's* state, not a placement fault - the game hides this
  layer precisely while it is empty.
- Open: `R2_TEX_DUMP=texdump` wrote no files in this build (the relative directory does not exist and the save
  fails silently); a re-run with `R2_TEX_DUMP=logs/texdump` settles texture-content vs sampling.

### 9.16 Chasing the fill: no guest word store reaches the pool

Three store-watch runs on the 12-slot pool (`00D982A0` + 0x780):

1. Stock build, `R2_WATCH=80D982A0,780`: nothing - the watch is compiled out of the guest stores on the Switch
   (`ctx.h RT_WATCHING`).
2. `RL_NX_WATCH_BUILD=1` (new switch) with the same range for 380 s: nothing, while `R2_MATWATCH` in the same
   boot shows the block going from stale heap content to **0/480 words at frame 226** - a 1920-byte clear
   that never reaches `watchStore` (9.17 shows `dcbz` is not it either).
3. With the test changed to compare the store's *host pointer* (so the uncached `0xC...` alias and VM-mapped
   writes count too), 240 s and 300 s runs: still nothing. The fill at frame 766 was never caught.

Code side: the 12-word matrix builder is `f_7FC0A9A8` (a 3x4 from a source matrix and a 3-float scale,
`dst+0x00..0x2C`). Its only call site on the cutscene path is `f_7FD68C54_orig -> f_7FC0A9A8(r3 = obj+0x84,
r4 = r1+0x10, r5 = obj+0xE0)`, i.e. into a **stack** buffer, and the result goes to
`f_7FC11DDC(obj->0xC8, 0, &stack, 1.0f?, 0)`. **Nothing on that path writes the pool.**

So the missing piece is not a store that lands at the wrong address: the per-strip matrices are never built on
this path at all. The pool's *actual* writer is still unnamed - see 9.17.

### 9.17 No instrumented writer explains the refill (`R2_DCBZ`, DMA watches)

Added `R2_DCBZ=<phys>,<len>` (+`R2_DCBZ_BT`) and re-ran the boot that reaches the refill, with the reader, the
store watch and the dcbz logger on the same block for 430 s:

| observation | value |
| --- | --- |
| pool at frame 812 | 93/480 words nonzero (`R2_MATWATCH`) |
| `dcbz` touching the pool | **0** |
| guest word stores into the pool | **0** |

So neither a cache-block zero nor any guest store writes the pool. The obvious remaining candidate is
`lockedCacheDma()` (`cpu.cpp:230`), which copies through a plain `memcpy` and so is invisible to `R2_WATCH`.
`watchHost(mem, bytes, "locked cache DMA")` was added there and the boot repeated (`R2_WATCH=00D982A0,780`,
physical form, which is what `watchHost` compares):

| writer | instrument | fired on the pool |
| --- | --- | --- |
| guest word stores | `R2_WATCH` (watch build, host-pointer test) | no |
| `dcbz` block zero | `R2_DCBZ` | no |
| locked-cache DMA | `watchHost` in `lockedCacheDma` | **no** |
| DVD read | `watchHost` (`hw.cpp:274`) | no |
| ARAM DMA | `watchHost` (`hw.cpp:1008`) | no |

Yet the same boot shows the pool going from 0/480 words at frame 720 to 93/480 at frame 786. **No instrumented
writer explains the refill.** Two things must come before another theory: a positive control that `watchHost`
fires at all (watch a DSP buffer range that a known boot writes), and a first-difference dump in `R2_MATWATCH`
(which word offset changed, and to what) instead of just "changed" - that names the slot, and with it the array
layout the game uses.

### 9.18 Summary of the title-screen investigation (what is settled, what is not)

Settled:

- **The tiled mosaic is the game's own art.** ~150 small textured quads at distinct addresses, in both switch
  states; not a defect (9.15).
- **The green rectangle is the cutscene video layer, drawn before it has frames.** A 512x336 (the movie's own
  resolution) quad at `x 64..576 y 72..408`; the game normally hides it with a zeroed matrix, and a video
  surface with no frames has U = V = 0, which decodes green (9.15). Texture content is still unviewed: the
  dump's files go to the SD card root, and no run so far reached the frame that binds it.
- **`R2_ZEROMTX=keep` is not a fix.** It restores the title backdrop and the transition (1 % -> 59 % lit,
  0 % -> 19 % lit) but also un-hides the empty video quad, so it is a diagnostic only (9.14).
- **The 12 matrix slots are a 1920-byte recycled pool** at `00D982A0`, cleared at frame 226 and refilled at
  frame ~786, by a writer still unnamed: guest stores, `dcbz`, locked-cache DMA, DVD reads and ARAM DMA are all
  instrumented and none of them fires there (9.13, 9.16, 9.17).
- **The 12-word matrix builder on the cutscene path is `f_7FC0A9A8`**, called from `f_7FD68C54_orig` into a
  *stack* buffer and handed to `f_7FC11DDC`; it never writes the pool (9.16).

Not settled, and what each needs:

1. **Which code writes the refill.** First a positive control on the new instrumentation (a `watchHost` range
   a known boot does write), then `R2_MATWATCH` with a first-difference dump so the changed slot offset is
   known - one boot each.
2. **Whether the green is empty pixels or a decode fault.** Run with `R2_TEX_DUMP=texdump` (now resolved to
   `sdmc:/switch/RL-NX/texdump` and logged on first write) and a boot that reaches frame ~1034, then view
   `tex1_512x336_*_1.png`.
3. **Why the pool is empty exactly where the strips need it.** With (1) answered, compare the DMA's source
   pointer across frames 226-797; an array that is never re-set is the likely fault, not the matrix math.

### 9.19 Root cause of the missing art: locked-cache accesses fell through to the arena (fixed 2026-11-24)

Section 9 chased this from the draw side for a day. The fault was one window in the memory model, not GX at
all, and it is Switch-only, which is why `red_two.exe` on the PC renders the same recompiled code correctly.

`ctx.h` maps guest memory with `hp()`, which on the Switch has a locked-cache branch
(`0xE0000000-0xE03FFF` -> `lockedCachePtr()`), while the typed loads and stores went through `hpr()`/`hpw()`,
which were `isVm(a) ? ... : memPtr(a)` and never had that branch. `memPtr` is `g_ram + ((a - 0x80000000) &
0x3FFFFFFF)`, so `0xE0000000 + off` resolved to **`g_ram + 0x20000000` - 512 MiB in, past the end of the
256 MiB arena**. On the PC build `memPtr` is `g_base + a` over a 4 GiB reservation and `memory.cpp` keeps the
locked cache at `g_base + kLockedCache`, so there the two paths already agreed; the console's arena is the only
place they could disagree, and nothing caught it because `cpu.cpp`'s `lockedCacheDma`, `dcbz`/`dcbzl` and the
save state all use `hp()`.

What that did: the recompiled game builds data in the locked cache every frame (the generated code reads and
writes offsets `0x0CC`, `0x10C`, `0x12C` of `0xE0000000`; Factor 5 stages XF matrix and display-list data there),
all of it went to unrelated heap and read back as zeros, and `lockedCacheDma` then copied the untouched,
still-zeroed locked cache out to the object's matrix array in main memory. That is exactly 9.12's zeroed
`LOAD_INDX` source, 9.13's "pool cleared and never refilled", and 9.16/9.17's "no writer is ever seen" - the
writer *was* seen, as the DMA, but it was copying zeros - and it is why `R2_WATCH`/`watchHost` never fired on
the pool: the stores never reached guest RAM at all.

Fix: `hpr`/`hpw` now return `hp(a)`, so typed loads, typed stores, `dcbz`, the DMA and the save state all agree
on where the locked cache is. One line each, `ctx.h`; the PC maps are unchanged.

Verified (Ryujinx, 300 s boot of the 12:18 build with `COS_DK_CAPTURE=1 R2_CAPTURE_FROM=200`, no fatal, no
write failures): the presented frame is `lit=287600..288000/288000` through the late intro, where the broken
builds measured `lit=3221/288000` on the title and `0` on the transition frames. The captures show the
**LucasArts logo** (frame 572), the **complete clone army** with its platform, the **X-wing** (715) and the
**Death Star explosion with the ship** (665) - all items from 9.1's "missing" column. Not yet re-checked on
hardware.

### 9.20 The in-level flicker: what the 12:56 logs rule out (open, 2026-11-24)

The locked-cache fix (9.19) cleared everything up to a level. In a level, the buildings on the surface and
the other craft flicker out and back while the HUD, the starfield and the four engine glows stay. Two logs
came with it: `log.txt` (hardware, 12:53) and `profile-006.txt` (the session's first 60 s).

What the run shows, in order:

- The attract demo enters mission 1 at ~88 s with no input. Level frames are a single `640x304` display copy
  (stride 1280) - the game's own 2.105:1 letterbox - at 25 fps settling to 11-19 fps, 2265 draws (7486 merged)
  per present in the first report.
- Nothing is rejected in-level. No `skipped` draws, no `frame buffers full`, no `copy dropped`, no
  `not in XFB`, no texture or array complaint, and the shader cache is warm (341 shaders).
- The scissor box offset (BP 0x59) is last written at 65.76 s with `0,0`; the intro's alternating `0,238` is
  gone before the level starts, so the sticky `s_boxBiasSent`/`s_boxOffY` pair is not shifting the level.
- `profile-006.txt` is the intro, not the level (`from 0.0`, `seconds 60.0`, 1571 frames in that span), and
  its 74 % of samples at `svcWaitProcessWideKeyAtomic` is the port's own condition-variable wait - the FIFO
  drain / `gpSync` behind Aurora - not libnx idling. So the thread that builds the frame is mostly *waiting
  for the renderer*, which the numbers below confirm.

Two numbers were added to the `R2_STATS` line to make that measurable (diagnostic only, nothing else
changed):

    frame 60:  484 draws ... ; 0 indexed XF loads, 0 KiB still queued at frame end          (intro, menus)
    frame 240: 5604 draws ...; 817 indexed XF loads, 921 KiB still queued at frame end     (level)
    frame 300: 8750 draws ...; 2983 indexed XF loads, 1204 KiB still queued at frame end   (level)

`still queued` is `fifo::get_buffer_size()` sampled in `endFrame()` before `aurora_end_frame()` drains it:
in a level the renderer is 0.9-1.2 MiB of command stream behind the guest when the guest has finished
building the frame, and the entire indexed-XF-load traffic of the game happens in those frames.

Tested and **ruled out**: the obvious consequence of that backlog is that `LOAD_INDX` is decoded late, so
the renderer transforms a draw with whatever the matrix pool holds when it gets there instead of what the
game named. Implemented as an A/B that emits the same words as an immediate XF load (`0x10`) at submit time,
then ran both paths on the same build over the same window: the presented frames were *pixel-identical*
(`lit` 204683 / 199978 / 205216 at the same scenes, same 886 KiB queue, captures one present apart because
Run 2 booted a frame earlier). The guest does not rewrite that pool before the renderer reaches it in these
scenes, so the late read is not the fault and the change was reverted - only the counters stayed.

Still open, and the strongest remaining lead: at submit time, from a submit-time matrix shadow that the A/B
shows is faithful, **half of a level frame's draws land wholly outside the game's own scissor** (2924 of
5604, 4513 of 8750) and 12-18 are "absurd" (>4096 px), where every pre-level frame measures 0 outside and 0
absurd. Either the level really submits most of its geometry off-screen (possible for this engine) or the
level's scissor is a band that the frame does not fill, which would clip exactly the world this report is
about and leave the HUD (drawn in its own pass) alone.

Next measurement, one boot, no rebuild: `R2_STATS=1 R2_DRAW_BOUNDS=1` in `boot.cfg`. With stats on, the
frame's largest scissor rejection is printed with both sides - the draw's box and the scissor it was tested
against - and `R2_DRAW_BOUNDS` also prints the frame's union of draw extents. A level mission is needed, so
let the attract demo run (no input, mission 1 at ~88 s on hardware).

## 9.21 A menu-chosen mission aborts entering play (2026-11-24): what is already settled

Two crash reports (`+0x325dff0`, 13:59 and 14:03) resolve to `svcBreak` under
`dk::detail::RaiseError`, reached from `submit_list()`/`submit_readback()` in `switch/deko/device.cpp`, the
frame being replayed from `end_frame`'s copy arena. The reported result `0x367` is
`MAKERESULT(359, 1)` = `RaiseError(DkResult_Fail)`, which deko3d's release build emits from
`dkQueueSubmitCommands`/`flush()` when `m_state == Error` - the queue had already taken a GPU error
notification, and this abort is the *next* call after it, not the fault. `DK_WARNING` compiles out of the
release library, so `checkError()`'s page-fault/method-error report was never written: the log simply stops
in the level-load block burst.

`GPFIFO: at most 0 entries per kickoff` in every stats line was the clue: the `nvGpuChannel*` wrappers in
`switch/deko/host/gpu_submit_diagnostics.cpp` were never actually linked (`--wrap` was only in a comment), so
they, and their failure reporting, were dead code. They are on the link line now, and
`nvGpuChannelGetErrorNotification` is wrapped too: it prints the notification, the driver's `NvError`
(page fault address and access, method error, rejected list, timeout) and `kartpad_deko_log_failure_context()`
before deko3d consumes the event.

Already ruled out for this crash: the build difference. The 13:52:15 build that crashes differs from the
12:18:06 build that played the level only in `src/runtime/gfx/gx_bridge.cpp`, whose additions are all behind
`s_statsOn` (off on hardware: `boot.cfg` there has `R2_PROFILE`, `R2_PAD_LOG`, `R2_COPY_LOG` only), so it is
behaviourally the same code. The likely difference is the scenario: the 12:56 log is the attract demo, which
uploads the level's textures (18 MiB in one report) while the crawl plays, while a menu-chosen mission leaves
that same upload to the first in-level frame - exactly where the fault lands.

## 9.22 The menu-chosen mission's timeout, named (2026-11-24, build 14:19 + the 14:27 log)

The wrapped channel calls finally produced the fault line:

```
[  128.515 gfx   ] display copy src 0,16 640x450 stride 1280 -> 0025CAE0: XFB line 0 of 450 (5486 draws in the frame)
[gpu-submit] kickoff failed: rc=0x00000d5c (2348-0006), fd=24313860, entries=109/2048,
             syncpoint=26 value=574009 increment=42, iova=0x0 words=0 flags=0x0 threshold=0
[gpu-progress] CPU=277751 retained_from=261368 last_GPU_report=277708 first_missing=277709
[gpu-submit] frame=1971 submits=4276 pass_draws=1 pipeline=TexCopyConv Z8 ... target=Copy Conv Texture (48x48)
[gpu-submit] error_info rc=0x0000275c type=0x0
```

What that settles:

1. **The result is the channel timeout, not a fault notification.** `2348-0006` is the code this port already
   documents for a scene-load timeout (`switch/deko/device.cpp`, `gpfifo_entries_recorded`;
   `switch/src/perf_switch.cpp`). `nvGpuChannelGetErrorNotification` printed nothing at all (its line is
   absent) and `nvGpuChannelGetErrorInfo` refused with `rc=0x275c`, so there is no page-fault address, no
   method error and no rejected-list detail to read: the driver stopped the channel, it did not report a bad
   command.
2. **Where the GPU stopped:** serial 277709, the *first* operation of the batch submitted before the failing
   kickoff, with the CPU already at 277751 - about 42 operations queued behind it, all `buffer to image`
   uploads into the level's textures (the recent-op list shows the 512/256/128/64/32/16/8 mip chain, a 16x8,
   a 256x16 and a 184x104 upload). The kickoff that failed carried only 109 of 2048 entries, so this is not
   the FIFO-overflow path the 128-entry split was added for.
3. **The same work succeeds on the other path.** The 12:56 attract-demo run uploads the level's textures
   while the crawl plays (~18 MiB in one stats report) and enters the same level. So the payload is not
   inherently bad; a menu-chosen mission sends that burst inside the first in-level frame instead, together
   with the frame's 5486 draws.
4. **The mitigation this port would want is compiled but dead.** `synchronous_operation()` in
   `switch/deko/backend_commands.cpp` returns false unless `kartpad_gpu_transition_guard` exists, and
   `switch/src/transition_guard.cpp` is absent from this tree, so no operation is ever made to complete
   during a load. SFA-NX's own note on that guard is exactly this failure: "every recorded 2348-0006 timeout
   came while a course loaded".

Instrumentation added for the next boot (diagnostics only, no behaviour change): `gpu_operation_buffer_image_copy`
records the upload's destination image label and address, format, mip, destination origin, staged extent,
row pitch/rows and the staging range, so `[gpu-breadcrumb-copy]` now names the upload a timeout lands in.
`[gpu-breadcrumb-copy] serial=... extent=WxHxD` is printed for uploads (it is 0 for image-to-image copies,
whose extent is not recorded).

Cheapest A/B before any rebuild: `perf.cfg` with `gpu_steady_v9 1` (flush every 32 operations) submits the
same burst in smaller pieces; if the level then loads, the timeout is about how much of a load is in flight at
once rather than about one bad command.

## 9.23 The flicker on video: what the frames actually show (2026-11-24)

`RL_Flicker1.mp4` / `RL_Flicker2.mp4` (1280x720, 30 fps capture of a 10-15 fps present rate, 30 s each).
Frame-difference analysis and frame extraction (`ffmpeg` from `imageio-ffmpeg`, decoded to gray):

- Capture frames come in runs of 1-5 identical images then one changed one - the game presents ~10-15 a
  second, so the capture cannot show per-present alternation directly; the flicker is coarser than that.
- **In the bad frames the whole 3D pass is absent, not scrambled.** Flicker1 frame 36 (t = 1.2 s): starfield,
  HUD (reticle, radar, throttle, targeting computer), the engine-glow sprites - and **no X-wing hull, no
  trench surface, no TIE fighters**. Frame 15 (t = 0.5 s): planet surface present across the top 80 %, stars
  below. Frame 578: everything present and correct.
- The survivors are the things that do not depend on the depth test: the 2D HUD (screen space), the particle
  glows and the starfield (drawn first, at the far plane). What disappears is exactly the depth-tested,
  depth-written opaque geometry - so the lead is the depth buffer's contents or compare, not transforms,
  textures or submission. A draw rejected by depth leaves no trace, and a mixed frame (some passes cleared,
  some not) is exactly "some elements, not others".

Structure of a level frame, from the same build's log: two EFB passes per frame (scissor box offset `0,238`
for one half, `0,0` for the other; 1288 of each in a 65 s run, strictly alternating), each half rendered into
EFB rows 0..225 and composited into one `XFB` 640x450 texture (`display copy src 0,16 640x226 -> line 0 of
450`, then `src 0,2 640x226 -> line 224`), presented from that texture (`present from XFB 640x450`).

Depth clearing in that path (`extern/aurora/lib/gfx/recording.cpp`): a display copy's clear is applied by
`resume_pass_after_copy`, which clears depth **only when the copied rect is the whole target**
(`.clearDepth = clearDepth && fullTarget`); for a half-frame rect it becomes a scissored clear draw
(`push_clear_draw`, whose pipeline does write depth, `gfx/clear.cpp:124`). So the second half of every frame
depends on that scissored clear having landed - and on the game asking for a depth clear on the copy at all.

**Local reproduction attempt:** Ryujinx with `R2_STATS=1 R2_DRAW_BOUNDS=1 R2_DRAW_LOG=1` reaches the level and
reproduces the *statistics* exactly as measured on hardware - frame 300: `9298 draws (9218 measured, 80 empty
strips, 4784 wholly outside the scissor, 14 absurd)`, `1121 KiB still queued at frame end`, largest reject
`v80 4 verts (3D), box x -99563..-98656 y 282..878 vs scissor x 340..979 y 356..597`. So the draw/scissor side
of the level is reproducible off-console. The presented frames captured there (`COS_DK_CAPTURE=1`,
`R2_CAPTURE_FROM=104`, frames 280/286/293/300, a ground scene with characters) render completely - **that
scene does not flicker in the emulator**, so the flicker itself is either level-specific or hardware-only, and
the emulator is good for the state machinery, not for seeing the symptom.

**Decisive, no-rebuild test:** `R2_GX_OVERRIDE=zalways` in `boot.cfg` relaxes every draw's depth test
(`gx_bridge.cpp`, BP 0x40 -> compare ALWAYS) without touching anything else. If the world then stays on
screen (wrong about which parts overlap, but present every frame), the geometry is being **rejected by the
depth test** and the fault is in the depth buffer's clear/compare across the two half-frame passes; if it
still disappears, the draws never reach the EFB and the half-frame compositing is the place to look.

## 9.24 Per-half depth-clear log, and the skip trigger in Ryujinx (2026-11-24)

**What was added.** `R2_DEPTH_LOG=1` (`boot.cfg` or the environment) makes
`resume_pass_after_copy` in `extern/aurora/lib/gfx/recording.cpp` print one line per render pass that a copy
ends. A level frame ends two of them (top and bottom half), so the log answers "was the EFB depth actually
cleared for this half?" directly instead of leaving it to the `zalways` A/B:

    [depthclear] frame 283 half 0 pass 3: copy 0,16,640x226 of 640x480 full=false xfbLine=0 draws=784 \
      req color=true alpha=false depth=true -> passDepthClear=false depthDraw=true \
      passColorClear=false colorDraw=true z=0.0000

- `half` restarts at every frame and counts only copies into a composed XFB (`xfbLine` = the XFB line the copy
  lands on, 0 for the top half, 224 for the bottom); `-1` is a copy into a texture, not the picture.
- `draws` = commands in the pass being ended, so a half that renders nothing before its copy is visible as such.
- `req ...` is what the game asked for (`clear && gxState.depthUpdate`, etc.).
- `passDepthClear` = the depth attachment's `LoadOp::Clear` for the next pass; `depthDraw` = a scissored
  `push_clear_draw` inside it (`gfx/clear.cpp` writes depth). Both false = the half draws over whatever depth
  the copy left behind - the state that loses geometry.

**What it shows so far.** A Ryujinx level run (`R2_DEPTH_LOG=1 R2_STATS=1`, 497 lines, frames 1-283) reports
`passDepthClear=false depthDraw=true` for every half: a 640x226 copy never covers the 640x480 target, so the
clear is always the scissored draw, and the game does request it (`req depth=true`) on both halves. So no half
in that run loses its clear, and the emulator cannot show the flicker's clear state either way - the hardware
log this switch now produces is what settles it: look for a half whose line reads
`req depth=true -> passDepthClear=false depthDraw=false`, or `req depth=false`.

**Skip trigger, scripted in Ryujinx.** `R2_INPUT` presses the pad from the port's own input script, so it works
in the emulator without a controller. Findings:

- The script clock is *not* frames: `scriptTime` advances 59.94/viRate per poll and the game polls far more
  often than it presents, so script units run ~15-40x per wall second while Aurora frames run ~2/s on Ryujinx.
  `R2_PAD_LOG=1` is the way to place a press: `buttons 0100` (A) and `1000` (START) appear as they fire.
- Pressing A (and START, and all four face buttons) across the attract sequence and across the hand-over into
  the level (presses confirmed in the pad log at 45.1, 47.9, 51.7 and 56.2 s, level entry 53.8 s) changes
  nothing: the level starts at nearly the same frame with or without input (231/233/236) with the same first
  half carrying 502 draws. The skip therefore does not reach a distinguishable guest path in this boot.
- No `2348-0006` channel timeout, no `kickoff failed` and no submit abort appears in any Ryujinx run - the
  GPFIFO stayed at "at most 110 entries per kickoff" with 0-13 list splits. The crash is a real-GPU channel
  timeout, so the emulator cannot be made to produce it; the `[gpu-breadcrumb-copy]` record still needs one
  hardware boot of the skip path.

## 9.25 `R2_DEPTH_LOG` on hardware: neither half loses its depth clear (2026-11-24)

**Run.** `E:\RL-NXLOGS\LOGS 11242026-1716\log.txt`, the NRO with the `[depthclear]` log, `R2_DEPTH_LOG=1`,
23,120 lines over 203 s. It reaches the level (first bottom-half copy at frame 374, last at 3353) and plays
~400 two-pass frames before returning to the attract loop.

**Result: the clear is never lost.** 14,940 `[depthclear]` lines, 10,881 of them with `req depth=true`. Not one
of those reads `passDepthClear=false depthDraw=false`: 164 passes clear by the depth attachment's
`LoadOp::Clear` (all 164 are `full=true` copies covering the whole 640x480 target) and the other 10,717 clear by
the scissored clear draw inside the new pass. The only XFB halves with `req depth=false` are two boot-time
640x450 attract copies; no level half. So the hypothesis `docs/PERF_HANDOFF.md` 9.23/9.24 was built to test -
that one half-frame pass draws over the previous half's depth - is **ruled out on hardware**. That removes
`R2_GX_OVERRIDE=zalways` as a test of the *clear* (there is no missing clear to compensate for) but not as a
test of the *compare*: it is still the only cheap way to tell "rejected by depth" from "never rasterised", and
9.26 keeps it as the first test for the flicker.

**Coverage per half - corrected 2026-11-25.** 401 frames are true two-half frames (400 bottom-half lines,
633-1140 draws per half, `xfbLine=224`): frames 374-686 at 8.6-38.0 s and 3241-3353 at 195.7-199.9 s, i.e. the
live level and nothing else. The other 3059 frames copy a single pass, and they are *not* level play: 2872 are
full 640x450 copies (the attract loop and menus - 7-15 draws a frame at 38-55.8 s, 1,300-1,700 at 61.6-195.6 s
while the attract demo plays a mission) and 187 are 640x304 letterboxed copies (frames 1203-1389, 56.6-61.5 s,
6 draws: the crawl/transition). Every level-play frame in this run is two-half, so "much of the level produces
one `half 0` line each" (the earlier wording here) was wrong: it is the attract loop, the menus and the crawl.

**The clear band cannot lose the world - see 9.26.** `clearRect` is clamped from the same copy rect whose pixels
the display copy reads, so a half clears exactly the rows that copy will show. In the two-half mode the
`0,238` pass's scissor is EFB `y=0..241` and its copy band `y=2..228` (Ryujinx, `R2_DRAW_BOUNDS`: frame 180/600,
`last scissor x 340..979 y 340..581`), the band sits inside it, and the only rows a half draws that it does not
clear (`y=228..242`) are rows its own copy never reads, or that the other half's copy overwrites in the XFB's
2-line seam. Worse for the hypothesis: the mission that actually flickers is composed differently again (one
640x450 copy per frame, 9.26), so there is no per-half band in it at all.

**Crash: not reproduced on this build.** Zero `kickoff failed` / `2348-` / `2345-` / abort / panic lines in the
full 203 s log, level included. The `[gpu-breadcrumb-copy]` record stays armed for the next occurrence and
nothing was changed for the timeout.

## 9.26 The clear band is dead; what is left to be wrong (2026-11-25)

Two measurements were taken for the lead 9.25 ended on: a level run in Ryujinx with `R2_STATS=1
R2_DRAW_BOUNDS=1 R2_DRAW_LIST=1 R2_COPY_LOG=1 R2_DEPTH_LOG=1` (`build-switch/tmp/halfbounds/SMOKE
11242026-190707`, 2,584 `[depthclear]` lines, frames to 678), and a recount of the *flicker session's own*
hardware log (`LOGS 11242026-1256`).

**1. The clear band covers exactly what is shown, by construction.** `resume_pass_after_copy` derives
`clearRect` from the same `rect` the pass's display copy reads, so the cleared rows are the copied rows. A half
can only draw outside its band through rows its own copy never reads. Measured in the Ryujinx level: in the
`0,238` pass the scissor is EFB `y=0..241` (`last scissor x 340..979 y 340..581`, offset already taken out) and
the copy band is `y=2..228`, so the world is inside the band, not beside it. The one uncleared strip,
`y=228..242` of the other half, is 14 rows that the two copies overwrite in the XFB's 2-line seam.

**2. The session we take to be the flicker session is not two-half at all - an inference, not a fact.**
`LOGS 11242026-1256` is *assumed* to be the run the flicker was reported from. Nothing in it says so: it names
no mission or level, holds no frame capture, and has no `[depthclear]` line (that flag was off), so the
identification rests on the report coming from this session. What the log does establish is how this level
composes. Its 4,468 presented frames use three layouts:

| layout | frames | when | draws per frame |
|---|---|---|---|
| two-half (`640x226` from y16 -> line 0, `640x226` from y2 -> line 224, plus `640x2`/`640x16` band copies) | 1,288 | 0-68 s | 500-3,000 |
| one `640x450` from y16 -> line 0 | 3,180 | 68-193 s (the level) | 3,904-12,818 |
| one `640x304` from y0 (2.105:1 letterbox) | 707 | 85-100, 110, 135-140 s (cinematics) | 6-40 |

So in that session's level every frame is a single full-height pass, its clear band is `y=16..466` and its
copy reads `y=16..466`: there is no per-half band in it to lose the world with (if that is the mission that
flickers - see below). The two XFB destinations
alternate every frame (`0025CAE0` / `002F8EE0`, both `XFB line 0 of 450`) and carry the same draw count to
within 0.05% (162 frames each, 1,122,074 vs 1,121,459 draws), so the picture is not alternating between a full
and an empty buffer either. Over t>170 s: 1,075 frames in 81.7 s
(13.2 fps), 3,904-12,818 draws a frame, average 6,219.

**How much 9.26 depends on that identification: nothing.** Point 1 stands on its own - the band is derived from
the same copy rect whose pixels are shown, so a two-half mission cannot lose the world to its band either. The
identification only decides whether this session had a band at all; if the reported mission is two-half, the
flicker there is still one of the four ranked items below. It becomes fact only with a hardware boot of the
reported mission that prints its copies (`R2_COPY_LOG`, one line per display copy with rect, XFB line and draws
in the frame) at the moment it flickers.

**3. What the render path actually does, and where a whole pass can still go missing.** Per presented frame the
port decodes the GX stream (Aurora's thread), records passes on the 640x480 EFB (reversed-Z depth, color+depth
load ops), ends a pass at every EFB copy, blits the copy's rect into one 640x450 XFB texture and resamples that
into a per-frame-slot present texture. A frame boundary is a *heuristic* in the bridge: `completes` from the
copy's destination line plus `s_topCopied`, not from anything the driver reports. In the level such copies are
not one per frame - a two-half frame carries `640x2` and `640x16` texture copies and up to a dozen offscreen
copies between the world passes and the display copy, and every one of them ends an EFB pass and starts a new
one with its own scissored clear. Given that (a) the clear is complete, (b) the draws are submitted (the
flicker session's per-frame draw count never collapses - it varies smoothly 12,647 -> 5,401 in the t=183-189 s
window) and (c) Ryujinx renders the same draw set completely, the remaining ways a whole 3D pass can vanish are (re-ranked in 9.27, where depth is last):

- **depth compare/values, not the clear.** Reversed-Z (`gx::UseReversedZ`) maps the GX 24-bit z through
  `clear_depth_value()` and every compare through `gx.cpp`'s table; a mismatch shows up as "everything fails"
  and only on hardware. `R2_GX_OVERRIDE=zalways` tests exactly this and has **never been run on the flicker** -
  on the title page only (9.8). One boot, no rebuild.
- **Aurora's draw merging.** The level merges thousands of draws per frame (`aurora: N draws (M merged)`); a
  merge that joins draws with different depth state, or that drops the first draw of a run, removes geometry
  without any counter in the bridge moving. `R2_NO_MERGE=1` is the no-rebuild A/B.
- **The frame boundary heuristic.** If a band/offscreen copy is taken as the frame's end, the world pass lands
  in the packet after the present and the frame shows only what was drawn before it (sky, starfield, HUD). The
  cheap check is one run with `COS_DK_CAPTURE=1 R2_COPY_LOG=1`: a captured frame can then be matched to its
  logged draw count, and a captured frame with no world but ~5,000 draws says the draws were submitted and
  dropped later, while ~500 says the boundary was wrong.
- **Late reads (1 MiB backlog).** The XF-load half of this was A/B-ed and cleared (9.20); vertex arrays and
  textures have not been.

`R2_NO_XFB` looks like the layer-split test (present the EFB instead of the XFB) but **it is inert on the
console**: `switchInit` sets `cfg.emulateXfb = true` unconditionally and never reads the flag, while the desktop
path does (`s_xfb = !envFlag("R2_NO_XFB")`). It is also not a valid A/B here: with XFB emulation off no copy
reaches `resume_pass_after_copy`, so no pass is cleared at all and the world would disappear for a different
reason.

The boot prepared for this is in 9.27, which also re-ranks the list above: depth is no longer first.

## 9.27 The flicker is lost after recording, not in the depth test (2026-11-25)

**The argument that moved depth off the top.** A frame that shows HUD, particles and starfield but no world does
not prove its 3D pass *ran* with a wrong depth buffer. In the band layouts the presented XFB is assembled from
several copies, so the HUD rows of a bad frame can be rows an earlier copy left behind - and the "survivors" are
exactly the things that change least between frames (a static HUD, a slow starfield). For the depth story to hold,
every depth-tested draw in the pass would have to fail while every compare-always draw lands, in the same pass,
frame after frame. That is a much narrower shape than "the picture that reached the display was not this frame's
3D pass". So the tests that ask *did this frame's world reach the GPU at all* now come first and the only cheap
depth A/B (`R2_GX_OVERRIDE=zalways`) is last.

**Two things the existing logs already settle, so they stop being candidates.**

- **Aurora's per-frame buffers never fill.** `frame_has_room` (`extern/aurora/lib/gfx/recording.cpp:1590`) is
  checked before every draw (`gx/command_processor.cpp:1233`); a draw that does not fit is dropped and Aurora
  prints `frame buffers full: N draws dropped (verts a/b indices c/d uniforms e/f storage g/h)`
  (`recording.cpp:759`). No such line in `LOGS 11242026-1256`, `-1716`, `-1427` or `-1403`, although 1256 plays
  the level at 3,904-12,818 draws a frame. The capacities (10 MiB vertices, 5 MiB indices, 48 MiB uniforms,
  20 MiB storage; `gfx/resources.hpp:10-13`) are therefore not deleting the world, and this was the most
  mechanical way the port itself could have.
- **The world's draws reach Aurora every frame.** `display copy ... (M draws in the frame)` never collapses
  through the level (9.26), so the GX decode, the emitter and Aurora's recorder all see the level's draws in the
  frames that flicker. What goes missing is downstream of them.

**Which leaves the stage Ryujinx never runs.** Ryujinx draws the same recorded GX state correctly and nothing
here has ever A/B-ed this level between the desktop path and deko3d, so a fault in the conversion is both
consistent with the evidence and the one place the evidence points.

**The mid-pass split, checked against deko3d's own source (2026-11-25).** The first version of this section
ranked "the frame is cut into several GPU submits, mid-pass" first, with "pass start, draws and pass end can
land in different lists" stated as fact. That was an inference from the split counters, and it does not survive
the source:

- **deko3d has no render pass object, so a pass cannot be "contained" in a list.** `dkCmdBufBindRenderTargets`
  (`switch/third_party/deko3d/source/maxwell/gpu_3d_base.cpp:239`) writes the target address/format registers;
  `dkCmdBufClearColor` (`:449`) writes the clear. What this port calls a pass is those commands plus the draws;
  there is no begin/end pass entry point anywhere in the API.
- **Finishing a list resets none of it.** `CmdBuf::finishList()` (`source/dk_cmdbuf.cpp:35`) signs off the
  GPFIFO entry and rewinds only the control-memory bookkeeping. Command memory is rewound by `clear()`, which
  runs at frame start, and no 3D state is touched there or here.
- **The next list continues the same channel.** `Queue::submitCommands` (`source/dk_queue.cpp:275`) appends the
  list's GPFIFO entries to the queue's one channel (`appendGpfifoEntries` -> `nvGpuChannelAppendEntry`), so the
  registers the first list set are still set when the second list runs: no reset, no context switch, nothing to
  re-bind.
- **And in this build a cut cannot fall inside a pass body anyway.** Splits happen only where
  `gpfifo_entries_recorded` is called: in `barrier()` and in `Allocation::used()`
  (`switch/deko/backend_internal.h:127-140`, `:159-170`). `barrier()` is called at the top of
  `begin_pass_native` (before its target bind and clear), by the copy and upload wrappers, and - capture or GPU
  timing only - at the top of `end_pass_native`. `used()` runs only in encoder-level uploads, which WebGPU does
  not allow inside a pass. The pass's own closures (begin, state, draws, end) contain no split point, so
  completing a list between them is not what happens.

What remains of the split is an *ordering* question (a cut between Aurora's command buffers changes what the
GPU has executed when the next `dkQueueWaitFence` runs), not an illegal mid-pass operation, and the per-frame
line below reports exactly where each cut fell. It is ranked last, with depth.

**Second pass, 2026-11-25: what the logs and the code eliminate.** Rather than rank on plausibility again, every
suspect was checked against the four hardware logs and against the code path it lives in. Almost all of them die,
and it is worth writing down why, because the surviving set is small:

- **Render compression is not in play at all.** `LOGS 11242026-1256` line 1: `render targets: colour
  uncompressed, depth uncompressed (menu Render compression)`. The whole "the copy engine cannot read a
  compressed EFB" class (the reason `copy_by_draw` exists, `backend_commands.cpp:979`) does not apply to these
  runs.
- **A missing shader cannot skip a draw in them either.** Same log: `load budget 0 per frame
  (COS_DK_SHADER_BUDGET, 0 = no limit)`, and every `frame` line reads `pipelines 0 queued 393 created` - the
  pipeline count is flat from 381 to 393 across the whole run, nothing is ever queued, and with a budget of 0 a
  `Compiled` stage loads on first use. `shader_usable` (`shaders.cpp:508-530`) can then only refuse a stage that
  was never compiled, which is what the new line's `skipped` count measures. This was the top suspect and it is
  now nearly dead for this session.
- **The frame is not being cut up.** The deko stats line reports `command memory max 192-768 KiB/frame (0
  overflows)` against a 4 MiB slice (`dk.h:40`), `stream max 683 KiB/frame (0 full)`, and `2.0-2.9 submits` per
  present - that is exactly one submit per Aurora command buffer plus the present, not a 128-entry cut. The
  `list splits` figure in the GPFIFO line is `fenceBatches`, incremented only by `gpfifo_entries_recorded`
  (`device.cpp:491`), and reads 13-124 per 30 s window: 0.4-4 a second, not per frame.
- **The copies arrive, all of them, in pairs.** Replaying `display copy` out of the log: 6,458 copies, of which
  3,178 are 640x450. In the window past 60 s at heights 450/226 there is one unbroken run of 2,776 full-frame
  copies, preceded by exactly 118 `half@0` and 118 `half@224` - two halves per frame, no lone half, no frame
  whose copy is missing. A world lost to a copy that never came is out.
- **No stale descriptor, no recycled staging, no freed image under a live draw.** `descriptors.cpp` invalidates
  the descriptor cache after *every* `dkCmdBufPushData` (`:43`, called at `:62`, `:84`, `:148`), retires slots by
  frame and frees one only once `frame_done` says the GPU is past it (`:60-64`, `:105-111`); deko3d's own
  `Queue::postSubmitFlush` invalidates image, shader, descriptor and L2 caches after every flush
  (`gpu_base.cpp:28-44`). Staging memory is fenced per generation (`backend_internal.h:187-219`,
  `backend_commands.cpp:934-955`) and `staging: reusing a generation whose fence has not signalled` appears in
  none of the four logs. Image heap ranges are freed at `frame_begin` only after the slot's fence
  (`memory.cpp:317-330`). Aurora's present textures are one global XFB plus two per-frame-slot pools
  (`recording.cpp:600-627`, `:649`), reused two frames later - safe because one queue executes them in order.

What is left is not a bookkeeping bug anybody can see in a counter: it is a hardware behaviour inside the replay
of a pass, or a pass whose draws are issued correctly and still not visible. The line below is the instrument
that tells those two apart in the flicker frame, and the ranking is now flat with one question:

1. **A pass's load op or clear, in the single-pass level layout.** `begin_pass_native`
   (`backend_commands.cpp:702-742`) applies Aurora's load ops and its scissored clear draw verbatim; the 17:16
   boot proved every clear request satisfied for the *two-half* layout, but no boot with `R2_DEPTH_LOG` has been
   run on the single-pass level this section's copies come from. A pass that continues an attachment and clears
   it loses everything drawn before it.
2. **A draw whose shader is not loaded**: only a stage never compiled (the budget is unlimited), counted by the
   line's `skipped`.
3. **Aurora's draw merging** - `R2_NO_MERGE=1`, still untested in this scene.
4. **Depth compare/values** - `R2_GX_OVERRIDE=zalways`, last, for the reason at the top.
5. **The list split** - source-checked above, kept only for its ordering effect; the line says whether any cut
   fell inside a pass at all.

**Next boot: the diagnostic build, made 2026-11-25.** `build-switch/sdcard/switch/RL-NX/boot.cfg` holds

```
R2_STATS=1
R2_FRAME_LOG=1
R2_DEKO_FRAME_LOG=1
R2_COPY_LOG=1
R2_CAPTURE_ON_BREAK=1
COS_DK_CAPTURE=1
R2_CAPTURE_FROM=60
```

Copy it next to the NRO (or add those lines to the console's own `boot.cfg`) and fly the mission with the
flicker. What each line answers:

- `R2_DEKO_FRAME_LOG` - one `deko frame N:` line per presented frame, from the deko3d side rather than the
  bridge: passes begun, draws replayed, draws skipped for a shader, submits and splits, where the last cut fell
  ("between passes, after pass P" or "inside pass P after D of its draws", with the call site that took it),
  copies and bytes copied, and the XFB's share of them. `diag_frame_report` in `switch/deko/device.cpp`; the
  counters are taken in `begin_pass_native`/`end_pass_native`, `kd::draw` and the texture-copy wrapper.
- `R2_COPY_LOG` - one `display copy src ...,<h>,... -> <dest>: XFB line N of 450 (M draws in the frame)` line per
  display copy, so every frame's layout and its draw count are in the log.
- `R2_FRAME_LOG` - the `frame` line every 10 s with draws/frame and the largest frame's verts/indices/uniforms/
  storage, which is suspect 0 (capacity) answered from the game's own numbers rather than from an estimate.
- `R2_STATS` - the per-frame draw/bind outcomes, unchanged from the pre-test boot unless something else moved.
- `COS_DK_CAPTURE=1 R2_CAPTURE_FROM=60` - presented images every 5 s from 60 s on (24 at most) into
  `logs/deko-capture/`, each logged with its lit-pixel count and hash. A world-less capture is then a number in
  the log before anyone opens a PPM, and `end_pass_native` logs one `pixel pass: <label> target=<texture>
  draws=N` line per pass of the captured frame (`backend_commands.cpp:745-755`) - which pass ran, into which
  target, with how many draws.

What to look for, and what each result means:

- **`frame buffers full` appears, or `largest frame: verts` sits at 10 MiB:** the capacity was the cause after
  all, and the fix is `gfx/resources.hpp`, nothing in the depth path. (The 1256 run says this will not happen.)
- **A capture whose `lit=` count is thousands below its neighbours:** that is a world-less frame, saved. Take the
  copy lines beside it in the log. Thousands of draws in that frame + a `pixel pass` line whose target is the
  world pass with `draws=N` large => the pass and its draws were replayed, so the loss is after them (the
  copy/present path - check that frame's `deko frame` line for an `XFB` copy); thousands of draws but *no*
  world-pass `pixel pass` line => the pass was never begun, i.e. suspect 2 (load op) or 3 (merging), with
  suspect 1 named if that frame's line reports skips.
- **A low-`lit` capture whose `deko frame` line reads `last cut none` or `between passes`:** the frame's passes
  were not cut in the middle, so the split is out and the loss is in what the passes did (look at the skipped
  count and the copy bytes: a frame with no `XFB` copy, or with `0` passes, never got its picture copied out).
- **The same, with `last cut inside pass P after D of its draws`:** a cut did land in a pass body after all -
  the one thing the source check says cannot happen - and its call site in parentheses says where
  (`GPFIFO entry batch`, `a submit`, a readback). That would reopen suspect 5 as a real defect and needs the
  pass's label from `R2_DRAW_LOG` beside it.
- **`skipped for shaders` above 0 in the frames that lose the world:** suspect 1, named by the count rather than
  inferred. Cross-check `R2_STATS` and the shader journal (`Cache/deko/sources`, `dksh_local.bin`) for a stage
  the level streams in mid-mission.
- **`passes` dropping to 1-2 (from 6-12) in a low-`lit` frame while the copy lines still show thousands of
  draws:** the frame's 3D pass was never begun, i.e. Aurora's pass was not converted at all - the load-op and
  merge suspects, not the split.
- **No low-lit capture in a run that visibly flickered:** the 5 s cadence is too coarse for a one- or two-frame
  flicker, so the cadence on its own exonerates nothing - that is what `R2_CAPTURE_ON_BREAK` below is for. Its
  `deko break at frame N:` lines are the record instead: each one names the frame, the numbers that differed
  from its neighbours and where the pair of images went.

**The line built for this (2026-11-25, `R2_DEKO_FRAME_LOG=1`).** One line per presented frame, e.g.

```
deko frame 4212: 7 passes, 6219 draws replayed, 0 skipped for shaders; 3 submits, 2 splits, last cut between
passes, after pass 4 (GPFIFO entry batch); copies 6 (1420.5 KiB), XFB 1 (576.0 KiB); passes EFB:3577->EFB
HUD:812->EFB sky:402->EFB
```

It is the conversion's own account, not the bridge's: `passes` counts `begin_pass_native` calls, `draws` counts
`kd::draw` issues and `skipped` the draws `bind_pipeline` refused, `submits`/`splits` come from `submit_list` (the
frame's final submit is counted as a submit but never as a cut), `last cut` names the pass that was open when the
last cut was taken plus the call site, `copies`/`XFB` are the texture copies' bytes with destinations labelled
`XFB` counted separately, and the last field lists the frame's four biggest passes as `label:draws->target`.
Nothing in the bridge can see any of these. What each shape of a bad frame means:

- **No pass drawing into the presented texture, or a biggest pass with 0 draws:** the world's draws are being
  issued into a pass that is not the frame (a pass that was never begun, or began with the wrong attachment),
  which is suspects 1-3 and names which.
- **The world's pass there with its draws, `XFB 1` copied:** the pass ran and its result was copied out; the loss
  is after the XFB copy, i.e. in the present - and `R2_CAPTURE_PASS` images of that frame settle it.
- **`XFB 0` in a world-less frame:** the copy never happened, which the log's copy lines cannot show (they are
  recorded from the bridge, not from the replay).
- **`skipped` above 0:** a stage the mission streamed in was not compiled yet.
- **`last cut inside pass P after D of its draws`:** the source check above says this cannot happen; if it does,
  the call site in brackets names the path and the split becomes real again.

**Run of that line, 2026-11-25 (Ryujinx, 180 s of the attract demo, `R2_DEKO_FRAME_LOG=1` only).** 331 presented
frames and every one a coherent line, e.g.

```
deko frame 330: 9 passes, 2268 draws replayed, 0 skipped for shaders; 2 submits, 1 splits, last cut between
passes, after pass 9 (Aurora queue submit); copies 2 (1130.0 KiB), XFB 2 (1130.0 KiB); passes EFB 2:1131->...
```

The counts agree with what the bridge recorded for the same frames (two 640x450 display copies a frame, ~1130
KiB each), they move frame to frame as the demo does (2268 -> 2248 draws within one second), and `last cut` is
always "between passes": the split never fell inside a pass body, which is the source check above confirmed by
a running build rather than by reading. The line itself is therefore trustworthy.

What the run did not settle was *naming*: every target read `Render texture`, because that is the label Aurora
gives each render target (`extern/aurora/lib/webgpu/gpu.cpp:240`) - all of them - so the field could not say
which target a frame's world geometry went into, which was half its purpose. `Tex` now carries a creation id
(`switch/deko/aurora/dk_objects.hpp`, atomic counter in `dk_objects.cpp`, the FIFO thread creates textures) and
both fields print `label WxH#id`:

```
... passes EFB 2:1094->Render texture 640x480#1 EFB 4:1094->Render texture 640x480#1 TexCopyConv Pass:1->Copy Conv Texture 640x2#62 EFB 1:1->Render texture 640x480#1
```

Same label, size and id as the neighbouring frames means the same target; a different id means the frame drew
somewhere else, which is the comparison a flicker frame needs.

**`R2_CAPTURE_ON_BREAK=1`: the capture that cannot miss the frame.** The five-second cadence samples one frame in
about seventy at the level's 14 fps and a flicker lasts one or two, so a run of clean cadence images exonerates
nothing. This watches every presented frame instead, in `switch/deko/device.cpp`:

- Each presented frame is copied into a ring of three readbacks **by the frame's own command list**
  (`break_capture_present`, from `frame_present` after the picture is drawn). No extra submit and no CPU wait: the
  frame's usual final `submit_list(nullptr)` signals the copy's fence, so the trigger cannot change the timing it
  is measuring. The slot is judged one or two presents later, when the GPU has written it, and never blocks -
  a slot whose fence has not signalled is simply tried again next present.
- The frame's pass shape is stored with the copy - the numbers of its `deko frame` line: passes begun, draws
  replayed, draws skipped, XFB copies, bytes copied. It is compared with the **median of the last 6 good
  frames**: passes and XFB copies must be equal, draws and lit pixels within 25%, copy bytes within 50%, and
  `skipped > 0` against a median of 0. Lit pixels are counted on every other pixel in both directions (a quarter
  of the picture), so the scan does not cost the frame.
- A frame that differs is written as `logs/deko-capture/break-NNNN.ppm` **with the frame before it** as
  `prev-NNNN.ppm`, and the log line repeats the numbers and the median it broke from:

  ```
  deko break at frame 179: lit pixels 0 vs 32691; the frame: 2 passes, 7 draws, 0 skipped, XFB 1, 1125.0 KiB
  copied; capture .../break-0179.ppm + .../prev-0178.ppm
  ```

- Two guards, both added because the first run showed they were needed: eight judged frames in a row that differ
  is the **scene changing**, not a flicker, so the baseline is restarted from that frame (without it the median
  stayed on the old scene and every frame after a transition was a "break" for the rest of the run); and a dump
  is at most one per 40 frames (about 3 s), up to 24 a run, so a transition cannot use up the captures.
- Cost on the console: one GPU copy of the presented image a frame (3.7 MiB at 1280x720, a copy-engine job, no
  fence wait) and three readback blocks of the same size (11 MiB). `R2_CAPTURE_FULL=1` writes the PPMs at full
  size instead of 640 wide.
- What it cannot do: it judges the **presented** image, so it says *that* a frame lost its world, not which pass
  did it - that is the frame's own `deko frame` line, printed beside it. `R2_CAPTURE_PASS` cannot help here: the
  pass images must be taken inside the frame and a break is only known a frame or two after it.

**What running it proved (Ryujinx, 190 s, `R2_CAPTURE_ON_BREAK=1`, `build-switch/tmp/dekoprobe2`).** 355 frames,
19 `deko break` lines, no `never judged`, no fatal, no `2359-`/`2345-`/`kickoff failed`. The breaks are the demo's
real discontinuities, and each kind behaved as designed:

- frame 179, `lit pixels 0 vs 32691` (a cut to black), written as `break-0179.ppm` + `prev-0178.ppm`;
- frames 180-185 the same shape, logged and not written (inside the 40-frame gap), then `8 frames in a row
differ, so this is the scene changing - baseline reset`;
- the boot screen -> attract demo change (`passes 9 vs 2, XFB copies 2 vs 1`) reset the baseline the same way
  instead of dumping 300 pairs;
- frames 236 and 300 (`passes 11 vs 9`, the demo's two extra passes) were written once each, 64 frames apart.

So on the console a flickering frame will arrive as a `deko break` line plus its pair of images, with the numbers
that broke, without the trigger swallowing them and without it turning a level transition into noise.

**The recordings, read (2026-11-25): what the flicker frame actually is.** `RL_Flicker1.mp4` (1280x720, 30 fps,
29.7 s, and `RL_Flicker2.mp4`, the same file) was decoded to 160x90 grey frames and the presented picture read at
several flicker events. It settles the question the counters could not, and it is not what the ranking assumed:

- **In the bad frame the world's draws rasterised nothing, they did not draw black.** The starfield fills the
  whole frame there, including the lower two thirds, so the stars are visible *through* the space the trench,
  tower and hull occupied. Black geometry would have occluded them.
- **The same frame's radar proves the draw data was good.** The top-right radar is a render texture showing a
  freshly rendered 3D view of the same scene, and the HUD's text is sampled from a texture; both are sharp and
  current (FORM/GUNS/FLEE, the reticle, the radar). Matrices, vertices, textures and descriptors were all fine in
  that frame.
- **The world pass ran.** The starfield is drawn under the world and it survived (it is drawn across the whole
  target, sky included), so the pass began, its target was bound, and its undepth-tested content came out. What
  failed is the depth-tested part of that one pass - or exactly the state those draws were given.
- **It lasts 1-8 video frames per event** (runs measured over the whole clip: 1,1,2,2,7,1,1,3,1,8,2,2,1,1,1,4
  at 30 fps, so up to ~4 game frames at the level's 14 fps), and every bad frame's world brightness is the same
  ~9.0/255: a deterministic "no world" frame, not noise.

So the failing technology is **depth as the world's draws see it** - the buffer's contents, their clear, or the
compare they are given - and it is back at the top of the list on evidence, not plausibility. The earlier
argument that a broken depth clear "would leave nothing, not even the HUD" was wrong: HUD, sprites, starfield
and engine glows are drawn without a depth test, so they are exactly what survives a depth failure.

**Depth tests, 2026-11-25 (Ryujinx, attract demo).** Two switches were added and one released switch identified:

- `R2_SKIP_DEPTH_CLEAR=1` (new) skips the pass's own depth clear. It did **not** reproduce the flicker - the
  world drew normally (`build-switch/tmp/flicker/nodepth-0279.png`) - and there is a reason: this game's depth
  clear is a clear *draw* (`clear::render`, `extern/aurora/lib/gfx/recording.cpp:1019-1038`), not the pass's
  load op, so skipping the load-op clear never removed it. The load-op variant is out.
- `R2_FRAME_DEPTH_CLEAR=1` (new, candidate fix) makes the replay clear each depth target once per frame
  whatever the pass asked for, and logs `depth: frame N's first pass into X loaded depth instead of clearing
  it`. Over 165 s of the demo it fired **0 times** with the picture unchanged: the passes do clear depth there.
  On a console the same line counts the frames where one did not, which is the direct measurement of this
  theory.
- `R2_GX_OVERRIDE=zalways` forces the depth compare field of BP 0x40 to 7 (always, `gx_bridge.cpp:2748`). It has
  never been run on a flickering frame, and it is now the decisive test: if the world stays with it, the fault is
  the depth state or values the world's draws receive (the bridge's per-draw depth translation), not the pass,
  the target, the copy or the present.

**The GPU-behind family, tested (2026-11-25).** The console runs this port GPU-bound at 13.7 fps; Ryujinx presents
at 60 with a trivial GPU, so no "reuse once the GPU is done" path is ever stressed there - which is the obvious
reason a bug could exist on hardware and never in the emulator. `R2_SLOT_NO_WAIT=1` (new) makes `frame_begin`
take the frame slot **without waiting for its fence at all**, which is the console's condition by force. It fired
from frame 4 on and nothing changed: the picture stayed clean
(`build-switch/tmp/flicker/nowait-0261.png`), no fatal, no `deko break` with a lit drop. That is inconclusive
rather than negative - an emulator whose GPU never actually lags cannot produce a lag race - so the switch is a
diagnostic, not a fix. What the hardware logs do say: none of `GPU stalled`, `has not finished frame`, `giving up
on the fence` or `staging: reusing a generation` appears in `LOGS 11242026-1256`, `-1716` or `-1427`, so the GPU was
never a full slot behind on the console either and those guards never fired. The reuse-race family is therefore
lower, not dead.

**The frame line now names the picture's source (2026-11-25).** `diag_copy_bytes` takes the source image as well as
the destination, and the frame line prints the first XFB copy whole:

```
deko frame 242: 9 passes, 1270 draws replayed, 0 skipped for shaders; 2 submits, 1 splits, last cut between
passes, after pass 9 (Aurora queue submit); copies 2 (1130.0 KiB), XFB 2 (1130.0 KiB), XFB copy Render texture
640x480#1 -> XFB 640x450#7; passes EFB 2:632->Render texture 640x480#1 EFB 4:632->Render texture 640x480#1 ...
```

(Ryujinx, `build-switch/tmp/copysrc`: the level's two half-passes draw into `#1` and the XFB copy reads `#1`, so
in a good frame the world's target and the picture's source are the same texture - which is what makes the one
comparison below meaningful.)

With ids in all three places, one flicker frame answers the remaining theories in one line, no rebuild:

- **world pass target == XFB copy source, draws normal** - the world's draws went into the picture's own target
  and the frame still had no world: depth as those draws see it, i.e. the `R2_GX_OVERRIDE=zalways` test.
- **world pass target != copy source** - the frame's geometry was drawn somewhere the picture never reads: a
  target-binding/translation fault, not depth.
- **`XFB 0`** - the picture was never copied out of the frame.
- **draws collapsed against the neighbours** - the bridge never issued them (upstream of deko3d).

**The depth of a broken frame (`R2_DEPTH_SHOT=1`, 2026-11-25).** The recordings leave one question open - the
world's depth-tested draws produced no pixels while everything undepth-tested in the same frame was correct - and
no counter can answer it: *did those draws write depth at all?* With this switch the frame's biggest pass's depth
target is copied into a ring of three by the frame's own command list (no extra submit, ~1.1 MiB at 640x450
Z24S8), and when the break trigger writes a pair it writes that frame's depth beside it:
`logs/deko-capture/depth-NNNN.pgm`, 8-bit, **0 = near, 255 = far**.

It was validated in Ryujinx at frame 296's break: `depth-0296.pgm` shows the scene's own depth - the figures near
(dark), the sky at far (white), the floor between - so the ring holds the frame's depth and the trigger dumps the
right one. What each shape of it means for a flicker frame:

- **the world's shapes in the depth, no world in the colour:** the draws rasterised and the depth they compared
  against is wrong -> the `R2_GX_OVERRIDE=zalways` test, and then the comparison that rejects them.
- **depth all far (255):** nothing wrote depth. The world's draws never rasterised - a pass that never began, a
  target or scissor they miss, or a shader that outputs nothing - and the frame line's per-pass `draws` says which.
- **depth all near (0):** the buffer was cleared to 0, i.e. a wrong clear value rather than a missing clear.
- **the previous frame's shapes:** the depth was never cleared - `R2_FRAME_DEPTH_CLEAR=1` is the answer, and its
  own log line says how often that happened.
- **depth and colour both right where the world is wrong only in patches:** colour write or blend, not depth.

**The pass field now counts depth-test and blend enables (2026-11-25).** Each of the frame's four biggest passes
reads `label:draws->target[dN bM]`: how many of those draws had the depth test on and how many had blending on.
A flicker frame whose world pass reads `d0` lost its depth test; one that reads `b0` lost blending. Ryujinx's
level pass reads `EFB 2:626->Render texture 640x480#1[d626 b626]` - every world draw depth-tested and blended,
which is the game's own state. **Limit, stated:** it counts *enables*, not modes, so the subtract/zero-alpha blend
that paints nothing (the class the `R2_GX_OVERRIDE=noblend` arm was written for) still reads `b626`; the depth PGM
below is what separates that from a depth fault, because a fragment killed by blending still *writes depth*.

**The depth clear fix, broadened to every pass (2026-11-25).** `R2_FRAME_DEPTH_CLEAR=all` clears at **every** pass
into a depth target instead of once per frame. The frame draws its world twice (`EFB 2` and `EFB 4`, 632 draws each,
into the same depth image), so a frame whose *second* pass lost its clear is uncovered by the once-per-frame arm and
covered by this one - and the game re-clears for both passes anyway, which is why mirroring it is safe. Verified on a
good frame: `lit` 204683 / 202972 / 207442 (the same values as every baseline run of this scene), 0 fatal lines,
59.3 fps, the arm firing once (`build-switch/tmp/clearall`). `boot.cfg` now ships `=all`.

**Checked and ruled out: a pass inheriting a scissor (2026-11-25).** The shape to explain would be the world's
geometry clipped to nothing while other passes look right, which is what a stale scissor rect does - and the first
draw of a pass is exactly where such a leak would show. It cannot happen here: `begin_pass_native` calls
`swdk::set_view(0, 0, w, h)`, and `set_view` (`device.cpp`) sets the **viewport and the scissor** to the target's
full size on every pass. Aurora caches its own scissor and viewport and skips repeats, but our value at pass begin
is the whole target, so the only way the two disagree is a *wider* rect, never a narrower one: overdraw at worst,
never invisible geometry. The per-draw `dkCmdBufSetScissors` in `direct.cpp` and the one `device.cpp` uses for its
own 2D draws are both inside the same command stream, so no third party moves the scissor mid-pass.

**`R2_GX_OVERRIDE=zalways` costs nothing on a good frame (2026-11-25).** With this port's reverse depth (GreaterEqual
against a clear of 0) every draw already passes the depth test, so forcing Always should change nothing - and it does
not: 95 s in Ryujinx, `f238 lit=203041/288000`, the presented swapchain `lit=655770/921600 nonopaque=0
range=[0..10,0..10,0..10,255..255]`, 0 fatal lines. That makes the console arm clean: **if the flicker disappears
with `zalways`, the fault is depth rejection; if it survives, depth is exonerated** - the same frame, the same
scene, one bit of state.

**Final pack, checked for length and lifespan (2026-11-25).** The exact `boot.cfg` for the console, 150 s: 60 fps,
**0** fatal/2345/2359/kickoff lines, captures `lit` 211597 / 212793 / 213218 / 214416 / 214681, the frame line 744
characters (well inside the log's line limit, so nothing is cut) and the `all` field present in all 313 frame lines.

**A good frame's pass map, now in every frame line (2026-11-25).** The line's `passes ...` field lists only the four
biggest; a new `all` field lists **every** pass of the frame in order with its draw count, including the ones that
drew nothing. Ryujinx's level frame reads exactly:

```
all EFB 0:0 TexCopyConv Pass:1 EFB 1:1 TexCopyConv Pass:1 EFB 2:632 EFB 3:1 TexCopyConv Pass:1 EFB 4:632 EFB 5:1
```

So the frame is: an empty `EFB 0`, a copy conversion, a one-draw `EFB 1` (the const-multiply quad), another copy
conversion, the world's **632 draws** in `EFB 2`, a one-draw `EFB 3`, a third copy conversion, the same **632 draws
again** in `EFB 4` (the picture is drawn twice per frame), and a one-draw `EFB 5`. That map is the frame's
skeleton, and it makes a flicker log self-classifying:

- **`EFB 2/4` still at ~632 with the world missing** -> the draws ran and were rejected or written wrong: read the
depth PGM (`depth-NNNN.pgm`) and the pass's `[d.. z.. b..]` fields.
- **`EFB 2/4` at 0 or absent** -> the draws never reached the GPU: the bridge or Aurora's merge dropped them, and
  nothing about depth applies.
- **`EFB 2/4` split across two different targets** (the `/z` and target ids differ) -> a binding problem, and the
  ids in the line name it.

**The console's GPU-behind condition, reached in the emulator at last (2026-11-25).** Every timing theory here was
unfalsifiable because Ryujinx's GPU is idle (60 fps against the console's 13.7). `R2_DRAW_REPEAT=<n>` issues each
replay draw n times, which moves the load to the GPU without changing the picture (same draw, same state,
overwritten):

- **20x: 59.7 fps** - the host GPU still has that much headroom, no condition created.
- **300x: 33-36 fps**, and the guard fires for the first time in any run here:
  `staging: reusing a generation whose fence has not signalled (1 times); the GPU is not keeping up with this
  buffer's writes`. That is the console's condition, deliberately.

Under that pressure, with the break trigger and depth shots armed, **no world-less frame appeared**: 19 breaks, every
one of them the demo's own `2 passes, 6-7 draws` small frames (`lit pixels 30356 vs 14182`, i.e. a scene change, not
a lost world), and no `GPU stalled` / `has not finished frame` / `giving up on the fence`. The emulator then ended
by itself after ~64 s (`exit code 3762504530`, no fatal from the port; the same exit code appeared with
`R2_GX_OVERRIDE=noblend`). So: GPU-behind *alone* does not remove the world, and the port's own reuse guard - which
does fire when the GPU genuinely lags - is not what does it. The timing family is now testable here; its first test
came back negative.

**The demo alternates shot sizes, and that is not a fault (2026-11-25).** With no diagnostic switch on at all, the
attract demo alternates frames of `9 passes, 1270 draws` with frames of `2 passes, 1 draw`; the break trigger sees
every switch as a break and reports `passes 9 vs 2` for them. This was mistaken for damage from a depth poison in
an earlier run of the same day - the poison had nothing to do with it. **Any reader of a console log should ignore
`passes 9 vs 2` / `2 passes, 1 draws`**: it is this demo's own alternation, not a lost frame.

**The shipped pack, run end to end (2026-11-25).** `boot.cfg` exactly as it will be flashed, 160 s in Ryujinx:
60.0 fps, **0** fatal/2345/2359 lines, captures `lit` 199978 / 207382 / 209336 / 209311 / 209482 / 209568 / 210099 /
210943 - the healthy range for this scene - the depth re-clear fired 6 times, and the 18 break lines are the demo's
alternation and scene changes (`build-switch/tmp/packcheck`). So the diagnostic pack is inert on a good scene: it
costs a little bandwidth and changes nothing that shows.

**`deko frame N` is not the game's frame counter (2026-11-25).** At 35.7 s into a run the port logs `deko frame 232`
while the game's own summary reports `1440 frames, 60.0 fps`. `R.frame` counts deko *presents*, and this scene
presents far fewer times than the game renders. Two consequences, both correcting earlier work here:

- comparing `frame-NNNN` captures **across runs by number** was never sound - run A's 232 and run B's 232 are
different moments of the demo unless both runs presented at the same rate, which they do not (the runs above
range from 6.6 to 60 presents/s).
- `R2_CAPTURE_BURST=<frame>` (added now, captures nine consecutive presents from that `R.frame`) must be aimed by
  watching a run's own `deko frame` lines, not by assuming seconds x 60.

With that in hand, the only trustworthy comparison is *inside one run, on adjacent presents* - which is what the
break trigger does and what a burst with a poison stride is for. Nothing below rests on a cross-run frame number.

**The flicker reproduced locally - and the repair claim retracted (2026-11-25, later the same day).** Read the
retraction first: the first version of this block said the repair restored a broken frame to `lit=204683`
"exactly". That comparison put a *repaired* frame 232 against a *baseline* frame 235 - different frames of a demo
that advances - so it proved nothing. A controlled test followed: `R2_DEPTH_POISON_AFTER_CLEAR=2` poisons every
other frame *inside one run*, and the break trigger then dumps a pair from the same scene seconds apart. At frames
177 and 176 of that run the trigger reports `lit pixels 0 vs 32753` - the poisoned frame lost **everything**, HUD
included, not just the world. With `R2_DEPTH_CLEAR_AFTER_CLEAR=1` added, the same A/B (two runs, captures at the
same scheduled frames 234 and 236) came out identical: `lit=956` and `0` in both. **So the repair switch is
unproven**: it has not been shown to restore a frame, and `boot.cfg` now carries it as a candidate only, labelled
as such. What *is* established is the half that matters for the diagnosis:

- `R2_DEPTH_POISON_AFTER_CLEAR=1` writes far depth (1.0) at the first draw **after** the game's own depth clear
  quad. The captured frame drops from `lit=204683` to **`lit=71847`** (`build-switch/tmp/poison2`, image
  `build-switch/tmp/flicker/poison-0235.png`): the scene keeps its structure and loses the bulk of its
  depth-tested geometry - the flicker's characteristic, on demand. Poisoning at pass begin instead (the first
  attempt) changed nothing at all, because the clear quad is drawn *inside* the pass and overwrote it.
- `R2_DEPTH_CLEAR_AFTER_CLEAR=1` clears the same buffer at that same point instead of trusting the quad. It is
  **unproven**: see the retraction above. What it does do is safe by construction - it only fires when the clear
  quad covered the whole picture (`scissor 0,16 640x450` against a `640x480` viewport, i.e. the game's 640x450
  picture inside the 480-row EFB, with the 30-row overscan allowed for) and only re-writes the value the game's
  own clear uses (0). It is in `boot.cfg` as a candidate, beside `R2_FRAME_DEPTH_CLEAR`, and its log line says
  whether it fired and why it declined.

So the established fact is one-directional: a depth buffer that is not empty at the moment the world's draws run
removes what the depth test rejects, up to and including a whole frame. Whether that is what the console does, and
whether re-clearing repairs it, is what the console boot with the depth PGM and the `depth:` lines has to say.

**Depth: what the emulator can and cannot decide (2026-11-25).** With counts, the level pass reads `z8x2,7x630`:
630 of its 632 draws are depth-tested with GreaterEqual and only two use Always, so depth really does gate the
world. Two attempts to break it locally changed nothing:

- `R2_SKIP_CLEAR_DRAW=1` drops the game's own depth clear quad. Its log names the target it dropped -
  `Render texture 640x480#1`, which is the world pass's own colour surface - so the experiment was aimed
  correctly, and the captured frame is byte-identical to the baseline (`lit=204683/288000` at the same frame).
- `R2_DEPTH_POISON=1` forces the depth buffer to 1.0 right after the clear, every frame: the same frame is again
  byte-identical.

The reason is the attract demo itself: its camera barely moves, so a buffer left holding the previous frame's depth
still accepts this frame's geometry. **The emulator cannot discriminate the stale-depth case at all** - which is
why the flicker never reproduces here, and why the console boot (a flying camera, plus the depth PGM) is what
decides it. Both switches are diagnostic only and are not in `boot.cfg`.

**The depth convention, measured - and a landmine found and fixed (2026-11-25).** The pass field now carries the
depth compare functions (`z`), and the pass-level clear its value:

```
[dk] depth: clear value 0.000 into Depth texture (1 distinct so far; reverse depth, 0 = nearest)
passes EFB 2:632->Render texture 640x480#1[d632 z8,7 b71841x1,71081x397,71146x234]
```

So this port's depth is **reversed**: the world's 632 draws compare `z8` (Always) and `z7` (GreaterEqual), no draw
in the level pass uses Less, and the game clears to **0.000**, which is the *nearest* plane. That is the whole
convention, and it decides the flicker's mechanism: with GreaterEqual against a clear of 0 every draw passes, and a
buffer that still held the **previous** frame's nearer geometry would reject exactly the parts of this frame that
are farther away - world gone, undepth-tested stars/sprites/HUD/glows and the radar's own target untouched, which
is the recorded picture.

**The landmine:** `R2_FRAME_DEPTH_CLEAR` (added earlier the same day) cleared the frame's depth to **1.0**. Under
reverse depth 1.0 is the *far* plane, so if that branch had ever fired on the console it would have put the whole
buffer out of reach and blanked the world - the flicker, made permanent, by the flag meant to fix it. It never
fired in Ryujinx (the game always clears), which is exactly why it went unnoticed. Corrected to **0.0** on
2026-11-25, and the new `R2_DEPTH_CLEAR_ZERO=1` forces a pass-level clear to 0 the same way. Verified with both on:
the captures are identical to the baseline (`lit` 204683 / 202972 / 207442 / 209292 at the same frames), the frame
clear fired 0 times, 0 fatal lines (`build-switch/tmp/clearzero`). So forcing 0 is a no-op on a good scene, as it
should be.

This also corrects the reading of the earlier `R2_SKIP_DEPTH_CLEAR` test: skipping a clear under reverse depth
leaves *nearer* stored values, which reject this frame's geometry, so the switch does model the flicker - but it
only skipped the pass-level clear, and the game's own clears mostly arrive as clear draws, so the emulator run did
not exercise the path a console would. The switch stays for that use; `R2_DEPTH_CLEAR_ZERO` is the repair to test
alongside it.

**The blend modes of the level pass, measured (2026-11-25).** The `b` field now lists the distinct blend modes of the
pass's draws with their counts, each as `mask|op|src|dst` in hex (tag = `(mask << 16) | (op << 12) | (src << 6) |
dst`, mask = target 0's colour write mask, the rest deko3d's `DkBlendState` factor/op values). Ryujinx's level
pass reads:

```
passes EFB 2:632->Render texture 640x480#1[d632 b71841x1,71081x397,71146x234]
```

Decoded: `71841` = mask 7 (RGB), Add, src=ConstColor, dst=Zero -> `dst = src * constColour`, **1 draw**; `71081` =
mask 7, Add, One/Zero -> a plain opaque write, **397 draws**; `71146` = mask 7, Add, SrcAlpha/InvSrcAlpha -> an
alpha blend, **234 draws**. So the world's geometry is 397 opaque writes and 234 alpha-blended draws, and the
earlier reading "every world draw is blended" was wrong: blending is on for the pass's effects, not for its bulk.
This is the good-frame signature: a flicker frame whose `71081x397` collapsed, or whose opaque draws turned into
`71146`, is a leaked blend state; one with the same counts is not, and the depth PGM then decides.

Two things this named, both to check on the next boot rather than guess at here:

- the **single `71841` draw** is a full-screen quad in a const-multiply mode, and the only constant our wrapper ever
  sets is `{0, 0, 0, dstAlpha/255}` (`patches/aurora-switch-encoder-state-cache.patch`, `SetBlendConstant`). With
  RGB 0 that quad computes `dst = 0`. It is correct today, so it must be scissored or depth-failed - but if that
  one draw ever lands covering the screen, the frame's world goes black while later passes (HUD) stay.
- the blend constant is **cached** by that same patch (`g_haveBlendConstant`, keyed on `dstAlpha`) and the cache is
  reset only when a WebGPU pass begins. Our replay re-records state per native pass, so a value the cache believes
  is already set after a pass boundary is exactly the class of bug that would change a frame's colours only.

**`R2_GX_OVERRIDE=noblend` blanks the whole picture (2026-11-25).** Clearing BP 0x41 bit 0 for every draw makes the
frame entirely black in Ryujinx: the EFB snapshot reads `lit=0/288000` and the swapchain `lit=0/921600`,
`range=[0..0,0..0,0..0,255..255]`, where the same frames without the flag read `lit=34113` (f179) to `lit=209396`
(f251). The emulator then ended by itself (`exit code 3762504530`) with no fatal line from the port, so this stays an
observation, not a conclusion: blending is load-bearing for the picture, so a wrong blend state *can* blank the
frame - and a fragment killed by blending still writes depth, which is why the depth PGM is what separates the two
families on hardware. The flag is diagnostic only and was not left on.

**Attempted reproduction, failed (2026-11-25).** The console is GPU-bound (13.7 fps) where Ryujinx is CPU-bound
(59.9 fps), which is the standing reason a timing bug could exist only on hardware. Ryujinx's internal resolution
was raised to 4x (`res_scale` 4 in `portable/Config.json`) to make the emulator's GPU the bottleneck: the game
still ran at 59.9-60.0 fps and no flicker frame appeared (`build-switch/tmp/gpubound`). The emulator's GPU has
enough headroom at 4x, so that route to the console's condition does not work and the config was restored to 1.

**Next boot** (one flash, all of the above): `R2_STATS=1 R2_DEKO_FRAME_LOG=1 R2_CAPTURE_ON_BREAK=1`,
`R2_FRAME_DEPTH_CLEAR=1` and `R2_DEPTH_SHOT=1`, then fly the mission with the flicker. A break now arrives as
`break-NNNN.ppm` + `prev-NNNN.ppm` + `depth-NNNN.pgm`. The `depth: frame N's first pass into ...` line says
whether a frame lost its clear; a `deko break` pair shows the frame itself. If it still flickers, the same boot
plus `R2_GX_OVERRIDE=zalways` says whether depth is the mechanism at all - the world visible with it means the
per-draw depth state is where the fix goes, and the switch then stays on only long enough to find which register
write is lost.

Built and in `build-switch/sdcard/switch/RL-NX/`. Everything above was run in Ryujinx, which draws this level
correctly: it proves the instruments and kills the load-op theory, and it cannot prove the flicker is caught -
that needs the console, the prepared boot.cfg and the mission.

## 9.28 Run 1 on the console (11252026-0843): the trigger spent itself on the attract mode (2026-11-25)

Boot was the twelve-line pack from 9.27 (`R2_CAPTURE_ON_BREAK`, `R2_DEPTH_SHOT`, `R2_FRAME_DEPTH_CLEAR=all`,
`R2_DEPTH_CLEAR_AFTER_CLEAR=1`, `COS_DK_CAPTURE`, `R2_FRAME_LOG`, `R2_DEKO_FRAME_LOG`, `R2_COPY_LOG`). 7,565
presented frames, 9.2 MB of log, 24 break pairs and 24 depth PGMs in `logs/deko-capture/`. The user flew the
flickering mission and the world still vanished every few seconds - more often when not firing.

**Every one of the 24 dumps is the attract mode.** Frames 15-1740: the title and the demo, 632-draw passes at
`640x304`/`640x450`, 4-24 draws a frame at `640x304`, the "Rogue Squadron II" plate and the Star Destroyer.
Not one frame of the mission, which is frames ~5390-7507: one pass of 1,100-1,900 draws, 10.8-14.3 presents/s,
until `FrntEnd_missionFailed_level1_4` at 317 s.

**Why the trigger never fired in the level.** Of the 544 break verdicts written for frames with a >=1000-draw
pass, exactly four mention lit pixels and all four are frames whose lit count was 0 (`lit 0 vs 230400` at
5889-5891, the slides before the level loads). Everything else is structural (`passes 8 vs 10`, `draws n vs
m`). `break_lit` counts *non-zero* pixels, and the trench is lit edge to edge: a frame that loses the world
there still reads ~230400 of 230400. The metric can only see the loss in space, where the background is black
- which is exactly where dumps 419 and 5889 came from. That is the defect in run 1's instrument, not in the
console.

**Measured in the same run, open, and not the flicker's cause.** One blend family (`71146`) drops ~380 of its
~748 draws every frame from f5908 to f6236 and returns in a single frame:
`[d1461 z8x67,7x1391 b71841x1,71081x596,71146x748]` becomes `[d1112 ... 71146x362]` with `skipped 379`, for
329 frames (~23 s at 14 fps). Those draws are skipped for shaders; the same window holds the run's worst
compile, `longest 22014 ms` where the level's other stages averaged 199-303 ms (`shaders: 192 compiled by uam`
is the last such line, printed at f6238). So one stage sat in uam's queue for 22 s while every draw that
needed it was dropped. It is a real hole in the picture and it is a *pop*, not a flicker: it starts once and
ends once. Left open, unfixed, and recorded here rather than chased first.

**The instrument that can see a lit picture lose the world (built, Ryujinx-checked).**
`switch/deko/device.cpp`: every judged frame is reduced to a 64x36 luma grid (four samples a cell, Rec.601) and
compared with the frame before it; the mean absolute difference is compared with the same number for the frame
before *that*. A jump of `R2_FLICKER_THRESH` (default 20, of 255) that is also at least 2.5x the previous
frame's change is a flicker edge - entering it or leaving it - and is written as a matched pair
`flk-before-NNNN.ppm` + `flk-after-NNNN.ppm`, each with its own `depth-NNNN.pgm` when `R2_DEPTH_SHOT=1`. The
factor is what keeps a steady fast pan from triggering; the floor keeps a still frame from doing it. Around it:

| switch | what it does |
|---|---|
| `R2_CAPTURE_FLICKER=1` | arms the one-frame change trigger (the pair above) |
| `R2_FLICKER_LOG=1` | one `picture: frame N changed X/255 (the frame before Y/255), top pass T draws, lit L` line per judged frame - the flicker is in the log as numbers even when no dump fires |
| `R2_FLICKER_THRESH=n` | the floor, of 255 (default 20) |
| `R2_CAPTURE_MIN_DRAWS=n` | a frame whose biggest pass drew fewer is not judged at all: the attract mode's biggest pass is 632-704 draws and the trench's is 1,100-1,900, so 1000 spends the budget on the level only |
| `R2_CAPTURE_GAP=n` | frames between dumps (default 40); 0 keeps every frame of a flicker run |
| `R2_CAPTURE_DUMPS=n` | the dump budget (default 24) |

Ryujinx, 75 s at the shipped floor: 233 `picture:` lines, largest change 9/255, nothing false-triggered, no
fatal (the emulator presents many identical frames, so this only shows the metric runs, not how the console
will read). Forced with `R2_FLICKER_THRESH=0`: `deko flicker at frame 2 ... flk-before-0001.ppm +
flk-after-0002.ppm + depth-0001.pgm + depth-0002.pgm`, 691 KB a PPM and 307 KB a PGM, three pairs back to back
at `R2_CAPTURE_DUMPS=3`, no fatal - the pair, the depth beside it, and the no-gap path all write.

**Run 2's boot** is in `build-switch/sdcard/switch/RL-NX/boot.cfg` (NRO built 09:02): `R2_STATS=1`,
`R2_DEKO_FRAME_LOG=1`, `R2_CAPTURE_ON_BREAK=1`, `R2_CAPTURE_FLICKER=1`, `R2_FLICKER_LOG=1`,
`R2_CAPTURE_MIN_DRAWS=1000`, `R2_CAPTURE_GAP=0`, `R2_CAPTURE_DUMPS=30`, `R2_DEPTH_SHOT=1`, and the same
`R2_FRAME_DEPTH_CLEAR=all` + `R2_DEPTH_CLEAR_AFTER_CLEAR=1` the user flies. No `COS_DK_CAPTURE`: the cadence
cost a submit and filled the log with pass dumps of the attract mode. What run 2 has to answer, in order:
(1) does a `deko flicker` pair show the world gone in `flk-after` and present in `flk-before`; (2) does that
frame's `depth-NNNN.pgm` hold the world's depth (draws rasterised, so the colour or blend is wrong) or only
far/near (the draws never reached the target); (3) does its `deko frame` line still list the 632/1,354-draw
pass with `[d.. z7x.. b..]` unchanged (draws ran, pixels were rejected).

## 9.29 Run 2 on the console (11252026-0924): the world leaves a bare clear in depth, not a rejected world (2026-11-25)

Boot was the eleven-line pack from 9.28 (NRO 09:02): `R2_CAPTURE_FLICKER=1`, `R2_FLICKER_LOG=1`,
`R2_CAPTURE_MIN_DRAWS=1000`, `R2_CAPTURE_GAP=0`, `R2_CAPTURE_DUMPS=30`, `R2_DEPTH_SHOT=1`, the frame log and
the same `R2_FRAME_DEPTH_CLEAR=all` + `R2_DEPTH_CLEAR_AFTER_CLEAR=1` the user flies. 10,139 presented frames,
8.2 MB of log, 91 files in `logs/deko-capture/`: three matched flicker pairs (5669, 5700, 5712), 27 break
frames, and a depth PGM beside almost all of them. The user flew the mission.

**The trigger now sees the level.** `deko flicker at frame 5669: the picture changed 36/255 where the frame
before it changed 1/255 (floor 20), 1006 draws in the top pass, lit 230400`. Same at 5700 (41 vs 3, 1036
draws) and 5712 (38 vs 3, 1047 draws), then 25 more edges after the dump budget ran out (5728, 5741, 5766,
5787, 5805, 5899, 5905 ... 6118). The lit count is 230400/230400 in every one of them - 9.28's proof that the
lit metric is blind here holds - and the luma grid catches the same event the video shows.

**In this run the world is not flickering 1-8 frames; it is gone for stretches.** Mean luma of the 640x360
captures: `10.4-10.6` for a frame with only the starfield, `48.2-51.8` for a frame with the trench, `27.3` for
the one half-there frame (5696). Of the 48 frames the run dumped between 5668 and 5717, exactly three have the
world (5696, 5700, 5712). At 14 fps that is ~3.5 s of flying with the world switched off and three single-frame
returns - which is why it reads as "it disappears every few seconds" rather than as a flicker.

**The pair, and what the depth says.** `flk-before-5699` is stars only (mean 10.55, 0.8% bright); `flk-after-5700`
is the full trench (mean 51.81, 44.1% bright). Their depth images:

| image | values | content |
|---|---|---|
| `depth-5699.pgm` (world gone) | 2 | `0x80` over rows 0-263 (168,960 px), `0x00` over rows 264-479 (138,240 px) - a clear and nothing else |
| `depth-5700.pgm` (world present) | 256 | the trench, the tower, the horizon - the whole world |
| `depth-5670.pgm` (world gone) | 32 | a smooth ground-plane gradient (67 at the top to 40 at the bottom) |
| `depth-5712.pgm` (world present) | 256 | same as 5700 |

`0x80` is the top byte of a Z24S8 value, i.e. depth **0.502** - the middle of the range, in a port whose depth
clear is 0.000 and whose world compares `GreaterEqual`. Rows 264-479 being zero is real content, not a clipped
copy: in Ryujinx the same dump fills all 480 rows (`depth 1 values mean 0, 0 holds 100% of 640x480`).

So the frames whose world is missing do **not** hold a world that a depth test rejected: a rejected world leaves
the cleared buffer (one value, 0) and the depth test never paints. They hold a constant 0.5 over exactly the
world's rect, where the next frame holds the whole world - the buffer was written by something, and it is not
the clear (whose value is logged and is 0.000 all run) and not the world's own draws.

**Nothing in the frame line separates the two frames.** 5699 and 5700 are both 6 passes, 1034 vs 1036 draws, 0
skipped for shaders, 2 submits, 1 split between passes, the same targets (`Render texture 640x480#1/z Depth
texture 640x480#3`) and the same family split (`z8x1,7x103x b71841x1,71081x718,71146x291`). Copies are 760 KiB
in both. So this is not a skipped draw, a dropped copy, a changed pass layout or a state the line records.

**One frame is a second mode.** 5670 (a break frame) has a world-less picture and a *ground-plane* depth
gradient: on that frame the geometry rasterised into depth and only the colour is missing. The two modes - no
depth and no colour (5699), depth but no colour (5670) - share only the colour side, so the colour loss is the
common one.

**Built for run 3 (NRO 09:42, emulator-checked), `R2_CLEAR_LOG=1` plus an extended `picture:` line:**

| switch | what it adds |
|---|---|
| `R2_CLEAR_LOG=1` | one line per frame for the game's own depth-clear quad: `clear: frame N depth quad writes z 0.0000 (viewport z 0.0000..0.0000), 0,16 640x450 of 640x480, pass cleared depth 1`. The pass-level clear value is only logged once per distinct value and is always 0.000, so it cannot be what leaves 0.5; the quad writes the *viewport's* z range (`clear::render` sets min == max == `clear_depth_value()`), and that number was in no log before |
| `R2_FLICKER_LOG=1` (extended) | the per-frame line now ends `, depth U values mean M, K holds P% of 640x480` - the frame's depth signature from the ring that was already being copied every frame. Every frame of a run, not only the 30 dumps, then says whether a world-less frame had a world in its depth (colour lost) or a bare clear (nothing reached the target) |

Ryujinx, 110 s: 245 `picture:` and 245 `clear:` lines, `depth 1 values mean 0, 0 holds 100% of 640x480` in the
menus, `clear: frame 2 ... 0,16 640x450 of 640x480`, no fatal. The flicker pair's log line also had its buffer
raised from 200 to 400 bytes: two PPM paths and two PGM paths did not fit, which is why run 2's pairs named only
the first depth image.

**Run 3's boot** is `build-switch/sdcard/switch/RL-NX/boot.cfg` (30 lines, md5 `ec53f88685620c4cb718c733069204aa`,
repo-root `boot.cfg` identical): run 2's pack plus `R2_CLEAR_LOG=1`. What it answers: on a frame whose world is
gone, does the clear quad write 0.5 (the fault is in what the port asks the GPU to clear to) or 0.0 (the 0.5
came from a draw later in the pass, and the next question is which draw); and does the per-frame depth signature
mark every world-less frame with a bare clear, or do some keep a world in depth like 5670 does.

**Still open.** What writes 0.502 over the world's rect on world-less frames; why the colour is lost even when
the depth is not (5670); the 9.28 22 s uam compile stall (`71146`, a pop rather than a flicker).

## 9.30 Run 3 on the console (11252026-1001): the constant is 1.0, not 0.502 - a draw writes the far plane, not a clear (2026-11-25)

**The log is from the 09:02 build.** `log.txt`'s header says `build Nov 25 2026 09:02:18`, its `opts` list has no
`R2_CLEAR_LOG`, and it has zero `clear: frame` lines, so run 3 ran the NRO from before the instrumentation (the
09:42 one is on the card now, 64,041,814 bytes, with the 30-line `boot.cfg`, md5 `ec53f88685620c4cb718c733069204aa`).
Nothing below depends on the missing lines: the dumps and the frame lines settle the question on their own.

**What the run contains.** One boot, 10,654 presents / 569 s, `R2_FRAME_DEPTH_CLEAR=all` + the re-clear repair
active (18,600 and 40,200 repairs logged), 553 flicker edges, 8 PPM pairs and 64 PGMs, and - unlike run 2 - the
flicker **repeats**: 20-56/255 jumps from 0-19/255 frames across 6482…10588, in long runs (6472-6481 is ten
world-less frames, then 6482 is clean).

**CORRECTION (9.31): this section's "1.0" is wrong - the value is 0.502.** Run 4's signature now prints the
format it decoded with (`depth(z24)` vs `depth(f32)`), and the EFB's depth binds as Z24: `0.502` there is the
byte `0x80` read as a Z24 top byte, exactly as 9.29 said. The float-target argument below does not hold: a float
target holding that byte would print 1.000 and does not. Read the rest of this section with 0.502 substituted for
1.0 and the far/near framing dropped; 9.31 has the measured numbers.

**What this section got right:** the buffer is one constant over part of the picture and `0` elsewhere, and no
clear in the run produces it.

**The value in the depth dumps was misread, and that is the run's finding.** The EFB's depth target is
`Depth32Float`: `extern/aurora/lib/webgpu/gpu.cpp` sets `.depthFormat = Depth32Float`, and
`switch/deko/aurora/dk_objects.cpp` maps it to `DkImageFormat_ZF32`. `write_pgm`/`depth_signature` were taking
byte 2 of each 4-byte pixel as "the top byte of a 24-bit Z24S8 value", and byte 2 of `1.0f` (`0x3F800000`,
little-endian `00 00 80 3F`) is `0x80`. So 9.29's "one constant, 0x80 (= 0.502), the middle of the depth range" is
**1.0** - the far end. Every world-less frame in this run reads the same: depth `128` over rows 16-465 (93.8% of
640x480, i.e. the whole 640x450 picture), `0` over rows 0-15 and 466-479 (the overscan), nothing else. World
frames hold 256 values. The picture of a 1.0 buffer is exactly what the recordings show: with the port's reversed
depth and `GreaterEqual`, a buffer at the far plane rejects **every** depth-tested draw, while the draws that do
not test depth (stars, sprites, HUD, glows) still paint - which is why `lit 230363` and the top-pass draw count
(1375 vs 1375, 6481 vs 6482) never move between a world-less and a world frame.

**It is not a clear, and the pass clear is not a candidate.**
- The pass-level clear value has exactly one distinct value in the whole run: `depth: clear value 0.000`.
- `R2_FRAME_DEPTH_CLEAR=all` clears 0.0 at the frame's first pass into the depth target.
- `R2_DEPTH_CLEAR_AFTER_CLEAR` fires on the game's own clear quad and logs
  `re-cleared to 0.0 after the clear quad (... scissor 0,16 640x450, viewport 640x480, pass cleared depth 1)` -
  deko3d's clear is whole-image, so after it **every** pixel is 0.0.
- Yet the biggest pass ends with 1.0 over rows 16-465 and **0.0 over rows 0-15 and 466-479**, the rows a
  whole-image clear cannot leave behind.

So the 1.0 is written by a draw, scissored to the picture, covering the whole viewport, *after* the clear quad's
repair and before (or as part of) the world's own draws. Passes and targets are identical across the flicker
(`EFB 1:1376 -> Render texture 640x480#1 / z Depth texture 640x480#3 ... `, then `EFB 0:7`, `TexCopyConv`,
`EFB 2:1`), and so are the pipeline families (`b71841x1, 71081x604, 71146x738` on both 6481 and 6482): the same
draws run in both frames, so the difference is in **uniform data**, not in which pipelines were built.

**Instrumented for run 4.** The depth readback now decodes floats (`depth_value()` in `switch/deko/device.cpp`,
format passed from the call site), so the `picture:` line reads `depth 0.000..1.000, 1.000 holds 94% of 640x480`
instead of a byte of a float, and the PGMs are 0 = 0.0 / 255 = 1.0. Two probes name the writer:
`R2_ZFREEZE_LOG=1` logs every zfreeze draw's plane and the depth it gives at the viewport's centre and edges -
zfreeze is the only place in the pipeline that writes **one constant depth across a quad**
(`clamp(plane(x, y), 0, 1)` in `shader.cpp`/`uber.cpp`), and a plane that saturates at the top writes exactly
this; and `R2_SKIP_DRAW_RANGE=a:b` drops draws a..b of every pass, so a few launches bisect the draw if zfreeze
is not in use. Run 4's boot is the 33-line `boot.cfg` (md5 `1a45bc081190c248b8947c1f14e9bfbd`, repo root and
card identical) - run 3's pack plus those two.

**Build and emulator check (NRO 10:18, 64,045,910 bytes).** Ryujinx, 120 s with run 4's flags: 254 presents, no
crash or fatal, `clear: frame N depth quad writes z 0.0000 (viewport z 0.0000..0.0000), 0,16 640x450 of 640x480`
on every frame. A second run with `R2_CAPTURE_MIN_DRAWS=0` (the menu's passes are under 1000, so the first run
judged nothing) printed the new signature unchanged in shape - `depth 0.000..0.000, 0.000 holds 100% of 640x480,
1/256 values` - and its PGMs decode to all-zero, which is right for a cleared buffer. A third run with
`R2_SKIP_DRAW_RANGE=0:60` booted, drew and ran audio: the skip path is safe. **The zfreeze lines were not
exercised** (the emulator's menu logged `0` zfreeze draws), so whether the probe fires on the console is unproven
until run 4.

**Run 4 answers:** is zfreeze in use at all, and does its plane saturate at the centre of the viewport on a
world-less frame; if not, which draw index writes the 1.0 (`R2_SKIP_DRAW_RANGE` bisection against the per-frame
depth signature, which is now in float).

**Still open.** 5670's second mode (depth gradient, colour lost); the 22 s `71146` compile stall (9.28).

## 9.31 Run 4 on the console (11252026-1025): zfreeze writes a plane that is not a plane, and the game stops submitting after level 1 (2026-11-25)

Build 10:18 (run 4's instrumentation), 5.0 MB log, 313 s, 18,240 presents, 4,778 Aurora frames, 271 flicker edges,
93 captures. `R2_ZFREEZE_LOG=1` was in the pack, so the two questions run 4 asked were both answered.

**1. zfreeze is in use, and its reference plane is not a depth function.** 1,100 `zfreeze draw` lines, and the
plane the port fits is garbage nearly every time - and it is *refit* constantly (`gen` 3801, 3805, 3807, …):

```
zfreeze draw 647400: plane -3185723047936.0000 -144038467919679361385152643072.0000 546779655190486643832283874000371712.0000
  depth at centre (320, 240) 1.0000, top edge 1 bottom edge 1.0000 right edge 1
zfreeze draw 648600: plane -555531554548543596752385909702262784.0000 -inf inf
  depth at centre (320, 240) nan, top edge nan bottom edge nan right edge nan
```

A slope of 1e29 a pixel is not a depth slope: the reference triangle was almost at the eye (`w` near 0), so the
fit divided by a near-zero determinant and the result clamps to 1.0 - or to NaN - across the whole viewport. Every
depth-tested draw behind a buffer at 1.0 fails, while the draws that do not test depth still paint. That is the
flicker's picture, and it is a plain wrong-number bug of the same kind as the locked-cache offset: the plane is
computed from three vertices with no check that they produce a usable plane.

**2. The depth constant is 0.502 after all, and it is a *row split*.** With the format now printed,
`depth(z24) 0.000..0.502, 0.502 holds 55% of 640x480, 2/256 values` is a buffer holding `0x80` over **rows 0-263**
and `0` over rows 264-479: `depth-2830…2848` are all `128:55.0%/0:45.0%  rows 0-263=128 264-479=0`, and the next
frame with the world in it (2849) holds 256 values. So 9.30's 1.0 was a misreading (corrected there), and the
value is the middle of the range over a band of exactly 264 rows. Neither a whole-image clear
(`R2_FRAME_DEPTH_CLEAR=all`, `R2_DEPTH_CLEAR_AFTER_CLEAR`) nor a scissored quad known to the log produces it: run
4's `clear:` line only ever showed `0,32 256x256` at z 0.0, a shadow target's quad. **And that was a flaw in the
log, not a fact about the frame**: it printed one quad per frame, and the emulator run of the fix shows a level
frame carrying three (`quad 1`, `quad 2`, `quad 3`), so the world's own quad was never in the log.

**3. The world-less picture is the dark one, and the pair proves it.** `flk-before-2830` 11.2, `flk-after-2831`
11.2, `flk-before-2848` 11.3, `flk-after-2849` 30.4: the frame whose depth holds one constant is the frame with no
world in it. Past-pass draw counts, lit counts and pipeline families are identical, as in every run since 9.27.

**4. The black screen: the guest stops submitting, the port keeps presenting.** Last real frame 4,775; frames
4,776-4,778 are `2 passes, 1 draws replayed` (the port's own picture), and then no Aurora frame at all while
`frame ]` keeps counting to 18,240 presents at 59.9 fps and audio keeps running to the end (313 s). So the guest
is not dead - it is blocked or looping - and the renderer is healthy: its stats settle at `aurora: 1 draws` per
120 presents, i.e. the game touches GX about once every two seconds. The guest had idle gaps before (frames
330-373, 435-464, 1151-1175 - 44, 30 and 25 frames) and recovered from all of them, so this is not the level
loader's normal quiet. Nothing in the log says why: no fatal, no fence or stall line, no disc or readback message.
This is more urgent than the flicker and it is a *hang*, not a picture.

**Instrumented for run 5 (NRO 10:38, 64,050,006 bytes).**

| change | why |
|---|---|
| `R2_ZFREEZE_GUARD=1` (new, **behaviour change**, default off) | rejects a fit that is not a plane - non-finite, `|slope| > 1` a pixel, `|offset| > 2`, `w == 0`, `|1/w| > 1e6` - and writes the flat far depth `0.0` instead, which cannot reject anything. Logs each rejection (`zfreeze plane rejected: …; flat far depth written instead`). This is the candidate fix for the flicker |
| `R2_CLEAR_LOG=1` (fixed) | every depth-clear quad of a frame, up to eight, with its target: `clear: frame N quad K depth z … into Render texture 640x480#1 …` |
| `R2_FLICKER_LOG=1` (fixed) | the signature names its format: `depth(z24) …` or `depth(f32) …`, so 0.502 and 1.0 can never be confused again |
| `R2_PROFILE=900` (new here) | the port's 1 kHz pc/lr sampler for 15 minutes, a `logs/profile-NNN.txt` every two minutes. For the black screen: the top self-time rows while it hangs name the guest function it sits in |

Ryujinx, 110 s with those flags: no crash or fatal; `clear: frame 2 quad 1/2/3 … into Render texture 640x480#1`
(the per-quad fix working); `depth(z24) 0.000..0.000, 0.000 holds 100% of 640x480` in the menu; `[prof] on`,
`[prof] R2_PROFILE: sampling the game thread for 45 s -> logs/profile-NNN.txt`, and `switch/tools/prof_report.py`
resolved `profile-001.txt` with `debug/RL-NX-1038.elf` into `svcSignalProcessWideKey 4.7%`,
`rt::f_7FD5D6B0 3.5%`, `svcSendSyncRequest 2.0%` with callers (`fsFileRead`, `audoutGetReleasedAudioOutBuffer`) -
so the hang report will be readable. **Not exercised:** no zfreeze draws in the emulator's menu, so the guard's
rejection path is unproven until run 5.

**A hazard found on the way (wrong comment, not a bug):** `backend_commands.cpp` and the docs say "reverse depth,
0 = nearest" while `clear_depth_value()` maps GX's far (`0xFFFFFF`) to `0.0` and the depth compare is
`GreaterEqual` - so 0 is *far* and geometry nearer than the clear passes. The comment sent one hypothesis (9.30's
"far plane everywhere") off in the wrong direction; it is now written the right way up where it mattered.

**Run 5 answers:** does the guard change the flicker (and do its rejection lines appear); which quad, if any,
writes 0.5 with a rect the log now names; and, from `logs/profile-*.txt`, which guest function the black screen is
stuck in.

## 9.32 Run 5 on the console (11252026-1104): the flicker is fixed, and the black screen is bisected to the 10:18 build (2026-11-25)

**The flicker is gone.** `R2_ZFREEZE_GUARD=1` rejected 11 zfreeze fits in the first level (`vertex off the screen or w
near 0`, `degenerate reference triangle`) and wrote flat far depth instead, and no frame ever lost its world again:
**2,437 of 2,437** picture lines hold a full **256/256 depth values**, and **none** holds the 1-2 values that a lost
world produced (run 4: `0.502 ... 2/256 values` on every world-less frame). Flicker edges fall from **271 in run 4 to
5 here**, and those five are ordinary whole-picture changes (a cut or a camera jump between frames differing by 20/255,
which the trigger cannot tell from a flicker). The zfreeze plane was the flicker, as 9.31 proposed. That fix stays.

**The black screen, bisected across every log on disk.** The guest's own frames, and how long the last one was
followed by presents with nothing submitted (the "tail"):

| build | log | Aurora frames | tail | frames that went `lit 0` |
|---|---|---|---|---|
| Nov 24 16:59 | 1124-1716 | 7,565 | 0 s | - |
| Nov 25 00:42 | 1125-0843 | 10,140 | 0 s | - |
| Nov 25 09:02 | 1125-0924 | 10,654 | 0 s | **0** |
| Nov 25 09:02 | 1125-1001 | 5,339 | 0 s | **0** |
| Nov 25 10:18 | 1125-1025 | 4,778 | **27.9 s** | 4 |
| Nov 25 10:38 | 1125-1104 | 5,444 | **53.7 s** | 5 |

The 09:02 runs played level 1 for 528 s and 327 s at 1,380-1,420 draws a frame and never produced a single frame with
no lit pixels; 10:18 and 10:38 both end with one, then stop. So this is not the level loader's normal quiet and not a
random hang: it starts with the frame the picture goes black.

**What the flags are not.** The port echoes its boot.cfg at boot, so each run's exact config is in its log. 0924 and
1001 (no hang) and 1025 (hang) differ by **two log flags only** - `R2_CLEAR_LOG` and `R2_ZFREEZE_LOG`; 1104 adds
`R2_ZFREEZE_GUARD` and `R2_PROFILE`. Every heavy probe (`R2_DEKO_FRAME_LOG`, `R2_CAPTURE_ON_BREAK`, `R2_DEPTH_SHOT`,
`R2_FRAME_DEPTH_CLEAR=all`, `R2_DEPTH_CLEAR_AFTER_CLEAR`) was already on in the runs that do not hang. The config is
not the cause.

**What the code is, recovered without version control.** `switch/build.sh` archives every build as
`debug/RL-NX-HHMM.elf`, so 09:02 and 10:18 could be compared directly: extract the string literals of each and diff
them. Everything new between the last working build and the first hanging one is

- `clear: frame %llu depth quad writes z ... into <target>` - the per-quad clear log (9.31),
- `zfreeze draw {}: plane ...` and its two counters - the zfreeze probe (9.31),
- `depth %.3f..%.3f, %.3f holds %llu%% ...` and `depth not read (its fence was not ready)` - the format-aware depth
  signature (9.31),
- `s_viewportDepthNear` / `s_viewportDepthFar` - the viewport's depth range now reaching `DkViewport`,
- the `R2_CLEAR_LOG`, `R2_ZFREEZE_LOG`, `R2_SKIP_DRAW_RANGE` env names.

Nothing there is a guest-facing path, and a guest that stops submitting cannot be caused by a renderer that logs more.
**So the bisect says "since 10:18" and the diff says "only diagnostics", and both cannot be the whole story.** The two
readings that fit: the older sessions stopped short of the point in level 1 where this happens (0924 is the strongest
counter-argument: 528 s at 1,417 draws, no black frame), or the fault is a latent one that the viewport depth change
tipped over. Rather than guess, run 6 measures it.

**The black frame itself, in the 10:38 log.** Frame 5,441: `8 passes, 1426 draws replayed`, `depth(z24) 0.000..1.000,
0.000 holds 65%, 256/256 values`, `lit 0`. The geometry, its depth and its clears all arrive; only the colour is black.
Frame 5,442 onwards: `2 passes, 1 draws replayed` - the port's own XFB copy and nothing else. So the guest submits a
black frame and then submits nothing at all for the next 53 s, while presents run at 59.9 fps (`idle 1750 ms / busy
250 ms per 120 frames`, `aurora: 1 draws`), the audio thread keeps running to the end, and there is no fatal, stall or
fence message anywhere. Whatever stops is inside the guest, and the guest's own PC sampler is the instrument that names
it - but its two-minute cadence missed the window (profiles written at t=120 and 240; the stop is at t=339).

**New: the stall watchdog (NRO 11:16, 64,050,006 bytes, always on).** `switch/src/host_switch.cpp`'s frame hook, which
keeps running on a black screen, now watches Aurora's draw-call total (`rt::gfx::drawCalls()`, new in
`src/runtime/gfx/gx_bridge.cpp`). Four seconds without a new draw call is not a slow frame or a load:

```
[   14.359 stall ] guest quiet: no Aurora draws for 4 s at frame 240 (last frame's picture: aurora: 0 draws ...)
[   21.071 stall ] guest drawing again after 6 s quiet (frame 480)
```

and at that moment it calls `rlProfDumpNow()` (new in `switch/src/prof_switch.cpp`), which snapshots the sampler and
hands the copy to the writer thread, so the report covers the stall as it happens instead of by luck. Then one report
every 30 s of continued quiet (six at most, and only while the guest is not drawing), and a `guest drawing again after
N s quiet` line if it recovers - which measures a load as a load. Emulator, 130 s with the run-6 pack: no crash, the
watchdog fired of its own accord four times, wrote `profile-001..004.txt` on demand, reported an 18 s quiet spell in
the menu recovering, and `prof_report.py` resolved a stall dump (`rt::f_7FD5D6B0` 3.8%, `svcSignalProcessWideKey`
3.6%, `svcSendSyncRequest` 1.8%). That the emulator has 18 s quiet spells in its menus is worth knowing: on hardware,
a `guest quiet` that ends with `guest drawing again` is a slow load, and one that keeps going is the fault.

**Run 6's boot.cfg** (26 lines, md5 `37c2aa1353d014938273d9bc40434736`) keeps `R2_ZFREEZE_GUARD`, `R2_PROFILE=900`
(needed by the on-demand reports), the frame/clear/flicker/zfreeze logs and the break captures, and drops
`R2_DEPTH_SHOT`, `R2_FRAME_DEPTH_CLEAR`, `R2_DEPTH_CLEAR_AFTER_CLEAR` - they existed to read and rewrite the depth
buffer while chasing the flicker, and they perturb the frame they measure.

**What run 6 answers:** the guest function (or thread and syscall) the stall sits in, from the first report written 4 s
after the picture goes black; whether that function changes between the 4 s, 34 s and 64 s reports, which separates a
slow load from a real hang; and whether the stall still starts at the same place with the depth probes gone.

## 9.33 Run 6 on the console (11252026-1130), and the first performance pass: where the time goes (2026-11-25)

**The black screen did not happen.** Run 6 played 268 s for 15,720 presents and 4,390 Aurora frames, and the guest
never stopped submitting. The only `stall` lines are two 4 s quiet spells in a load, both of which reported
`guest drawing again after 4 s quiet` - the watchdog doing what it was built for. The flicker stayed fixed.

What run 6 changed was the boot.cfg: `R2_DEPTH_SHOT`, `R2_FRAME_DEPTH_CLEAR=all` and `R2_DEPTH_CLEAR_AFTER_CLEAR` came
out (nothing else did, and the NRO's game path barely moved - 9.32's ELF diff). **So the depth probes, not the game,
are the leading suspect for the stall**, and that is worth saying plainly: the port was clearing the depth buffer and
copying it out once a frame, and the guest stopped submitting on the runs where it did. It is one run, so this is a
correlation, not a verdict: if the black screen comes back, the depth probes go back on one at a time.

Also: **three frames went `lit 0` in this run without the guest stopping.** A dark frame is therefore a symptom, not
the trigger - 9.32 read the two together because they had always arrived together.

**The on-screen frame counter (NRO 11:35, 64,050,006 bytes).** Top middle, `FPS 14`, drawn by the present callback
(`switch/deko/backend_present.cpp`) with the same 3x5 text box the boot screen uses, over the game's picture.
`R2_FPS=0` hides it. **It counts real presentations, not the guest's retraces**, which is the whole point:

```
[dk] frames 1688-2009: 10.7 presents/s; per present: frame fence 0.00 ms, acquire 0.16 ms, record 12.73 ms, submit+present 0.23 ms, longest 14.19 ms, 2.8 submits
[  30.713 perf ] 30 s: 28.8 fps at 60 Hz, 225 frames over 25.0 ms, longest 1091 ms, 1511 draws/frame, game thread busy 87% (presenting 0.2 ms/frame), level 22
```

The guest's own `frame ]` line says 59.9 fps in the same window, because the guest paces itself on the emulated
retrace and cannot see how long its frame really took. The port was never running at 60 in level 1: it presents
**10-17 frames a second** there (28 in the menus), i.e. 60-95 ms a frame.

**Where those 60-95 ms go.** The render worker's own accounting (every 30 s) is unambiguous: `record 12.73 ms`,
`submit+present 0.23 ms`, `frame fence 0.00 ms`, `acquire 0.16 ms`. Recording a frame (CPU: replaying Aurora's
commands into deko3d) is the renderer's whole cost, and the GPU is never waited for - **the GPU has headroom; the port
is CPU-bound.** The guest's frame is the bigger half: `game 75.2 ms/frame` in the in-level window, with the game thread
**100% busy**, and total presents at 10-17 a second.

Self time on the main thread, 120 s in level 1 (`profile-004.txt`, 1,594 frames, `game 75.22 ms/frame`; the sampler
taps about four threads, so read these as shares of all sampled time, not as a per-frame budget):

| function | share | what it is |
|---|---|---|
| `drawBoundsEfb` | 2.4% | guest: its own display-list bounds pass, 98% self-recursive |
| `onCommand` | 1.6% | guest: GX command decode |
| `musyxDspMail` | 1.0% | guest audio DSP mailbox |
| `f_8002759C` | 0.8% | guest |
| **`XXH3_hashLong_64b_default`** | **0.7%** | **port: texture hashing in the texture cache** |
| `svcSendSyncRequest` | 0.5% | `fsFileRead` 82% of it |
| `execute` / `cmdLength` / `gxFifoWrite` | 0.4% each | guest FIFO parse and the bridge's write |
| `memmove`, `ensureFrame` | 0.2% each | port |

Three things follow. **One, almost all of it is the recompiled game, not the port** - the top six are guest code, and
the only port entries of any size are the texture hash (XXH3, ~1.9 ms a frame) and `memmove`. **Two, the consumer of
the CPU is single-threaded**: the guest's frame is one thread's work, so cores 0 and 2 cannot help it. **Three, the
GPU is idle enough that draw-count work is not yet the lever** - `submit+present 0.23 ms` and no fence wait.

**Threads and cores, from the log's own placement lines.**

```
THREAD Main thread policy tuned allowed 0x7 requested 1 actual 0x2 preferred 1 priority 44
THREAD Aurora render worker policy tuned allowed 0x7 requested 2 actual 0x5 preferred 2 priority 44
THREAD Aurora FIFO processor policy tuned allowed 0x7 requested 0 actual 0x5 preferred 0 priority 44
THREAD deko worker role 3 preferred core 2 mask 0x7
```

So they are **not** on the same thread, and not on the same core: the guest's main thread sits on **core 1**, and the
renderer (Aurora's render worker, which records, submits and presents) and the GX FIFO processor share **cores 0 and
2** - mask `0x5`, so either can land on either. Two consequences worth acting on: the render worker and the FIFO
processor can collide with each other on one core, and the profiler's 1 kHz sampler is pinned to **core 2**, one of the
two the renderer is allowed on - it costs the renderer while it runs.

Also fixed on the way: the depth-shot's float test compared a `DkImageFormat` against `WGPUTextureFormat_Depth32Float`.
The two enums share no values, so it was always false and every float depth target was decoded as Z24 - the compiler
had been warning `-Wenum-compare` about exactly that line, and it is now compared against `DkImageFormat_ZF32`. Nothing
measured in 9.30-9.32 changes (they were about a colour buffer), but the depth-shot instrument was lying.

**Not verified:** the counter's position and legibility were not seen on a screen (no screenshot path here); the
emulator run of this build only proves the overlay draws without crashing an 95 s session.

## 9.34 The texture hash cache: 1 full hash a frame instead of 54 (2026-11-25)

**The hotspot, and why it was pure waste.** `textureIdentity()` (src/runtime/gfx/gx_bridge.cpp) hashes each bound
texture's bytes with XXH3 to notice content changing behind unchanged texture registers. Its throttle was written for
the PC build: *once a frame, but no more than 60 times a second*. On the console a level frame is 60-95 ms, so the
60 Hz floor never bites and **every distinct texture in the frame is fully hashed, every frame** - 54 distinct
textures in the emulator's level frame, several hundred binds - and 9.33's profile put XXH3 at 0.7% of sampled time,
~1.9 ms of the guest thread's 75 ms frame.

It almost never found anything. Every `stats` line of the 1130 run reads `changed 0` next to 300-870 binds. The port
was spending 1.9 ms a frame re-proving that nothing had moved.

**The cache.** A full hash is now taken only when something says the bytes could have moved:

| signal | what it is |
|---|---|
| no hash yet | first bind of a texture (the key is address + image0 + mips + levels, as before) |
| `epoch` changed | **BP 0x66 = GXInvalidateTexAll**, the game's own "texture memory changed" write, now counted into `s_texEpoch` in the live BP handler; every texture's next bind re-hashes |
| the probe moved | `textureProbe()`: eight 64-byte windows from an even spread over the image, chained through `XXH3_64bits_withSeed` - about a thirtieth of the read, on whole cache lines, so an image rewritten in place (a movie frame, a menu fade, a CPU-decoded texture) moves it at once |
| the safety net | a round robin: one texture in every `R2_TEX_HASH_NET` (64) frames takes an unconditional full hash, so a change the eight windows miss is still corrected, without ever costing a whole frame |

`s_strictOn` (R2_STRICT) and `R2_TEX_HASH_EVERY=1` keep the old every-frame hashing, so the two can be compared on
the same route. The EFB-copy destination check and the per-copy hash in `prepareCopy()` are untouched: those images
change every frame by definition.

**The risk is measured, not hidden.** The net is the only path where a full hash can disagree with the cache without
the probe having moved, so that case increments `s_probeMiss` and logs `texture %08X (%ux%u): contents changed without
moving the probe` (first eight only). The stats line now ends the texture group with
`hashed <full> of <full+probes>, missed <n>`; a `missed` of 0 over a run is what says eight windows are enough, and a
non-zero one names the textures that need a wider grid.

**Emulator check (Ryujinx, 110 s, NRO 11:45, 64,050,006 bytes):** no crash, and the level frame that used to hash
every texture now reads

```
texture binds 276 (changed 0, repeated 276, copy 0, flat 0, hashed 1 of 54, missed 0); 817 indexed XF loads
```

i.e. **one full hash and 53 probes where there were 54 full hashes**, with nothing reported as missed. (The emulator
proves correctness only: Ryujinx does not model A57 cost, so this says the cache works, not what it saves.)

**What the console run should show.** The counter is the honest measure, since the render worker is not the thread
that hashes: the guest thread loses ~1.9 ms of its ~75 ms frame, so the counter should move by a percent or two at
level frame rates (expect ~13.3 -> ~13.6 fps, not more - the frame is 75 ms and the hash is 1.9 of it), and
`record` in the 30-second `[dk] frames` line should be **unchanged** (12-13 ms), because the recording happens on the
render worker, which never hashed. Both numbers are in the same log line block, so the run answers it directly. If
the counter does not move, the profile was attributing the hash to a different thread than the one this change
touches and the next step is to profile again with `R2_PROFILE=900` and look at XXH3's share.

## 9.35 Threading is not the limit: three cores, one of them working (11252026-11:52)

The question this answers is "optimize threading", and the honest answer is that there was no threading win
left to take - the guest is single-threaded, and the two Aurora threads exist only to hand its work to the
GPU. What the profile says, per thread, from `profile-004` of run 6 (453,724 samples over 256 frames):

| thread | share of all samples | of *its own* samples in `svcWaitProcessWideKeyAtomic` |
| --- | --- | --- |
| 0 game | 21.3 % | not in its top 12 - it is busy |
| 1 GX FIFO processor | 26.2 % | **86.9 %** (64.8 ms of 74.5 ms per frame) |
| 2 Aurora render worker | 26.2 % | **81.7 %** (60.9 ms per frame) |
| 3 shader compiler | 26.4 % | **99.6 %** (74.8 ms per frame) |

The percentages are shares of *sampled time*, not CPU time: the sampler walks the thread list in order, so
the four threads come out at ~25 % each almost regardless of what they do. The `svcWaitProcessWideKeyAtomic`
column is the one that means something, and it says the three worker threads are idle ~82-100 % of the frame.
The FIFO processor's real work is ~9.7 ms/frame (`memmove` 1.3, `fill_uniform` 0.9, `prepare_idx_buffer` 0.8,
Aurora's own `XXH3_hashLong_64b_withSeed` 0.8, `draw_prim` 0.7, `process` 0.4, the rest spread thin); the
render worker's real work is ~10 ms/frame of `kk::draw`, bind-group command packing and staging.

So a frame is one saturated guest thread and three mostly-empty workers. Spreading the guest's work over
cores would mean making the guest multithreaded, which the recompiled PowerPC code is not. Everything that
will move the frame rate is therefore on the guest thread, and it is listed at the end.

### What was changed anyway (correctness of placement, not a speedup claim)

`place_thread()`'s "tuned" policy gave both Aurora threads the mask `0x5` (cores 0 and 2) with different
preferred cores. Horizon does not time-slice threads of equal priority, so if the render worker was already on
core 2 the FIFO processor could sit behind it, or the other way round, for a whole frame. Each role now owns
one core outright, and the log says so:

```
THREAD Main thread policy tuned allowed 0x7 requested 1 actual 0x2   (core 1, alone)
THREAD Aurora render worker policy tuned allowed 0x7 requested 2 actual 0x4   (was 0x5)
THREAD Aurora FIFO processor policy tuned allowed 0x7 requested 0 actual 0x1   (was 0x5)
```

The background workers (shader compile, cache) went from `0x5` to the lowest core only - the render core is
now the renderer's. The profiler's sampler thread used to pin itself to the *highest* core, which is exactly
where the render worker now lives; a 1 kHz sampler on the renderer's core is the one place it costs a thread.
It takes the lowest core instead, beside the FIFO processor that is waiting 87 % of the frame.

### The hotspot this found, and the 9 % it gives back

`drawStatsNote()`, the `R2_STATS` per-draw measurer, was `drawBoundsEfb` at **6.7 ms of the guest thread's
75 ms frame** - 9 % of a level frame, on the one thread nothing else can help. It decodes every draw list a
second time and transforms its vertices so the line can print "how many draws fell outside the scissor".
The measurement is now opt-in: `R2_STATS=1` keeps the cheap counters (draws, texture binds, indexed XF
loads), the breakdown comes back with `R2_STATS_BOUNDS=1`, and the line says which mode it is in:

```
frame 120: 484 draws (bounds not measured: R2_STATS_BOUNDS=1 adds the breakdown); texture binds ...
```

Also worth correcting in the record: run 6's 10.7 presents/s and `record 12.73 ms` were measured with
`R2_CAPTURE_ON_BREAK`, `R2_CAPTURE_FLICKER`, `R2_FLICKER_LOG` and `R2_DEKO_FRAME_LOG` on, and the capture
path's `break_judge` is **7.19 ms/frame on the render worker** (9.6 % of its samples). It is properly gated -
`break_capture_present` returns at once unless `R2_CAPTURE_ON_BREAK` is set - so the perf config does not pay
it, but the frame numbers from that run carry it.

### Where the guest thread's 75 ms actually goes (profile-004, self time)

```
6.7 ms  drawBoundsEfb   (R2_STATS measurer - now off by default, see above)
4.5 ms  rt::gfx::onCommand  (guest GX command handling in the bridge)
2.9 ms  rt::musyxDspMail    (the guest's own audio DSP mailbox)
2.3 ms  rt::f_8002759C      (guest code)
1.9 ms  XXH3_hashLong       (texture identity - the 9.34 cache)
1.6 ms  svcSendSyncRequest  (75 % audout, 25 % fsFileRead)
1.2 ms  gxFifoWrite
1.1 ms  cmdLength / 1.1 ms execute / 0.9 ms f_7FC90610 / 0.9 ms f_7FCBAD28 ...
```

The rest is thousands of small recompiled guest functions, which is what a 60 Hz GameCube frame costs on one
A57. Next candidates, in order: `onCommand`'s per-command path (4.5 ms for 836 draws = 5.4 us a draw),
the audio mailbox pair (4.5 ms with the audout IPC), and the FF3 re-check in the 9.34 cache. Note that
`drawBoundsEfb` was 9 % and it took a per-thread view of the profile to see it - the flat main-thread table
lists it as guest-looking code.

Emulator check of the three changes (`run10smoke`, 100 s): no crash, placement lines as above, stats line in
the new form, 484 -> 5604 draws across the boot/attract/level sequence. Ryujinx cannot show the speed gain.

## 9.36 The guest's own optimisation level: -O2 -> -O3 (11252026-12:01)

9.35 closed the last real hotspot that was *not* the game itself: after the stats measurer goes, the guest
thread's 75 ms is ~90 % recompiled PowerPC, spread over thousands of functions with nothing above 0.9 ms.
That code had been compiled at `-O2` since the guest core was brought up, inherited from the desktop build's
idea of "release". It is the one lever that acts on the thread nothing else can help, so it is now `-O3`.

`switch/CMakeLists.txt` grew a cache variable used by both Clang targets (the generated game code and the
runtime, whose hot half - `cpu/`, `interp`, `vm`, `hle_*`, `gfx/gx_bridge` - runs on the same thread):

```cmake
set(RL_NX_GAME_OPT "-O3" CACHE STRING "optimisation level for the recompiled game and the runtime")
```

`switch/build.sh` passes it through, so the old level is one word away and the A/B is a rebuild:

```
RL_NX_GAME_OPT=-O2 switch/build.sh 8      # the 9.35 build
RL_NX_GAME_OPT=-O3 switch/build.sh 8      # this one (default)
```

What is known, measured here: 123 TUs carry `-O3` in `build-switch/build.ninja`; the build takes 261 s on
8 jobs, allows no different code path (no `-ffast-math`, no `-funsafe-math-optimizations`, and
`-fno-strict-aliasing` is still on, so nothing that decompiled pointer-chasing code relies on changed); the
NRO grows **64,050,006 -> 65,250,134 bytes (+1.9 %)**; a 110 s Ryujinx run boots, draws (5,600 draws by
frame 240) and reports no crash or fatal.

What is *not* known: whether it is faster. Clang's `-O3` adds loop unrolling and vectorisation, and on
decompiled code those can lose more to i-cache than they gain - this is a hypothesis to be settled by the
FPS counter on hardware, not by the emulator (Ryujinx emulates the CPU; it cannot time it). If it regresses,
`RL_NX_GAME_OPT=-O2` puts the previous build back exactly.

### Why not threading, still

The audio path remains the only guest-thread work that could move: `rt::musyxDspMail` (2.92 ms/frame, the
MusyX DSP list interpreter) plus ~1.56 ms of `svcSendSyncRequest`, three quarters of it audout
(`audoutAppendAudioOutBuffer` 38 %, `audoutGetReleasedAudioOutBuffer` 37 %). `switch/src/audio_switch.cpp`
has no thread by design ("the game thread must never wait on the audio device"); giving it one would move
~4.5 ms (6 % of a frame) off the guest, and would need the finished 32 kHz blocks handed over in a ring
buffer, the resample and the audout submits done on the worker, and the DSP list reply (`0xDCD10001`) posted
back through the guest's own mail queue to keep that queue single-threaded. That is a real design change with
audio-correctness risk, it cannot be validated for speed here, and it is 6 % - so it stays on the list rather
than in the build. The 9.35 placement work (one core per Aurora thread, sampler off the render core) is the
part of threading that could be fixed without inventing a race.

Next, in order of expected value: hardware A/B of 9.36; then the audio thread; then `-flto=thin` across the
game and runtime targets (thousands of tiny guest functions, so cross-TU inlining is where the remaining
compiler gain would be - at the cost of a much slower link and a much bigger build).

## 9.37 The guest accessors were the frame: 35.7x -> 20.6x expansion (11252026-12:10)

The goal is 60 fps locked, i.e. 16.7 ms of a frame that now measures 75 ms. 9.35 and 9.36 took out the two
things that were not the game (the stats measurer, the -O2). What was left is the recompiled guest code, and
the question "why is the guest's 16 ms of Gekko work 75 ms of A57 work" finally has a number.

**How it is measured** (`build-switch/tmp/expand.py`, repeatable): the generated source keeps one
`// PPPPPPPP: XXXXXXXX` line per guest instruction, and `nm -S` gives each function's host size, so
host-instructions-per-guest-instruction is `size/4 / lines`. Six of the profile's hottest guest functions,
852 guest instructions in total:

```
before 9.37   total 30,430 host / 852 guest = 35.7x    (worst: f_7FD27418  4,181 / 59 = 70.9x)
after  9.37   total 17,560 host / 852 guest = 20.6x    (worst: f_7FD27418  2,665 / 59 = 45.2x)
```

35.7x is not "decompiled code is hard": it was one specific mistake. Every guest load and store inlined the
whole accessor ladder - `isIo`, then `isVm` (twice, once inside `hpr` and once for the page-split test), then
the locked-cache test inside `hp()`, then the arena mask and the byteswap - **plus the body of every cold path
behind those tests**. In `f_7FD27418` that was 4,181 host instructions for 59 guest ones, with 54 call sites
to `vmMiss`, 54 to `lockedCachePtr()`, 41 to `ioWrite`, 38 to `wrSplit` and 12 to `rdSplit`/`ioRead` - all of
them unreachable in a normal frame. Every guest access paid for three range tests and dragged the rest
through the i-cache.

**The fix.** `ctx.h`'s Switch branch now tests the arena once:

```cpp
inline bool inArena(u32 a) { return (a - kGuestRamBase) < kGuestRamSize || (a - kUncachedBase) < kUncachedLen; }
inline u8* arenaPtr(u32 a) { return g_ram + (a & kGuestRamMask); }   // serves both views
```

and everything else is one out-of-line call into `memory_switch.cpp`'s `rdSlow`/`wrSlow`, which run the old
chain in the old order. The order is load-bearing: IO registers (0xC8000000-0xDFFFFFFF) mask into the arena
the way the uncached view does, so IO is recognised first; the second range test stops at 0xC8000000, where IO
begins; the VM region keeps its page-split test, so a VM access that crosses a 4 KiB page still goes byte by
byte through `rdSplit`/`wrSplit`; the locked cache keeps its 16 KiB buffer; and anything else still lands on
`memPtr`, exactly as before. The desktop build is untouched (its accessors are the old ones, under `#else`) -
it has a real 4 GiB reservation and no arena.

Measured here: expansion 35.7x -> 20.6x; NRO **65,250,134 -> 40,833,878 bytes (-37.4%)**; build 162 s. An
110 s Ryujinx run reports no crash, and the draw counts are identical to the 9.36 build at the same frames
(3 / 484 / 484 draws at frames 0 / 60 / 120; 1,405 draws, 4,902 merged), which is the evidence that the new
path reads and writes the same bytes as the old one.

Also fixed in the same build: **`-fwrapv`** on both Clang targets. 9.36 shipped `-O3` without it, and the
guest's `add`/`mullw` are supposed to wrap; the port skill's rule for recompiled code is
`-O2 -fwrapv -fno-strict-aliasing`. `RL_NX_GAME_WRAP` is the cache variable, `-fwrapv` by default.

**What was not measured: the frame rate.** The executed cost of a guest load went from ~15 host instructions
(three compare-and-branch ladders, three loads, a mask and a byteswap) to ~7 (one short-circuited pair, the
mask, the load, the byteswap), so the expectation is tens of percent rather than the 4.5x that 60 fps needs.
Ryujinx cannot time the CPU, so this needs the FPS counter on hardware, against the 9.36 build
(`debug/RL-NX-1201.elf`, 65,250,134-byte NRO) run on the same route and mission.

**What is left of the 20.6x**, in order, for whoever continues: the float-conversion helpers inline their
denormal loops (`s2d`, `d2s` - hundreds of instructions each, several per function, rarely entered but paid
for in i-cache); `psqStoreQ`/`psqLoadQ` are 12 call sites per function; and every guest register read/write
goes through the `Ctx` memory array, because `__restrict` only stops the callee's stores from invalidating
the caller's registers. Getting to 60 fps means the last of those - a codegen that keeps `c.r[]` in host
registers across calls - not more tuning of this shape.

## 9.38 The GOT load on every guest access, and two dead ends (11252026-12:24)

Reading the host code of a leaf guest function after 9.37 (`f_80032738`, 35 guest instructions) showed the
shape of one guest store now:

```
and  w8, w8, #0x3fffffff        // inArena's second range
sub  w0, w9, #0x8, lsl #12      // the effective address
lsr  w8, w0, #27
cmp  w8, #0x18
ccmp w0, w20, #0x8, ne          // ... and its first
b.ge wrSlow                     // cold
adrp x8, GOT
ldr  x8, [x8, #2016]            // <- g_ram's ADDRESS out of the GOT
ldr  x8, [x8]                   // <- g_ram
and  x9, x0, #0xfffffff
strb w1, [x8, x9]
```

The arena pointer was being fetched through the GOT: three instructions and two dependent loads before the
address is even formed, on **every** guest load and store. The cause is the toolchain: `SwitchFoxhollow.cmake`
compiles everything `-fPIC` (line 30), and an NRO is a static ELF that never `dlopen`s anything, so position
independence buys nothing and costs this.

Two ways of turning it off do not work here, both found by trying them:

- **`-fno-pic` on the two archives**: `ld.exe: read-only segment has dynamic relocations`. The NRO is linked
  `-fPIE` (see the link line), and a non-PIC object puts absolute relocations into the read-only segment.
- **`-fvisibility=hidden` on the two archives**: the same link error, for the same reason - their address
  constants become non-preemptible absolutes and land in `.rodata`.

What works is hiding the one symbol that matters. `g_ram` now carries
`__attribute__((visibility("hidden")))` (ctx.h and memory_switch.cpp), which is enough on its own: a hidden
global cannot be interposed, so Clang gives it `adrp` + `ldr` and drops the GOT hop. GOT references in
`f_80032738`: **3 -> 1** (the survivor is `g_trace`'s check at function entry). NRO 40,833,878 ->
**40,706,902 bytes**; build 162 s; a 110 s Ryujinx run has no crash, the same draw counts (3 / 484 / 484 / 484
at frames 0 / 60 / 120 / 180) and audio clean (2358 buffers out, 0 dropped).

A build-system trap worth remembering: `RL_NX_GAME_PIC` is a CMake `CACHE` variable, so a value set by an
earlier configure wins over the new default in `CMakeLists.txt` and the build silently kept
`-fvisibility=hidden` from the failed attempt (the error looked identical, and it was the same build).
`switch/build.sh` now passes `-DRL_NX_GAME_PIC` through like `RL_NX_GAME_OPT`.

Not measured: the frame rate again. Two instructions and a dependent load per guest memory access is worth
something, but Ryujinx cannot time the CPU. `RL_NX_GAME_PIC=-fvisibility=hidden` reproduces the failed
attempt; leaving it empty (the default) is the shipped state.

### Where the 60 fps goal stands after 9.35-9.38

| change | what it removed | measured |
| --- | --- | --- |
| 9.35 | the `R2_STATS` bounds measurer on the guest thread | -6.7 ms of a 75 ms frame (profile) |
| 9.35 | one core per Aurora thread, sampler off the render core | placement verified; no speed claim |
| 9.36 | `-O2` on the guest code and runtime | NRO +1.9 %; speed unmeasured |
| 9.37 | three range tests + 4 inlined cold paths per guest access | expansion 35.7x -> 20.6x, NRO -37 % |
| 9.38 | the GOT hop on `g_ram` | 3 -> 1 GOT loads per function; speed unmeasured |
| 9.37 | `-fwrapv`, which 9.36's `-O3` shipped without | correctness |

Together they are the difference between "the port spends its frame on things that are not the game" and
"the port spends its frame on the game". They do not add up to 4.5x, and nothing at this level will: the
remaining expansion is the codegen itself (guest registers live in a `Ctx` array and are reloaded around every
call, so a call-heavy function cannot keep them in host registers). The recompiler is a prebuilt
`build-switch/host/r2recomp.exe` with no source in the tree, so that change is not available from this side.
The next real steps, in order of expected value: (1) the hardware numbers for 9.37+9.38 versus 9.36 - if the
memory-access work did not move the counter, the profile is dominated by something else and should be redone
with a per-thread view on the new build; (2) the audio thread (9.36's 4.5 ms); (3) `-flto=thin` across the two
guest archives, which is the only compiler lever left that could cross the call boundary and let Clang keep
guest registers in host registers.

## 9.39 Hot-call clustering, and the arithmetic of 60 fps (11252026-12:38)

9.37/9.38 took the per-instruction cost down; what is left of the 20.6x is the **call convention**: the guest's
registers live in `Ctx`, and a call to another guest function forces the caller to write its live registers back
and reload them after, because Clang cannot see into a callee in another translation unit. Guest functions are
spread over 96 generated files, so a hot caller and its callee are almost never together and nothing is inlined.

**What the profile says the hot set is.** `switch/tools/hot-unity.py` reads a profile and the `.elf` of the
build that wrote it, maps sampled pcs to guest functions (with the same anchor bias prof_report.py uses:
`bias = report anchor - elf's rl_nx_prof_anchor`), and reports: **1,067 sampled guest functions, top 60 =
66.8 % of all guest samples**. Those 60 plus the 145 functions they call directly make a cluster of **205
functions, 42,617 lines** - 1.5 MB of source, compiled as one TU. It writes `gen/code_hot.cpp` and cuts those
definitions out of their 29 original files, so each is still defined exactly once.

A trap worth recording: the first attempt symbolised with the newest `.elf` instead of the profile's, matched
**one** function carrying 100 % of samples, and would have clustered the wrong code. The pcs are runtime
addresses, so the ELF must be the one that build wrote (the report's `build` line says which).

**Result.** At the default inlining threshold (225) Clang inlines nothing here - the average guest function is
~145 guest instructions, well above the cost cutoff - and the NRO came out **byte-identical** to 9.38
(40,706,902), i.e. clustering on its own is free but does nothing. With `-mllvm -inline-threshold=2500`
(`RL_NX_GAME_INLINE`, now the default) the cluster grows 295k -> 321k host instructions (+8.8 %), the whole
guest 6.64M -> 6.97M (+5 %), and the NRO **40,706,902 -> 42,099,542 bytes (+3.4 %)**. The sampled hot function
`f_7FD27418` did not change at all, and that is consistent: all 76 of its call sites are runtime helpers
(`wrSlow`, `rdSlow`, `psqStoreQ`, `frsqrte`), not guest functions - it is a leaf, so inlining has nothing to
do in it. The gain, if any, is in the call-heavy functions the cluster was built for.

Checks: 110 s Ryujinx run, no crash, draw counts unchanged (3 / 484 / 484 / 484 at frames 0 / 60 / 120 / 180),
audio clean (2349 buffers out, 0 dropped). Not measured: the frame rate, as ever on the emulator.

Safety and revert: a regenerated `gen/` puts the clustered functions back in their files, so `switch/build.sh`
now detects that (`gen/code_hot.cpp`'s first function also defined in a `code_NNN.cpp`) and drops the stale
cluster instead of letting `--allow-multiple-definition` pick one silently. To go back to 9.38 by hand: delete
`build-switch/gen/code_hot.cpp` and set `RL_NX_GAME_INLINE=` (a copy of the pre-cluster gen is in
`build-switch/gen.bak-hotunity`).

### The arithmetic of "locked 60 fps on stock clocks"

60 fps is 16.7 ms a frame; the last hardware profile (9.33 run, 75.2 ms) put ~70 ms of that in the guest
thread's recompiled code and ~5-10 ms in the port's own emulation of hardware. 9.35-9.39 removed most of the
latter (stats measurer 6.7 ms, GOT hops, most of the accessor cost) - call it 1.2-1.6x together, to be
confirmed on hardware. Getting to 16.7 ms from 75 needs **4.5x**, and the remaining cost is not waste:

| what | cost | can it be removed at this level? |
| --- | --- | --- |
| recompiled guest code, per-instruction | ~70 ms | only by codegen: 20.6 host insns per guest insn |
| - of which memory access | ~7 of those 20 | no: one arena test, mask, load, byteswap is the floor |
| - of which register-file traffic | ~8-10 | only by inlining every call, or registers in host regs |
| `onCommand` (port's GX decode) | 4.5 ms | partly: Aurora decodes the same bytes again on its own thread |
| MusyX DSP list + audout IPC | 4.5 ms | yes: a real audio thread (designed in 9.36, not built) |
| display-list replay (`execute`/`cmdLength`) | ~2.6 ms | yes: cache the translated list between replays |

So the honest evaluation is: **60 fps is not reachable by port-side tuning on this codegen.** The three rows
that can still be taken are worth ~10 ms of 75 - they would move 13 fps to ~15. The 4.5x has to come from the
guest's own instruction stream, and that means the recompiler's call convention (guest registers kept in host
registers across calls, i.e. a proper register allocation) or whole-program inlining. `build-switch/host/
r2recomp.exe` is prebuilt and its source is not in this tree, so neither is available from here; the whole
program is also split across 96 TUs, which is why inlining had to be approximated by a profile-driven cluster
rather than done properly with ThinLTO (the link goes through GCC's `ld` for a `-fPIE` NRO, which cannot take
LLVM bitcode).

The next decision is the user's: (a) get `r2recomp`'s source and change the call convention - the only route
to 60; (b) take the remaining ~10 ms and accept ~15 fps in levels; or (c) treat 30 fps as the target and
pursue (b) plus the audio thread. Everything is switchable: `R2_STATS_BOUNDS`, `RL_NX_GAME_OPT`,
`RL_NX_GAME_INLINE`, `RL_NX_GAME_WRAP`, and the cluster file.

## 9.40 Forcing the hot chains inline with `__attribute__((flatten))` (11252026-12:46)

9.39 got the hot call chains into one TU and raise the inliner's cost threshold, and the measurement said it
mostly did not work: the cluster grew 8.8 % where full inlining of its 145 callees would have duplicated
~190k instructions. Clang's cost model was still declining the callees - raising a number was the wrong tool.

`__attribute__((flatten))` is the right one: it orders the inliner to inline every call it can reach inside
that function, cost model aside. `switch/tools/hot-unity.py` now marks the 60 hot functions with it (default
on; `--no-flatten` for an A/B), and the effect is visible in the emitted code:

| function | before (9.39) | after (9.40) |
| --- | --- | --- |
| `f_7FCBAD28` | 6,198 insns, 2 guest calls | 8,220 insns, **0** guest calls |
| `f_8004B8EC` (top sampled) | 139 insns, 1 guest call | 935 insns, 7 calls at two levels deeper |
| `f_7FC90610`, `f_8002759C` | leaves | unchanged, as expected |

So the hot functions now contain their callees' bodies, which is the condition under which `Ctx& __restrict c`
lets Clang keep the guest's registers in host registers instead of writing them back and reloading them around
every call - the ~8-10 of the 20.6 host instructions per guest instruction that 9.39 identified as the wall.

Cost, measured: all guest code 6.97M -> **7.41M host instructions (+6.3 %)**, NRO 42,099,542 -> **43,864,918
bytes (+4.2 %)**, build 242 s. One function grows 6.7x (`f_8004B8EC`, which pulled in two levels), so its
i-cache footprint grows with it - the trade this attribute always makes. 110 s Ryujinx run: no crash, draw
counts unchanged (3 / 484 / 484 / 484 at frames 0 / 60 / 120 / 180), 1,421 draws, 4,949 merged, audio clean.

Still unmeasured: the frame rate. This is the change that most directly attacks the remaining 4.5x, and it is
also the one where the i-cache cost could eat the register gain, so it is the first thing to A/B on hardware:
`debug/RL-NX-1246.elf` with the cluster, `debug/RL-NX-1238.elf` without the attribute, `debug/RL-NX-1224.elf`
with neither, all on the same mission.

## 9.41 Widening the flattened cluster to 80 % of the guest (11252026-12:58)

9.40 proved `flatten` inlines the hot chains. This extends it to the point of diminishing returns: the same
tool with `--top 120`.

- **Coverage**: the profile has 1,067 sampled guest-thread functions; the top 60 were 66.8 % of guest samples,
  the top 120 are **80.2 %**. The cluster (hot + their direct callees) is 165 functions, 2.1 MB of source.
- **Effect on the emitted code**: the top-60 functions are unchanged from 9.40 (`f_7FCBAD28` 8,220
  instructions and 0 guest calls, `f_8004B8EC` 935 and 7), i.e. the new work is all in functions 61-120, which
  are now flattened as well. All guest code 7.41M -> **7.87M host instructions (+6.2 %)**; NRO 43,864,918 ->
  **45,691,734 bytes (+4.2 %)**; the guest's .text is now ~31 MB.
- **Checks**: 110 s Ryujinx run, no crash, 1,319 draws / 4,534 merged, audio clean, stats frames 3 / 484 / 484 /
  485 (the last is a mid-transition frame; earlier builds read 484 there and the draw count does move by one
  across a transition, so this is not yet a difference that matters - if it ever stops matching at frames 0/60/
  120, that is the signal).

A build note that cost a build: the first attempt was killed by a 5-minute command timeout while ninja was
mid-link, and the *next* build then failed with `CMake Error: File can't be removed and still exist:
librl_entry.a` because the dead run's archive step raced it. `rm -f build-switch/librl_entry.a` and a rebuild
cleared it. Long builds here go in the background with the log polled, not in a timed command.

### The hardware A/B ladder (all four kept, same mission, read `FPS` + the `[perf]` line)

| build | what it is | NRO |
| --- | --- | --- |
| `debug/RL-NX-1224.elf` | guest accessors + hidden `g_ram` (9.37/9.38), no cluster | 40,706,902 |
| `debug/RL-NX-1238.elf` | + hot cluster, default inlining (inert: byte-identical code) | 42,099,542 |
| `debug/RL-NX-1246.elf` | + `flatten` on the top 60 (66.8 % of samples) | 43,864,918 |
| `debug/RL-NX-1258.elf` | + `flatten` on the top 120 (80.2 %) | 45,691,734 |

If 1246 or 1258 is slower than 1224, the i-cache cost of flattening outweighs the register keeping, and the
answer is `--top 30` or `--no-flatten` rather than abandoning the idea. If it is faster, the next widening step
is the long tail (`--top 250`), which the same tool takes as an argument.

Nothing beyond this list can be evaluated from here: every remaining port-side item is ~1-3 ms (audio thread,
display-list replay cache, the duplicated GX decode), and the 4.5x the goal needs lives in `r2recomp`'s call
convention, which is a prebuilt binary in this tree.

## 9.42 CPU and GPU balance, measured: one core busy, everything else idle (11252026-13:14)

The question "use the best from CPU and GPU" is answerable from the last hardware log (run 6, 9.33 profile)
and the per-thread view of it (9.35). Per 71-75 ms frame:

| engine | work per frame | idle | evidence |
| --- | --- | --- | --- |
| guest thread (one core) | 75.2 ms | **0 %** | `game 75.22 ms/frame`, no wait call in its top twelve |
| GPU | off the critical path | **~14 ms** | `frame fence 0.00 ms, acquire 0.16 ms, submit+present 0.22 ms` at 13.9-14.7 presents/s |
| render worker (Aurora command recording) | `record 9.98-10.97 ms` | **~85 %** | its own thread, 81.7 % of samples in `svcWaitProcessWideKeyAtomic` |
| GX FIFO processor | ~9.7 ms | **~87 %** | 86.9 % of its samples waiting |
| shader compiler | ~0 | **99.6 %** | |
| audio output | ~4.5 ms, but on the guest thread | thread unused | `svcSendSyncRequest` 1.56 ms + `musyxDspMail` 2.92 ms, both role 0 |

So the machine is one saturated core, two cores ~85 % idle, and a GPU with ~14 ms of every frame free. The
reason the idle engines cannot help is structural, not tuning:

- **Nothing left for the GPU to take.** Every GPU-shaped job is already on the GPU - vertex and fragment
  shading, rasterisation, EFB copies, the scan-out - because Aurora/Dawn does the drawing and deko3d presents.
  What remains on the guest thread is the guest's own PowerPC instruction stream (≈70 ms) plus three slices of
  emulation: GX command decode 5.6 ms (`onCommand` 4.5 + `gxFifoWrite` 1.1), audio 4.5 ms, display-list replay
  2.6 ms. A compute shader cannot run PowerPC; moving a memcpy-class slice to the GPU would add latency to a
  pipeline that already has none to spare.
- **The guest cannot be split across the two idle cores.** The game is a single-threaded PowerPC program; the
  port gives it one host thread and its own fibers. Parallelising it is a rewrite of the guest, not of the port.
- The three movable slices total ~12.7 ms of 75 (17 %). Each was examined and each is smaller than it looks:
  `onCommand` is mostly `fifo::write_data` copying the draw's bytes into Aurora's FIFO (the GPU side needs
  those bytes), the audio path's audout IPC is ~100 µs per call so a thread would move the cost off the guest
  without removing it, and the display-list replay is guest-code-driven, so caching its *output* is unsafe
  (the lists depend on live matrices and array state).

One experiment from this angle did not pay: `-fno-unroll-loops` on the flattened build shrank the NRO by 57 KB
(**-0.13 %**, 45,691,734 -> 45,634,390), i.e. `-O3`'s unrolling is not where the flattening's +12 % came from,
so the default build is restored (`debug/RL-NX-1314.elf`, 45,691,734 bytes, same code as 12:58's).

The honest summary of the whole CPU/GPU question: this port is GPU-rich and CPU-poor, and the CPU shortage is
inside the recompiled guest. Every remaining port-side slice is 1-3 ms, and the 4.5x the 60 fps goal needs is
in the guest's call convention (9.39). If the flattened builds (9.40/9.41) move the counter on hardware, the
next widening is the long tail; if they do not, the remaining ideas are worth 17 % at most, which is 13 -> 15
fps, and the goal needs `r2recomp`'s source.

## 9.43 Run 7 on the console: the ladder measured, and the frame is 8 us a draw (2026-11-25)

Four hardware runs of the 9.41 ladder, one `boot.cfg` for all of them (`R2_FPS=1`, `R2_STATS=1`,
`R2_ZFREEZE_GUARD=1`; no profiler, no bounds measurer, no frame log), same mission, 150-210 s each covering the
menu, the level load and in-level play. The builds name themselves in the log's first line, so the mapping is
not taken on trust: `build Nov 25 2026 12:24:06` = A (1224), `12:38:38` = B (1238), `12:44:11` = C (1246),
`13:11:11` = D (1314, the 12:58 code).

### One line describes all four builds

Frame time against `draws/frame`, from the 17 in-level `[perf]` windows (level 22; the two windows that contain
the load are left out - they report `longest 700-1400 ms` and are the fade, not the frame):

```
ms/frame = 4.1 + 0.00796 * draws/frame        7.96 us a draw, r2 = 0.97, n = 17
```

Fitted per build, the slope is **7.9 us (A), 7.2 (B), 8.1 (C), 8.8 (D)** - and the steepest is the widest
cluster. The heaviest windows (>6,200 draws a frame), which is where 60 fps would have to be won:

| build | draws/frame | fps | ms/frame |
| --- | --- | --- | --- |
| A 1224 | 6,962 / 7,154 | 17.5 / 16.6 | 57.1 / 60.2 |
| B 1238 | 6,962 / 6,323 | 17.3 / 18.1 | 57.8 / 55.2 |
| C 1246 | 7,077 / 6,793 | 16.6 / 16.6 | 60.2 / 60.2 |
| D 1314 | 6,355 / 6,259 | 17.0 / 16.7 | 58.8 / 59.9 |

At the one draw count that two builds share exactly - 6,962 draws - A is 57.14 ms and B is 57.80 ms, +1.1 %.
**The ladder is not a speedup.** Clustering plus `flatten` at top-60 and top-120 buys 0 to -3 % while costing
+7.7 % and +12.2 % NRO, so the hypothesis 9.39/9.40 tested - that guest registers held in host registers inside
the hot functions is where the remaining 20.6x lives - is falsified at the scale that matters: in the levels,
where the game actually runs, inlining the hot call chains changes nothing measurable.

### The profile that justified the ladder was taken in another regime

`profile-004` (9.35) was 836 draws a frame in a 75 ms frame. These runs are 4,400-7,100 draws a frame in
57-60 ms frames. Both are hardware and both report level 22, but they cannot be the same shape of frame: eight
times the draws and a *shorter* frame. So "the guest thread's 75 ms is ~90 % recompiled PowerPC, spread over
thousands of functions with nothing above 0.9 ms" describes the 836-draw frame - and nothing has ever profiled
the regime the game spends its time in. Every codegen conclusion in 9.36-9.41 rests on that profile.

### The arithmetic this points at

9.35 measured the port's own per-draw slices at 836 draws: `onCommand` 5.4 us a draw, `gxFifoWrite` 1.4,
display-list replay (`cmdLength` + `execute`) 2.6 - **9.4 us a draw**. The hardware fit above is 8.0 us a draw.
Either that is a coincidence between two regimes, or the hot levels are the port's GX decode and display-list
replay all the way down. Both readings predict exactly what the four builds did, which is why 12 % more NRO
moved nothing: guest codegen cannot touch a per-draw path.

Three things say it is not the GPU or the texture path: the present side is `frame fence 0.00 ms, acquire 0.15,
record 0.5-3.4` (0.5 us a draw), `submit+present 0.18` with three frames in flight and never a wait; texture
binds are 32-150 a frame against 4,400 draws; and the guest thread is busy 100 % in every heavy window while
the render thread idles.

### Next, and only this: profile the in-level regime

`R2_PROFILE=200` on `RL-NX-A-baseline.nro` (its ELF is `debug/RL-NX-1224.elf`), in the spot that reads 16-17
fps. The profiler starts at boot, so the value has to span the menu and the load: its reports are *incremental*,
a new file per interval (periodic every two minutes plus one on stop), so `R2_PROFILE=200` leaves profile-NNN
with 0-120 s (menu, load) and profile-NNN+1, the `stopped` one, holding 120-200 s - the in-level section, which
is the only one worth symbolising: `switch/tools/prof_report.py logs/profile-NNN+1.txt debug/RL-NX-1224.elf --top 40`. Be
in the heavy spot before 120 s and stay there until the SD write lands. If
`onCommand`, `fifo::write_data` and the replay dominate, the target is the port's per-draw path - a replay
cache keyed on the guest's list address with a validity check, and a cheaper `onCommand` fast path - and the
guest-codegen route is finished. If it is guest code after all, the locals emitter (register-file traffic) is
the next build; but `flatten` has already tested that hypothesis and returned 0, so it is not built before the
profile says so.

Correctness of this run: audio 3-8 dropped buffers per three minutes and none before the level, no fatal, no
fallback, draw and merge counts unchanged at matched times, no flicker with the guard on. `RL-NX-A-baseline.nro`
is also 5 MB smaller and equal-or-faster than the flattened builds, so it is the performance build until
something measurably beats it.

## 9.44 Profile on the baseline, in-level: 54 % guest code, 37 % port, and onCommand is 0.7 us a draw (2026-11-25)

Run 8 on the console: `RL-NX-A-baseline.nro` (the ELF is `debug/RL-NX-1224.elf`), `boot.cfg` =
`R2_FPS=1`, `R2_ZFREEZE_GUARD=1`, `R2_PROFILE=200`. The reports are incremental, and the fourth one is the
in-level section: **120.0-200.0 s, 80.0 s, 1,686 frames, 1,477 slow, 310,896 samples (8,064 dropped, 2.5 %)**,
`game 47.35 ms/frame`, present 0.06, wait 0.00, acquire 0.00, dawn renderer, `frame overlap 0`, audio 0
underruns. The `[perf]` windows inside that span read 19.9 fps at 6,159 draws, 17.1 at 6,751 and 18.7 at
6,372, so 47 ms is the heavy levels, not the menu - this is the profile the ladder should have been built on
and never was (9.43).

### Where the 47.35 ms goes (main-thread self time, 42.99 ms attributed)

| what | ms/frame | % | functions |
| --- | --- | --- | --- |
| recompiled guest code (`rt::f_*`) | 23.22 | 49.0 | 1,097 |
| guest memory accessors (`rdSlow`, `wrSlow`) | 2.39 | 5.0 | 2 |
| GX decode + display-list replay | 8.44 | 17.8 | 6 |
| texture hashing (XXH3) | 2.35 | 5.0 | 12 |
| audio: `musyxDspMail` + DSP | 2.11 | 4.5 | 8 |
| `svc*` IPC (audout, fs, sync) | 1.87 | 4.0 | 7 |
| FIFO copy (`memmove`, vector insert) | 0.75 | 1.6 | 2 |
| `ioWrite`, HLE, dispatch, `fctiw`, misc | 2.72 | 5.7 | 170 |
| **guest side** | **25.61** | **54.1** | |
| **port side** | **17.38** | **36.7** | |

The GX 8.44 is `onCommand` 4.54 + `gxFifoWrite` 1.37 + `execute` 1.17 + `cmdLength` 0.59 + `cpRun` 0.54 +
`burst` 0.24. The guest's largest single function is 1.263 ms (2.7 %), the next 0.845; seven are above 0.5 ms
and the remaining ~1,090 are a tail. There is nothing to attack in the guest one function at a time.

### 9.43's open question, answered: the per-draw cost is the game, not the port

9.43 left two readings of the 8 us/draw slope. This is the first one. `onCommand` is 4.54 ms a frame at ~6,600
draws - **0.69 us a draw** - and it was also 4.5 ms at 836 draws in profile-004, so what it charges for is the
vertex bytes it copies, not the number of commands. The entire GX path with the FIFO copy behind it is 9.2 ms
(19 %), nowhere near the 47 ms. The slope is the guest's own per-object work: 23.2 ms of guest code over ~6,600
draws is ~3.5 us a draw, plus the ~4 ms base the fit found.

### What this does to "locked 60 fps on stock clocks"

16.7 ms is the budget. Remove the port's 17.4 ms entirely and the guest's 25.6 ms is still **39 fps**. Remove
the guest entirely and the port's 17.4 ms is 57 fps. 60 needs both, so 60 is not reachable on this codegen -
the measured ceiling is ~39 fps, and that ceiling is what 9.39 called "the 4.5x has to come from the guest".
A locked 30 is reachable:

| work | what it takes | ms | frame |
| --- | --- | --- | --- |
| today | | | 47.4 (21 fps) |
| port: texture change tracking instead of hashing ~93 textures a frame | 2.35 | -2.3 | |
| port: audio thread (`musyxDspMail` + most of the audout IPC) | ~3.4 | -3.4 | |
| port: replay cache for `execute`/`cmdLength`/`cpRun` | ~2.3 | -2.3 | |
| codegen: 25-30 % off the guest's 25.6 ms | ~7 | -7.0 | |
| | | | **~32.5 ms (31 fps)** |

The first three are the port-side items 9.42 listed as "1-3 ms each"; at this frame they are 8 ms together
because the port is 37 % of it, not 6 %. The fourth is the recompiler's register model, and it is unproven:
`flatten` (9.40/9.41) changed nothing, so if the locals emitter also changes nothing, the guest's 25.6 ms is
not register traffic and the codegen route is closed for good.

The other two cores are still idle by construction, as 9.42 said: in this same 80 s the FIFO processor thread
spends 36.2 ms of 47.1 in `svcWaitProcessWideKeyAtomic` and the render worker 40.5 ms of 47.1. Only the guest
thread is working.

Correctness of this run: 0 audio underruns, no fatal, no fallback, no flicker (guard on), `frame overlap 0`.

## 9.45 The locals emitter builds and boots, and submits nothing (2026-11-25)

9.44 left one lever on the guest's 54 %: the recompiler's register model. The in-tree emitter's locals change
(guest GPRs as function locals, spills only around calls) was completed and built: 97 TUs regenerated, NRO
**42,386,262 bytes**, ELF `debug/RL-NX-1420.elf`, no compiler warning or error.

Ryujinx, 110 s: the guest runs at 59.9 fps and never submits a draw. `[8.925 stall] guest quiet: no Aurora
draws for 4 s at frame 240`, and at frame 5,040 (89 s) it still reads `aurora: 0 draws ... arrays sent: 0`. The
flat-120 build in the same harness draws by frame 720 (19.3 s: 6 draws, 478 merged), so this is not the attract
mode failing to draw. No crash, no fatal, no assert - the guest spins at 100 % busy.

**One real bug found on the way, and kept.** `canClobber()`'s pure list contained `safepoint(c` and `rfi(c`.
`safepoint()` (`runtime/threads.cpp:426`) *delivers guest exceptions* - `deliverException(c, 4/8, pc)` - and
`rfi()` returns from one; both run guest code that reads and writes `c.r[]`, so listing them as pure meant a
pending decrementer interrupt ran its handler while the caller's dirty locals were still in host registers. They
are no longer in the list, and the generated code now flushes inside the branch and reloads after:

```
if (c.ctr != 0) { if (g_irqPending) { c.r[0] = r0; ... safepoint(c, 0x800304E4u); r0 = c.r[0]; ...
```

That is correct code and it stays. The rebuild with it (`42,353,494` bytes, `debug/RL-NX-1429.elf`) still
submits nothing - 0 draws at 89 s again - so there is a second defect in the locals path that is not located.
Worth recording for whoever picks it up: `markDirty()` recognises only the textual form `rN = `, so any future
emitter output that assigns a guest register some other way is invisible to it, and the reload marker restores
*all* touched registers after every call rather than the ABI's volatile set - correct but paid for on every call.

**State left behind.** `gen/` was regenerated with `R2RECOMP_NOLOCALS=1`, i.e. the old `c.r[]` form (0 local
declarations in `code_086.cpp`), so the next `switch/build.sh` produces working code; and `RL-NX.nro` on the
card was rebuilt from `debug/RL-NX-1224.elf` with elf2nro at 40,706,902 bytes - the same code as A. The four
ladder NROs are no longer in `build-switch/sdcard/switch/RL-NX/` (only `RL-NX.nro` is); their ELFs are all still
in `debug/`, so any of them can be re-cut without a rebuild.

**Decision: the locals emitter is parked.** Two builds of it have failed to draw, its expected value was 25-30 %
of the guest's 25.6 ms, and 9.43 showed that the neighbouring idea - `flatten`, which puts the same registers in
host registers - measured 0 on hardware. The 8 ms of port work in 9.44's table is worth more and carries no such
risk: it is 17 % of the frame, it is in code that is already understood, and it is measurable on the console.

## 9.46 Phase 0 of the plan: metering, a state probe, and one bloat cut (2026-11-25)

`docs/PERF_PLAN.md` is now the working plan (three workstreams: measurement and correctness, port work off the
guest thread, a native GX path, and an LLVM backend). This is what phase 0 actually delivered, including the
part that did not work.

### R2_ICOUNT - the codegen metric (works)

The recompiler emits one count per basic block (`ctx.h: icountAdd`), so the executed guest instruction count is
exact. `R2_ICOUNT=1` then prints, next to the frame line, `[meter] 0.38 M guest insn/frame, 44.26 ns/insn,
23 M insn/s`. fps moves with the scene; ns per guest instruction does not, which is the point: it compares two
codegen builds on one scene.

Cost control: the counter is wrapped in `#if defined(RL_NX_ICOUNT)` in the generated code and the define comes
only from `RL_NX_ICOUNT=1 switch/build.sh 8` (CMake option `RL_NX_ICOUNT`). Compiled in it costs 2.5 MB of
`.text`; out, shipped code carries neither counter nor branch. An ordinary build says so if `R2_ICOUNT` is set.

Verified in Ryujinx (110 s): the counter tracks the boot (0.02 M insn/frame during the load, 0.38-0.39 M in the
attract) and the [meter] line prints. Ryujinx's ns/insn means nothing for speed - it emulates the CPU - so the
real number comes from the console.

### R2_STATE_HASH - mechanically right, and not a correctness gate

`R2_STATE_HASH=<frame>[,...]` prints `[state] frame N hash <xxh3> draws N` over the 32 GPRs, FPRs, CR/XER,
control and memory-management registers, all 256 MiB of guest RAM and the 16 KiB locked cache. It runs, it is
cheap (three hashes cost ~25 ms each) and it is on in every smoke from now on.

**It cannot compare two builds at a frame index, and the measurement says so.** Two runs of the *same* NRO,
same boot.cfg, printed different hashes at the same frames, and different draw counts:

| frame | run A | run B |
| --- | --- | --- |
| 300 | `33479a8ee8832f10`, draws 0 | `7b705bfdd8638238`, draws 0 |
| 600 | `142a274151a8224b`, draws 6 | `2f273a75c671bef1`, draws 6 |
| 900 | `c68891f77d1153cd`, draws **1** | `469c06f799a12854`, draws **7** |

The guest's timeline is wall-clock in several places - interrupts, audout and fs IPC, the present cadence - so
the same frame index is a different amount of guest work in each run. Any hash of that state therefore reports
a difference between a build and itself. The harness needs determinism before it can gate anything, and that is
plan phase 0b: a virtual timebase (events and the decrementer driven from the guest's own frame count), audio
from a fixed cadence, fixed IPC latencies - with "the same build hashes equal at frame N" as its acceptance
test, and a function-level differential tester as the fallback. Until then this stays a boot sanity probe.

### One bloat cut, measured: the dead cluster's inlining threshold

`RL_NX_GAME_INLINE` defaulted to `-mllvm -inline-threshold=2500`, raised for the profile-driven hot cluster
(9.39). The cluster measured 0 to -3 % on hardware (9.43), but the threshold applies to *every* TU, not just the
cluster: with the cluster file gone it still inflated the guest `.text` from 40,162,324 to 41,504,156 bytes
(+3.3 %), i.e. 1.34 MB of code that nobody measured a gain for. The default is now empty; a rebuild with the
current `gen/` comes out at exactly **40,706,902 bytes**, the A-baseline size, i.e. the emitted code is back to
the shape the hardware numbers of 9.43 were taken on. `RL_NX_GAME_INLINE="-mllvm -inline-threshold=2500"`
restores the old build for an A/B.

### Where the tree stands

`gen/` is the old `c.r[]` form (`R2RECOMP_NOLOCALS=1`), `RL-NX.nro` on the card is the 15:13 build
(40,706,902), the emulator's `boot.cfg` carries `R2_STATS=1` + `R2_STATE_HASH=300,600,900`, and the new switches
are in `README.PORT.md`'s table. Two smokes of the final build: no crash, no fatal, draws flowing (1,351 draws /
4,684 merged at 90 s, within a percent of the flat-120 reference), state lines printing.

## 9.47 Phase 0b, first attempt: a frame-driven guest clock deadlocks, so the gate becomes an oracle test (2026-11-25)

The plan said phase 0b is a deterministic guest timeline, because the state hash of 9.46 could not compare two
builds at a frame index. This is the attempt and its result.

### What was built

`R2_DETERMINISTIC=1` made `tbNow()` - the single function behind the guest's timebase register, the
decrementer, the scheduled-event queue and the audio pacing - return a virtual counter. It advanced by exactly
one frame period (`kTbClock / viRate()`, 675,450 ticks at 59.94 Hz) per presented frame, from
`viFrameHook` (`switch/src/host_switch.cpp`), and stood still in between, with `raisePending()` on each
advance so the event queue was re-evaluated on that frame rather than whenever the host timer thread woke.
The plumbing is in `runtime/events.cpp` (`detInit`, `detAdvanceFrame`, `deterministicClock`).

### What it did

The guest booted, then stopped at **4.18 s**, its last log line a DSP register write (`CC005028`), with no
frame ever presented and no further output for the rest of the run. It is a hang, not a crash.

### Why

The frame hook only runs *after* a frame is presented, and the guest cannot present one while it is waiting for
an interrupt that requires time to pass. The specific wait is the audio DSP: the boot hands the DSP a task and
waits for its interrupt, which the runtime raises from an event scheduled on the timebase. With the timebase
frozen, that event never comes due, so nothing presents, so nothing advances the clock - a deadlock by
construction. The same structure would bite anywhere the guest busy-waits rather than idling: this port's own
`[frame]` line has read `idle 0 ms / busy 2000 ms` in every hardware run, i.e. the guest thread never idles,
so an "advance when the guest idles" variant would not have helped either.

**Decision: whole-game determinism is not the gate.** Making it work needs the clock to advance with guest
*execution* (one count per basic block, which the metering build already emits), and then the guest's own
timing loops, audio and DMA pacing all run off a synthetic rate that no longer matches the display - a large
behaviour change whose only purpose is a test harness. The switch is left in the tree, inert, logging that it
is disabled: setting it can no longer turn a working build into a hang.

### The gate instead: an oracle test (plan phase 0b-ii)

Deterministic by construction, and it does not care that the game is not:

1. At a guest function entry (a probe the emitter can emit under a build flag, like `icountAdd`), copy the
   whole 256 MiB arena aside (~25-30 ms) and save the `Ctx`.
2. Run the same function body through the **interpreter** (`runtime/interp.cpp: interpret(Ctx&, u32 addr)`,
   the correct-by-construction path the port was brought up on) against that memory.
3. Hash the resulting `Ctx` and arena; then restore both, call the recompiled function by asking
   `g_funcTable` for its address, and hash the result again.
4. They must be equal. No frame timing, no input, no scheduling: one call, one comparison, both starting from
   identical bytes.

Rate-limited (a handful of probes a run) this is cheap, it names the exact function that diverges, and it
gates any emitter change - the C++ emitter as it is today, and the LLVM backend that comes next. What it needs
before it can be built: a bounded interpreter run (a step limit, so a function that never returns cannot hang
the test) and the probe hook in the emitter.

The tree after this attempt: `RL-NX.nro` is the 15:27 build (40,706,902 bytes, byte-size identical to the A
baseline), `R2_DETERMINISTIC=1` logs that it is disabled and the port runs normally, one smoke to confirm
(1,920 frames, draws flowing, `[state]` hashes printing).

## 9.48 Phase 0b delivered: the oracle test works, and it can fail (2026-11-25)

The gate 9.47 specified is built and measured. It does not need the game to be deterministic, because it does
not compare runs: it compares two **engines** on one function from one starting state.

### What it is

`R2_ORACLE=<n>` arms n probes. A probe build (`RL_NX_ORACLE=1 switch/build.sh 8`) emits one
`oracleProbe(entry, c)` at every guest function entry - compiled out of ordinary builds, where no call exists.
On a probe (`runtime/oracle.cpp`):

1. capture the live `Ctx` and copy the 256 MiB arena to a scratch buffer (allocated on the first probe, after
   the guest arena exists), plus the 16 KiB locked cache;
2. point `g_ram` at the scratch, mute every device (`ctx.h g_oracleIo` - `ioRead` answers 0, `ioWrite` and
   `gxFifoWrite` do nothing), and run the body through the **interpreter**
   (`interpretBounded`, step-limited to 400,000 so a body that never returns cannot hang the test);
3. hash the resulting `Ctx` + memory;
4. do exactly the same for the **recompiled** body via `g_funcTable`, from the same bytes, and hash again;
5. compare, and name the function if they differ. The live state is never touched: both passes ran on the
   copy, and the game continues as if nothing happened.

### Result

Six probes, six agreements, and the port ran normally after them (2,400 frames, 1,267 draws / 4,262 merged at
53 s):

```
[4.953 oracle] 8002E2EC ok (interpreter 3fd7b5af703859b8) - 1 passed, 0 failed, 0 skipped
[5.322 oracle] 8002E2EC ok (interpreter 6fbe61e58026f530) - 2 passed, 0 failed, 0 skipped
[6.778 oracle] 80029DE0 ok (interpreter c3aab9e8cb295a38) - 6 passed, 0 failed, 0 skipped
```

### The control that makes it a gate

`R2_ORACLE_FAULT=1` flips one bit of `r3` in the recompiled result. Every probe must then fail - and does:

```
[4.902 oracle] 8002E2EC MISMATCH: interpreter 3fd7b5af703859b8, recompiled f9aef6b60d1a06f9 (0 passed, 1 failed)
[5.269 oracle] 8002E2EC MISMATCH: interpreter 6fbe61e58026f530, recompiled 6f86343b9d839a18 (0 passed, 2 failed)
```

Two things come out of that pair of runs, and they matter more than the six passes:

* the **interpreter's hashes are identical in both runs** (`3fd7b5af703859b8`, `6fbe61e58026f530`, ...) - the
  oracle is deterministic precisely because it starts from captured bytes instead of a timeline, which is the
  determinism 9.46/9.47 could not get from the clock;
* a one-bit difference in the recompiled result is caught, so this is a gate and not a rubber stamp.

### Limits, stated plainly

Six functions, all reached within the first few seconds; the probe fires on the first entries after a 256-entry
warm-up and the budget is spent by ~6.8 s. Each probe is one call from one state, not a proof about the
function in general. What it does give is: a fault in the recompiled body of a probed function shows up, with
its name, in a log line - which is what the emitter and the coming LLVM backend need before any performance
number is believed.

The build of this section (`RL_NX_ORACLE=1`, 40,829,782 bytes, +123 KB of per-entry calls) is a diagnostic
build only; the card was rebuilt without the hook afterwards.

## 9.49 Phase 1b started: the hash cache stops paying for GX's invalidate (2026-11-25)

9.44 put 2.35 ms of XXH3 on the guest thread. `[stats]` said `changed 0` and 300-870 binds a frame, so the
work was hashing, not finding anything. Reading the cache 9.34 built, one guard defeated it:

```cpp
if (!t.hashTb || net || t.epoch != s_texEpoch) { full hash }
```

`s_texEpoch` moves on GX's own texture-cache invalidate (BP 0x66). A game that invalidates once a frame
therefore sent every bound texture back through a full 64-bit hash once a frame - the cache degenerated to
exactly the every-frame hashing 9.34 was written to remove, on every frame it mattered.

### What changed

The epoch is no longer a reason to take a full hash. "The hardware's cache is stale, look at memory again" is
satisfied by the cheap fingerprint, which the *else* branch takes every frame anyway for 512 bytes instead of
the whole image. A full hash now happens only on a texture's first sight and on the round-robin net
(`R2_TEX_HASH_NET`, 1 in 64 per frame). Nothing else in the identity path moved: a moved fingerprint still
takes the full hash and decides, a TLUT change still bumps the version, `R2_STRICT` and `R2_TEX_HASH_EVERY`
still force the old behaviour for an A/B.

### Why it is measured in bytes, not milliseconds

The emulator cannot time the guest (9.43), so the stats line now reports the frame's hashing **in KiB, by
reason** - `first`, `net`, `probe`, `copy`:

```
[stats] frame 0: ... texture binds 1 (changed 1, repeated 0, ...); ... hashed KiB first 8 net 0 probe 0 copy 0
```

Bytes times a measured XXH3 rate is an honest bound on what a change is worth, and it is the same on the
Switch and in Ryujinx. `copy` is the one path still hashed unconditionally every frame (EFB copy
destinations, whose images can be large); it is instrumented now so the next in-level run says whether it
matters before anything is built for it.

### Verified, and not

Verified in Ryujinx: builds clean, boots, the attract scene is unchanged against the pre-change build (1,267
draws, 4,218-4,258 merged per 120 frames, no fatal, `missed 0`).

Not verified: the in-level effect. The 2.35 ms only exists with a level's ~93 binds a frame, and a scripted
`R2_INPUT` sequence does not leave the attract scene in Ryujinx (tried: 4,080 frames, still 1,267 draws), so
the first honest reading of this change is the next hardware run's `[stats]` line in level 1: `hashed N of M`
should collapse towards the net (1 in 64), and `hashed KiB first/net/probe/copy` says where any rest goes.


## 9.50 The `-` menu finished, the release deko3d abort now logs, and a review of the plan (2026-11-25)

**Builds:** 18:42 (`debug/RL-NX-1842.elf`) and 18:45 (`debug/RL-NX-1845.elf`, the one on the card), both
deko, 0 errors, NRO 40,706,902 B (the same byte count as 17:07; the symbols below are in the ELF, so the
size is padding, not a stale link).

### The menu (README 7.2)

What changed, and why each was a bug rather than a polish:

| Problem in the 11-26 code | Fix |
|---|---|
| `R2_MENU_OPEN` read at static initialisation, before `boot.cfg` reaches the environment: it never worked, which is why no smoke ever logged `[menu]` | read on the first frame (`menuDraw`) |
| Minus is the modifier of every default pad hotkey (Minus + RB saves a state): a press-edge toggle opened the panel on every save, and the d-pad half then moved its cursor | the panel toggles on Minus *released* with no other button pressed while it was held; Minus + d-pad is never menu navigation |
| edges were set by `tick()` (once a vblank) and read by `menuDraw()` (once a game frame): at 20-30 fps two presses in three were lost | edges latch (atomic) until `padEdge()` consumes them |
| `pollMenuPad()` had no caller | one SDL query a vblank, shared by the hotkeys and the menu (`tick()`); `R2_MENU_PAD=0` turns the menu's half off |
| draw call `if (false)` | on, after `picture(user, 1)` (stage 2 records nothing for the game picture), viewport restored, nothing recorded while closed; `R2_MENU_DRAW=0` turns it off |
| state shared with the render worker as plain globals | atomics |
| a second, guest-frame FPS counter duplicating `R2_FPS` | the FRAME RATE row switches the existing present-rate counter (`swdk::overlay_fps_set`) |
| every row started at OFF; STATS LOG could not turn `R2_STATS=1` off (`on || envFlag`) | rows start from the real state; `setStatsEnabled(on)` sets exactly `on` |

**The stall question (AGENTS.md section 4, hypothesis 1) is answered.** Two 60 s smokes:

| build | panel | `[stall] guest quiet` at | `[menu]` |
|---|---|---|---|
| 18:42 | never opened (the static-init bug) | 10.3, 17.0, 25.7 s | none |
| 18:45 | opened at 12.3 s (`R2_MENU_OPEN=1`) | 10.3, 21.4, 28.2 s | `perf: panel open at start` |

The first stall precedes the panel in both, and the run without any menu activity stalls the same way: the
4 s gaps are the boot/attract sequence's black stretches. The watchdog line needs filtering, not a fix.

**Verified:** builds, links, the panel opens in Ryujinx and nothing faults. **Not verified:** the picture
(Ryujinx never reaches `frame_present`: no swapchain), the Minus tap, d-pad and A by hand, and the draw
counts against a baseline (one run each, intermittent timeline, so they are not compared here).

### The crash reports of 2026-11-25 (README 8.11)

Three Atmosphere reports (10:08, 13:29 = build 13:14, 17:22 = build 16:42), all `2359-0001`, all the same
stack: `__wrap_wgpuQueueSubmit` -> a copy closure's `barrier()`/checkpoint -> `submit_readback` ->
`dkQueueSubmitCommands` -> `Queue::onCmdBufAddMem` -> `flush()` -> "attempted to flush queue in error
state". The `check_queue` at the top of `submit_list` passed, so the GPU's error arrived between the check
and deko3d's own flush: earlier, already-flushed work faulted or timed out. That is 8.11's signature
(mission start with the crawl skipped). No log came back, and the release library logs nothing.

Change: patch 4 in `switch/third_party/deko3d/PATCHES.md`. Release `RaiseError` calls a weak
`rlnx_deko_release_error(result)`, which the port defines (`switch/deko/backend_commands.cpp`): one
`[dk] FATAL: deko3d release error` line with the queue state and frame, the breadcrumb history and the
recent-operation ring (`log_gpu_operation_context`, lock-free reads of GPU-written memory), then a
synchronous log flush. It takes no lock, because the failing submit holds them. **Not verified:** a fault
has not been provoked; the hook is linked (`nm`) and compiles in the release library only.

### Review of the optimisation approach (an outside read, not a measurement)

The measurement discipline is right and should stay: ns per guest instruction, the oracle gate (9.48), the
"tried and zero" list, hardware-only speed claims. The arithmetic (guest 25.6 ms, ceiling ~39 fps) is right.
What I would change is the order and the size of the next steps:

1. **Phase 3 as written (an LLVM IR backend) is the wrong next step.** The emitter already produces C++
   that Clang compiles; that *is* LLVM. The 20.6x expansion comes from the code's shape, and the shape can be
   changed from the C++ emitter:
   - Every guest store is a `memcpy` into `u8*`, which may alias anything: Clang must assume it changed
     `c.r[]`, `c.f[]`, `c.cr[]` (all reached through `Ctx& c`) and `g_ram`, and reload them after every
     guest store. *Hypothesis, cheap to test:* `Ctx& __restrict c` in the generated signatures, and
     `u8* const ram = g_ram;` once per function with the accessors taking it. One emitter change, gated by
     `R2_ORACLE`, measured by `R2_ICOUNT` ns/insn.
   - `inArena()` is two range tests per access because the arena is 256 MiB (it must exclude the IO window
     at 0xC8000000 in the uncached view). Moving the port arena from 0x8F000000 to below 0x88000000 makes the
     arena 128 MiB and the test one mask and one compare for both views. Check first that nothing the
     game uses lies at 0x88000000-0x8FFFFFFF beyond Battle of Hoth's ~14 MiB past MEM1.
   - The locals emitter (9.45) failed on bugs, not on the idea; it was written before the oracle existed.
     Retry it per function, oracle-gated, before any new backend.
2. **Phase 2 (a native GX -> deko3d path around Aurora) is a renderer rewrite** and the riskiest item in the
   plan. Before it: `gx_bridge.cpp` keeps its own BP/XF/CP shadow and decodes every FIFO command on the guest
   thread, and Aurora then decodes the same stream again on its own thread, which waits 87 % of the time.
   Much of the shadow work is diagnostics and heuristics (bounds, `xwingDraw`, `cardFill`, texture
   identity). Measure `onCommand` with the diagnostics compiled out, not only switched off, then move what is
   left to Aurora's FIFO thread. That is phase 1a/1c's goal with far less new code.
3. **The deko3d layer is the MKDD/SFA-NX one carried over wholesale** (`kartpad_*`, "course transition",
   `steady_v9`, `JKRPcBeginHostAlloc`). It works, but its load-time protection, `kartpad_gpu_transition_guard`
   (each GPU operation completes before the next while a level loads), has no implementation here, and
   that is the mechanism those ports used against exactly 8.11's 2348-0006 during loads. Implement it for
   Rogue Leader once the next crash log confirms a timeout inside the upload burst.

**Next step named:** the hardware log of the next mission-start crash (it now carries the breadcrumbs), and
in parallel the `__restrict` / hoisted-`g_ram` emitter experiment on one oracle-gated build.

## 9.51 Tatooine Training's 2348-0006: a kickoff storm, bounded (2026-11-25, build 19:08)

The 18:45 hook caught the crash (LOGS 11252026-1854, -1856). In both runs the GPU's last report sits one
kickoff (~22 uploads) behind the CPU, on the same upload (mip 6 of a 512x512 RGBA8 texture). In 1427 the GPU
completed 20 more uploads while the dump printed. So the GPU was working; the driver refused a kickoff. The
failing frame made 2371-4276 batch submits (`gpfifo_entries_recorded`, one kickoff per 128 entries, ~42
syncpoint increments each from the per-upload fences) and never waited for the GPU.
- **Change:** `device.cpp` waits for the GPU (`dkQueueWaitIdle`) every `R2_GPU_BURST_WAIT` batches in a frame
  (default 4, `0` = the old behaviour). `R2_GPU_BATCH_ENTRIES` sets the batch size (default 128).
  - Only load bursts reach it; steady frames make about 2 submits.
  - Cost: a longer load. Each wait is logged.
- **Logging:**
  - `GPU burst: frame F: B batch submits, W GPU waits (ms, longest)` for each frame over 8 batches.
  - A `GPU bursts (30 s)` summary.
- **Ryujinx smoke:** passed (12 batches, 3 waits in the attract load). Correctness only.
- **Unconfirmed on hardware.** If it still fails:
  1. Try `R2_GPU_BURST_WAIT=1`.
  2. Then try that with `R2_GPU_BATCH_ENTRIES=512`.
  3. If it still fails, the fence-per-upload count per kickoff is next: share one fence per batch.

## 9.52 Step 1 of the 60 fps plan: one build that logs everything (2026-11-25, build 19:49)

**Build 19:58 fix:** 19:49 died on hardware right after the boot screen (2168-0002 data abort, PC on `mrs cntvct_el0` in `idleWait`). Horizon traps EL0 reads of the virtual counter; Ryujinx allows them, so the smoke test passed. `perfmon.h` now reads `cntpct_el0` (the register libnx `armGetSystemTick` reads; same 19.2 MHz). Never use `cntvct_el0` in this port. 19:58 also renames the boot title from the SFA-NX leftover to ROGUE LEADER LOADING.

The plan (shared with the user 2026-11-25): measure first, then fix one limit at a time, each change behind a
`-` menu row and compared on the same manual route. The user runs the route by hand (no padrec replay) and
can only change settings from the `-` menu.

### What the 19:32 hardware run already says (LOGS 11252026-1932, build 19:08)
- **The crash is gone** (9.51 holds). The first ~10 s in the level look glitchy: 160 shaders compiled by uam
  on the console (~250 ms each) while 690 draws were skipped and ~330,000 drawn with the ubershader. That is
  the shader cache warming, not a new bug; a second run of the same level should be clean (Step 7: seed).
- **In flight the game thread is the limit:** 14-16 game frames/s, game thread busy 99-100 %, the frame
  fence wait 0.01 ms, the render worker recording 5.6 ms per present. 6,000-8,800 draws a frame.
- **The profile was cut short:** one 1.7 s report (written by the stall watchdog), then nothing, because the
  periodic report came every 120 s and the run ended first. Fixed: every 30 s.
- **In those 1.7 s:** `f_80055F60` is 41 % of the game thread (14.8 ms a frame), a paired-single loop
  (12 unrolled blocks, quantized `psq_st` stores) whose stores reach `wrSlow` (31 % of its callers), with
  `psqStoreQ` -> `wrSlow` the next cost. Candidate: quantized vertex data written to the gather pipe one
  element at a time through `ioWrite`. The new `pipe_wr`/`psq_st_q` counters confirm or refute this.

### What the build logs (src/runtime/perfmon.h, switch/src/perfmon_switch.cpp)
- **Counters, not timers, on hot paths:** `rdSlow`/`wrSlow` by region (IO by register block incl. the
  gather pipe and the EFB window, VM, locked cache, other), quantized psq loads/stores, draws, EFB copies to
  textures and the XFB (with pixels), texture re-sends, vertex array bytes, Aurora's per-frame stats.
- **Timers on rare spans (game thread):** idle wait, HLE DSP mail, disc reads (+bytes), audout calls, locked
  cache DMA (+bytes).
- **deko3d totals:** presents, frame fence / acquire / record / submit time, submits, burst waits, uam
  compiles and their time, shaders waiting.
- **Per-thread CPU %** from the kernel (`InfoType_ThreadTickCount`) for game, Aurora FIFO, render worker,
  audio and the pipeline workers.
- **Output:** five `[pm*]` lines a second and `[hitch]` lines (frames over 50 ms, with that frame's own
  deltas). The reporter times itself (`pm_us`). `switch/tools/perf_report.py` turns a log into per-level
  tables; "untimed" is the game time left for the profiler to split.
- **GPU TIMING row:** the SFA-NX per-pass timer (`gpu_timing.cpp`) was compiled in but hard-wired off; the row
  turns it on. Hardware only.

### Verified, and not
- Built (19:49), symbols checked (`rlnx_dk_totals` from `device.cpp`, not the weak fallback).
- Ryujinx smoke: all five line types and `[hitch]` lines appear; `perf_report.py` reads them; no fatal.
  Already visible there (emulator-independent counts): the attract loop reads the VM region ~230,000 times
  a frame through the slow path, and writes the gather pipe ~15,000 times.
- Reporter cost in Ryujinx: 62 us a frame on average (syscalls are emulated); to be read from `pm_us` on
  hardware.
- No hardware run yet.

### The test (one question)
Profiler ON and PERF LOG ON (default) from the `-` menu before selecting the mission; fly the same course
2-3 minutes; quit; send `logs/log.txt` and `logs/profile-*.txt`. Then: `perf_report.py` + `prof_report.py`
decide Step 2's limit.


## 9.53 Step 2, round one: the game thread's port overhead (2026-11-25)

Evidence: LOGS 11252026-2014 (build 19:58, 13 min, the whole of level 1 then level 22). 30.7 game fps, game
thread 95 % busy at 33.3 ms/frame, present 0.11, fence 0.01; FIFO thread 19 %, render worker 11 %. The game
thread is the only limit. Per frame: ~93k VM reads + ~13k VM writes + ~56k locked-cache accesses through
`rdSlow`/`wrSlow` (level 1: 187k lc writes, 166k VM reads), ~43k gather-pipe writes through
`wrSlow -> ioWrite -> gxFifoWrite`, and 7.4 ms in the GX path (onCommand 4.0, execute 1.0, gxFifoWrite 1.0,
cmdLength 0.5, cpRun 0.5). Hitches: 140-200 ms frames on the FIFO thread in `arb_mip_check`/`convert_texture`.

### What changed (one build, all on by default)
1. **Cheaper slow path (ctx.h, memory_switch.cpp).** `rdSlow`/`wrSlow` use Clang's `preserve_most`
   convention, so a call no longer makes the recompiled caller spill its caller-saved registers, and they test
   the common cases first with no further call: a VM access whose page is in the software TLB and stays inside
   it, the locked cache, and (writes) a gather-pipe append straight into the 32-byte buffer (`g_gpCur`/
   `g_gpLimit`; a full burst calls `gxGatherBurst`; a null limit - oracle test, R2_DRAW_CALLER - keeps the old
   `ioWrite -> gxFifoWrite` path). Everything else runs the old IO-first chain (`rdChain`/`wrChain`). Counters
   keep their meaning (each access counted once), so `[pm-mem]` reads the same; the time per access is what
   should move. `g_vmRead`/`g_vmWrite`/`g_lockedCache` are hidden (no GOT load).
2. **Why not inline (build 20:44, not shipped).** The first try put the VM/locked-cache tier and the pipe
   append inline in every guest access. It built and linked, and `.text` went 37.5 -> 66.6 MB (+78 %): the
   9.37 I-cache lesson again. Replaced by item 1 before any test.
3. **CP parse in place (gx_fifo.cpp `cpRun`).** Commands run straight from the FIFO in guest memory; only a
   command cut by a chunk end is carried in `s_cmd` (was: a vector insert + front erase per 32-byte burst).
4. **Bridge worker (gx_bridge.cpp).** `onCommand` - all of the bridge and its Aurora FIFO writes, and the frame
   boundary - runs on a worker thread fed by an 8 MiB single-producer ring. The game thread drains it (waits
   for the worker to go idle) at every point where the game can observe the GPU or touches bridge state: CP
   reads (gpSync), PE draw-done/tokens that raise an interrupt (hw.cpp peToken/peFinish -> `gxPeBarrier`), PE
   register access after a polled token, every vblank, save states, setMenuTiling, stats(). Between drains
   only one thread ever runs bridge code, so no bridge state needed locks. Worker: highest core (beside the
   render worker), priority 0x2D, profiler role 5, `[pm-thr]` name `gx_bridge`. Its wait shows as
   `gxsync=`/`gxsync_calls=` in `[pm]` and `gxsync=` in `[hitch]`. R2_BRIDGE_THREAD=0 starts it off; the
   `-` menu's **GX THREAD** row switches it at the next vblank.
5. **Arbitrary-mip check sampled (extern/aurora texture_convert.cpp).** Compares each level with a 2x2 box of
   its parent at <=1024 texels a level, no allocations (was: a full box-filtered chain in fresh buffers).

**Not done: hot-unity (fix 3 of the ranked list).** Judged, not just inherited from 9.43: it works through
`flatten`-inlining hot chains, and item 1 just made every guest access carry more inline code, so flattening
200 functions multiplies code size on a 48 KB I-cache for a gain that depends on the inlining. It also
rewrites `gen/`. Revisit if guest code still dominates once the port's overhead is off the thread.

### Verified
Build 20:48:42 (`debug/RL-NX-2049.elf`): `.text` 37.46 MB (19:58: 37.49). Disassembly: a VM TLB hit in `rdSlow` is ~20
instructions with no register saves. Ryujinx 120 s: attract loop, ~9,800 draws/frame (2,260 after merging), worker
started on core 2 at priority 45 and busy 27 %, 2-3.5 drains a frame, no fatal, no unknown FIFO opcode. No hardware run.

### What to read in the next log
`[pm-thr]`: `Main_thread` should fall and `gx_bridge` appear (expect 15-30 %). `[pm]`: `game` ms/frame and
`gxsync` (the game thread's wait on the worker - if this is several ms, the drains are too frequent and the
next step is making the PE barrier lazier). Profiler: `rdSlow`/`wrSlow` self time per call should be a
fraction of 19:58's (1.7 + 1.25 ms for ~180k calls a frame). `[hitch]`: fewer
140-200 ms texture frames. A/B: GX THREAD off in the `-` menu on the same course.

## 9.54 Three levels on build 20:48: where the frame goes, and the big levers (2026-11-25)

Log `E:\RL-NXLOGS\LOGS 11252026-2118` (stamp 20:48:42 matches `debug/RL-NX-2049.elf`), 1,138 s, three missions
(about 140-540 s, 570-870 s, 870-1140 s; the `[pm]` `lvl` byte reads 22 throughout, so split by time), 44
profiles. **36.9 fps over the run; 17-58 fps by 30 s window, and the low windows are the high-draw ones
(5-8k draws/frame).** GPU fence wait 0.00 ms; FIFO 22 %, render 16 %, bridge worker 34 %, game thread 88-100 %.
The game thread is the only limit, as before.

### The game thread, per frame (profiles 010-044, 35,409 frames, 28.4 ms of samples a frame)

| what | ms/frame | notes |
| --- | --- | --- |
| recompiled guest code (`rt::f_*`) | 17.3 | 1,896 functions sampled; top 10 = 21 %, top 50 = 47 %, top 250 = 83 % |
| GX hand-off on the game thread | 2.7 | `bridgeSink`/`ringPush` 1.33, display-list replay `execute`+`cmdLength` 0.82, `cpRun` 0.29, bursts |
| waits (condvar) | 2.5 | `gxsync` 1.36 (drains), idle 0.67, audio |
| `rdSlow`/`wrSlow` | 2.2 | per frame: 96.5k VM reads, 13k VM writes, 31k locked-cache, 58.6k pipe writes |
| MusyX DSP HLE | 1.0 | stays on the guest thread (user rule) |
| IPC | 0.8 | `audoutGetReleased` 0.33, `audoutAppend` 0.25, `fsFileRead` 0.21 |
| psq/fctiw helpers, memmove | 0.8 | |

Even with every port cost at zero, 17.3 ms of guest code is ~58 fps. **The big gains are in what the
recompiler emits**, not in any one guest function: the profile is too flat for hand-written HLE to matter
(the top 40 functions together are 43 %).

### What the emitter produces (disassembly of `debug/RL-NX-2049.elf`)

Host instructions per guest instruction in the hottest functions: `f_7FC90610` (PSMTXConcat-shaped, 51 guest
insns) **50x**; `f_7FC0A8B4` 55x; `f_7FCA8518` 21x; `f_8002759C` 17x. Reading the hot path of one `psq_l`:

* **`s2d` (every `lfs`, every float `psq_l` word): ~16 instructions with two branches**, a bit-exact software
  single->double, where the hardware `fcvt d, s` is exact for every non-NaN input (denormals included, FZ off).
* **Guest registers live in `Ctx`.** Each FP result is stored to `c.f[]`/`c.ps1[]` at once and each base register
  and `c.gqr[0]` is reloaded before the next access, because `c` escapes (it is passed to `rdSlow`'s callers'
  callees) so every out-of-line slow call - even one on a cold path - is assumed to modify it.
* **`g_ram` is reloaded after every store** (`ldr x8, [x21, #3576]`): a byte store may alias the global.
* **Every word of a multi-word access is tested separately**: `stmw r14` is 18 arena tests, `stfd`/`lfd` are two,
  a `psq_l` pair is two, each with its own cold `rdSlow`/`wrSlow` call stub.

### Ranked levers (estimates are game-thread ms/frame, from the numbers above)

1. **Emitter memory ops, no change to the register model (-2 to -3 ms, low risk).** `s2d` fast path
   (`fcvt` unless the exponent is all ones; the old code kept for NaN/Inf - exhaustively testable over all 2^32
   inputs on the PC); one arena test for 8-byte and paired accesses and for `stmw`/`lmw` ranges; `g_ram` read
   once per guest function into a local. Applies to all 1,896 functions.
2. **GX hand-off off the game thread (-3 ms, medium risk).** `ringPush` does a seq_cst store and a seq_cst
   load (a full barrier) per GX command, ~30k a frame: batch into a staging block published per burst or at a
   barrier. Push display lists as (address, length) references and let the worker parse them, and move `cpRun`'s
   command parse to the worker too - the real GPU reads both FIFO and display lists asynchronously, and the
   game already waits for tokens/draw-done before reusing them, which the PE barrier keeps. Then drop the drain at
   every vblank (keep it only where the game observes the GPU), so `gxsync` falls from 1.36 ms.
3. **Locals emitter v2: guest GPRs, FPRs/ps1 and CR fields as function locals (-5 to -7 ms, high risk, the
   largest lever).** Spill only at guest calls, exits and safepoints. 9.45 built the GPR half and it drew nothing
   in Ryujinx; the defect was never located because the oracle (9.48) did not exist yet. The gate now does: a
   locals build with `RL_NX_ORACLE=1` and a large `R2_ORACLE` budget names the first function whose result
   differs from the interpreter. Known weak spots to check first: `markDirty()` only sees `rN = ` text, and the
   reload after a call restores all touched registers.
4. **VM region on the fast path (-1.5 to -2 ms, medium-high risk).** 110k slow calls a frame are the
   VM-loaded module's own data (code and data sections are copied to 0x7FC00000+). If the game's page mappings
   change rarely, a host window aliased per page (`svcMapMemory`) placed so 0x7E000000-0x8FFFFFFF is one
   contiguous range would make those accesses the arena test. Needs a count of `vmMiss` and remaps first.
5. Small: audout calls once per buffer instead of per poll (0.3-0.5 ms).

1 + 2 are the certain ~5-6 ms (26 -> ~20 ms busy, ~45-50 fps in the heavy windows); 3 is what reaches 60.
Hitches are separate: level loads (0.4-1.1 s frames at the 24 MiB texture-upload cap) and 98 shaders compiled on
the console in this run (22.5 s of uam, 6,774 ubershader draws).

## 9.55 Levers 1-3 built: locals v2, cheaper memory ops, batched GX ring (2026-11-25, build 23:21)

Build **23:21** (`debug/RL-NX-2321.elf`, NRO 46,232,406 bytes) has all three levers. Saved copy:
`build-switch/tmp/RL-NX-2321-v2.nro`. The previous generated code (the old `c.r[]` form) is kept in
`build-switch/tmp/gen-nolocals` as the way back. The new recompiler is `build-switch/host/r2recomp-v2.exe`.

### Lever 1: memory ops (ctx.h, emit.cpp)
- `s2d` fast path: normal numbers and zeros use the hardware conversion. Denormals, infinities and NaNs keep the
  bit path. **Checked on the PC: it matches the old function on all 2^32 inputs** (0 mismatches).
- Every generated function reads `g_ram` once into `u8* const M = memBase()` (after the oracle probe) and passes it
  to `rd32(M, a)` and the rest. A guest store is a byte store that may alias any global, so the old accessors had
  to reload `g_ram` after each one.
- One arena test per multi-word access: `inArenaN(a, n)` for `lmw`/`stmw` (a plain load or store per word after
  it), `rd64`/`wr64` (one 8-byte access) and the psq pair. A range that leaves the arena takes the old
  word-by-word path. Note: the `stmw` fast path skips `RT_WATCH_HIT`. That macro is compiled out on the Switch;
  a `RL_NX_WATCH` build would miss stmw stores.

### Lever 3: locals emitter v2 (emit.cpp)
Two defects in 9.45's v1 were found by reading the code:
1. It tracked dirty registers in text order, not along control flow. The back edge cleared the set inside
   `if (g_irqPending)`, and a forward goto past a call that flushed reached a later return with nothing spilled.
2. `rdSlow`/`wrSlow` -> `vmMiss` -> `pageFault` builds the DSI frame at `c.r[1] - kFrameSize`. A stale r1 put
   that frame on top of the current function's stack frame.

v2 design:
- GPRs, FPRs, ps1 halves, CR fields and CTR are all locals: `r3 f1 p1 cr0 ctr`.
- There is no dirty tracking. A spill marker expands to a store of **every slot the function writes anywhere**.
  A reload marker reloads every slot it mentions. Storing an unchanged slot rewrites the same value, so this is
  correct on every path.
- Where the markers go:
  - Spill: before every call that may run guest code or read Ctx (anything taking `(c,`/`(c)` that is not on the
    pure list), before every return (inside the branch for a conditional return or tail call), inside the
    back-edge interrupt check, before `idleWait`, and before a tail call into the next entry.
  - Reload: after every one of those calls.
- r1 is written through to `c.r[1]` on every change.
- Slots are found by a token scan, not v1's substring search. It skips numbers whole, skips members after `.`,
  and counts `x = ` (not `==`) and `x--`/`x++` as writes.
- `psqLd` returns a `PsPair` that is assigned explicitly. An output reference was invisible to the scan; the first
  v2 draft lost those writes from the spill.
- `psqLoadQv`/`psqStoreQv` (cpu.cpp) carry the quantized types. `crS`/`crU`/`crPack`/`fcmpv` replace the
  `Ctx` CR methods.
- `R2RECOMP_NOLOCALS=1` still produces the old memory form, and that form compiles with the same ctx.h.

### Lever 2: GX ring (gx_bridge.cpp)
- `ringPush` no longer does a seq_cst store and a seq_cst load per command. It writes at `s_wrLocal` and
  publishes every 16 KiB (`ringPublish`: a release store, then a fence, then the sleeping check, which is Dekker
  with the worker).
- It also publishes before every wait (`bridgeDrain`, a full ring).
- The vblank drain stays. It keeps SDL/input single-threaded (the worker pumps Aurora events in `ensureFrame`).
- Parsing on the worker and display-list references are not done.

### Oracle sweep (oracle.cpp)
`R2_ORACLE_EVERY=<k>` probes every k-th function entry, and each function is probed once. So `R2_ORACLE=300`
covers 300 different functions instead of the first six entries.

### The oracle gate: 300 passed, 0 failed (Ryujinx, oracle build, 2026-11-26)
`R2_ORACLE=300 R2_ORACLE_EVERY=41`: **300 passed, 0 failed, 19 skipped**. 69 of the passes are VM-module
(7FCxxxxx/7FDxxxxx) functions, the hot ones. The game ran on to 382 s afterwards.

Control: `R2_ORACLE_FAULT=1` caught **20 of 20**. The gate can still fail.

Getting a sweep past ~20 probes exposed oracle flaws that the 6-probe test in 9.48 never hit. All are fixed in
oracle.cpp, threads.cpp, events.cpp, vm.cpp, cpu.cpp, interp.cpp and switch/src/host_switch.cpp:
- **VM page cache.** The VM TLB held pointers into the live arena, so both passes read and wrote live memory.
  During a pass every entry is now moved to the same offset in scratch, and the live TLB is restored after.
- **Live events.** `safepoint()` ran live device events and delivered interrupts inside a pass, with `g_ram` on
  scratch. A due ARAM->MEM1 page transfer was thrown away with the copy, and the live game then executed zeroed
  VM code (illegal instruction 00000000 at 7FC3DA04 / 7FC46478 / 7FCEE550). Inside a pass `safepoint()` now
  delivers nothing and leaves `g_irqPending` set for the live run.
- **Clock reads.** `mftb` is frozen at the probe's start. Before this, `8002F218` (`mftb` -> r3) and similar
  small functions mismatched only on the clock.
- **Abandoned probes.** A probe is skipped when a pass needs the pager (vm.cpp gives it a dummy page), idles
  (`idleWait`), spins (more than 200,000 safepoints), switches guest threads (`OSLoadContext`/`OSSaveContext`/
  `OSInitContext`/fiber switches) or calls `fatal()`. `oracleAbandon()` longjmps back to `runOne`.
- **VM code fetch.** The interpreter fetches VM-module code from a copy of the module's text taken at the first
  probe (DOL image at `0x80100000`, 2 sections, 1,654 KiB), because the pager keeps most of those pages out of
  memory.
- **Coverage.** `R2_ORACLE_EVERY=<k>` probes every k-th entry, and each function is probed once.
- **Diagnostics.** Each probe logs `probing X` before it starts, and a mismatch logs which registers and which
  64 KiB chunks differ.

### Ryujinx (correctness only)
- 75 s: first draws at 46 s, after a 16.6 s loading frame in the emulator.
- 150 s: attract loop steady at ~2,240 draws / 7,300 merged per 120 frames, 0 dropped audio buffers, no fatal.
- The 9.45 v1 build never drew.

**Release build for hardware: 00:56** (`debug/RL-NX-0056.elf`, NRO 46,236,502 bytes; it adds the oracle fixes
to 23:21). Ryujinx 150 s: ~2,205 draws / 7,114 merged per 120 frames, 0 dropped audio buffers, no fatal.
Hardware was not measured. Compare it with the 2118 run (36.9 fps, same three levels).

## 9.56 Build 00:56 on hardware: gameplay evaluation (2026-11-26, drop `LOGS 11262026-0749`)

The user ran 00:56 for 1,140 s across three levels (36.5 fps over the whole run, as before). Reported: the
intro and the "Start level" menu are very slow, in-engine cutscenes are fine, gameplay is smoother with no
glitches, and fps falls into the 20s during explosions and laser fire.

**Gameplay** (profiles 008-016, 240 s, game section 31.1 ms + present 3.9 ms). The game thread is the limit.
The other threads are mostly waiting: FIFO is 26 ms idle of 35, the render worker 28 of 35, and the bridge 20
of 35, with another ~4 ms of the bridge's busy time spent spinning (`workerMain` atomic load + `cpuRelax`).
Main thread, per frame (`scratchpad cat.py` buckets):

| bucket | ms/frame |
|---|---|
| guest code in VM-mapped functions (0x7Fxxxxxx) | 13.05 |
| guest code in the DOL | 4.62 |
| waiting (condvar: `gpSync`'s `bridgeDrain` + `fifo::drain`, present hand-off) | 5.12 |
| slow memory path (`rdSlow`/`wrSlow` self: VM TLB, locked cache, pipe append) | 3.30 |
| GX command parse on the game thread (`execute`, `bridgeSink`, `cpRun`, `cmdLength`, `burst`) | 3.21 |
| other runtime (`svcSendSyncRequest` 0.88 = audout/fs IPC, `fctiw`, `d2s`, `s2d`, ...) | 3.51 |
| DSP HLE (`musyxDspMail`; stays on the guest thread) | 1.57 |

The guest profile is flat: the hottest function is 0.69 ms. Of the guest samples, **13.4 % sit on loads and
stores to the Ctx register** (2.2 ms; call-site spill/reload: a call in 80057970 stores ~60 slots and reloads
~75) and **12.6 % on `[sp]` spills** (2.0 ms; register pressure from the reload-everything live ranges).
`gxsync` is 4.3 ms/frame over 3.9 calls; every CP register read (~49/frame) runs `gpSync`, which drains the
bridge worker and then Aurora's FIFO thread, although `gxCpRead` answers from game-thread state.

**Explosion frames.** Frames over 60 ms average 10,800 draws, 162k pipe writes and 9.1 ms `gxsync`, against
3,600 draws and 55k pipe writes on average. All buckets scale together (window 007 at 49 ms has the same
proportions as window 014 at 33 ms), so the cost is per draw on the game thread: guest code + pipe writes
+ parse + sync.

**Intro and "Start level" menu** (profiles 029-031, 67 ms/frame, ~15 fps). `f_80057970` alone is 22.2 ms;
it is the IDCT of Factor 5's `.movie` decoder (`celebration.movie` is the intro, `lvlvideo.movie` the
level-select background). Add `f_8005AD80` 3.1 ms plus the quantized paired-single chain `psqStoreQv ->
psqStoreQ -> wrSlow` (~10 ms: the decoder's output goes to the locked cache). The in-engine cutscenes do not use it.

Ranked levers (estimates, not measured):
1. **GP thread**: CP parsing moves to the worker, and CP/PE progress comes from what the worker has consumed
   instead of a blocking drain. About 8 ms average, more in explosions. Medium-high risk (timing, memory reuse).
2. **Call-site liveness** in r2recomp: spill only what was written since the last call, reload only what is
   live after it. About 2-3 ms, plus menus. Oracle-checkable.
3. **Movie decoder native**: `f_80057970` (and its helpers) in native code, plus a fused quantized
   `psq_st` to the locked cache. Fixes the intro and menu (67 ms -> ~35 ms estimated). Oracle-checkable.
4. **VM fast path** in the 7F functions (TLB lookup inline where the address is VM). About 1.5-2 ms.
5. **audout/fs IPC off the game thread** (lever 5 of 9.54). About 0.9 ms.

## 9.57 GP thread, audio-out thread, call-site summaries, quantized psq direct path (2026-11-26)

Built on 9.56's levers 1, 2, 5 and part of 3. Correctness checked in Ryujinx only; **speed not measured on
hardware yet**.

**GP thread** (`hw/gx_fifo.cpp`, `gfx/gx_bridge.cpp`; R2_GP_THREAD, - menu GP THREAD, on by default).
- The game thread keeps the CP/PI registers, the breakpoint and the read pointer, so the GP still looks
  "infinitely fast" to the game, as before. It no longer runs the commands. A walker (`gpWalk`) finds where
  each command ends: it follows the vertex formats, including CP writes inside display lists. It then hands
  whole commands to the bridge ring. The worker runs `execute` -> `onCommand` (`gxGpParse`).
- A display-list call is replaced by a **copy of the list taken at the call** (`gpDlSink` / `gxGpParseDl`).
  The first build read lists late on the worker. The game reuses a list once the read pointer passes the call,
  so the rewritten list's vertex formats sized later draws wrongly: "unknown FIFO opcode E3" at 92 s in one of
  two Ryujinx runs, and then no drawing.
- PE draw-done and tokens are posted by the worker (`gpToken`/`gpFinish`). Before posting, the worker
  `fifo::drain`s Aurora's thread, so the game cannot reuse memory the renderer still has to read. It then
  `wakeGuest()`s. The game thread applies them in `safepoint()` and at PE register access (`gxGpPoll`).
  They arrive asynchronously, as on the console.
- CP status reads no longer sync. CP reads, PE reads, idle loops and safepoints only publish the ring
  (`gxGpKick`). The vblank drain stays.
- Still read late: vertex arrays, textures and indexed XF loads. 9.20 found late indexed loads pixel-identical,
  but the window is now longer, so look for flicker in explosions and particles. GP THREAD OFF is the A/B.
- Ryujinx: two 150 s attract runs and the 420 s oracle run had no misparse, about 2,190 draws a frame (as 00:56),
  and no fatal.

**Audio-out thread** (`switch/src/audio_switch.cpp`; R2_AUDIO_THREAD, - menu AUDIO THREAD, on by default).
- `push()` queues the MusyX block (32 slots). A 2 ms-poll thread at priority 0x2B on the lowest core does the
  resample and the audout IPC.
- The DSP stays on the guest thread. The [pm] `audio` timer now counts the thread's time, not the game's.

**Call-site summaries** (`src/recompiler/emit.cpp`, `build-switch/host/r2recomp-v3.exe`).
- Each function's read set (every slot it mentions) and write set are closed over its callees. A direct call
  to a non-opaque callee spills only the caller's written slots in the callee's read set, and reloads only the
  caller's slots in its write set.
- Opaque functions: `dispatch` (2,549 own), `rfi`, `sc`, `RT_SAVECTX_CALL`, `hle_*` hooks, and functions that
  return twice. Safepoints are not opaque. The handler saves and restores the interrupted registers, and the
  caller's values live in its locals (`R2RECOMP_SUMMARY=strict` makes them opaque; `=0` turns the summaries off).
- Result: 4,582 of 8,104 functions are opaque. 18,988 of 37,559 call sites are summarised, which removes
  265,968 stores and 309,161 loads statically. The NRO is 600 KB smaller.
- The previous gen is in `build-switch/tmp/gen-v2` (built with `r2recomp-v2.exe`).
- Oracle (Ryujinx, oracle build): `R2_ORACLE=300 R2_ORACLE_EVERY=41` gave **300 passed, 0 failed, 22 skipped**,
  and the game ran on to 400 s.

**Quantized psq direct path** (`cpu.cpp` `psqLoadQv`/`psqStoreQv`).
- An access inside one arena view, the locked cache or one VM page converts the pair in place, with the same
  conversions as before. IO, page splits and watched addresses take the old path.
- Aimed at the movie decoder's ~15 ms/frame in `psqStoreQv -> psqStoreQ -> wrSlow`.
- The oracle sweep compares the interpreter's `psqLoadQ`/`psqStoreQ` with the compiled `Qv` path.

**Not done**
- **The movie IDCT itself** (`f_80057970`, 22 ms). It is the whole macroblock loop, not a bare IDCT, and
  compiles to 88,832 host instructions for 1,667 guest ones. 39 % of its samples are loads and 11 % `fmov`:
  64 double locals (f0-31 plus ps1 halves) against 32 host FP registers, plus inline `s2d`/`d2s` per psq.
- Two routes for it. A native decoder means reversing the bitstream calls (`f_8005B400`/`f_8005BAB0`) too.
  The general route is a **paired-single codegen mode**: f/p pairs as `float32x2` NEON registers. +, -, *, /
  and sqrt are bit-identical to `rsingle(double op)` (double rounding is innocuous at 53 >= 2*24+2). Only the
  fused ops need a double fma per lane. That halves the register count in every ps-heavy function. It needs
  per-register typing, because `stfd`/`lfd` save and restore doubles.
- **The VM inline fast path** (9.56 lever 4). 9.53 measured +78 % `.text` for the inlined form, so it needs a
  narrower design: for example, only the loads in the hottest 7F functions.

**Release build 09:12** (`debug/RL-NX-0913.elf`, NRO 45,511,510 bytes; Ryujinx 150 s: ~2,173 draws/frame, no misparse, no fatal). **For the hardware run.** Compare with 00:56 (LOGS 11262026-0749) on the same levels:
- explosion fps and `gxsync`;
- the intro and level-select fps;
- any flicker. If there is any, try GP THREAD OFF first.

**Saved - menu rows** (`switch/src/menu_switch.cpp`, 2026-11-26).
- Each switch of a row writes every row to `config/menu.cfg` in the port's SD folder, as the names that set
  their start state (`R2_FPS`, `R2_STATS`, `R2_PROFILER`, `R2_PERFLOG`, `R2_GPU_TIMING`, `R2_BRIDGE_THREAD`,
  `R2_GP_THREAD`, `R2_AUDIO_THREAD`).
- The next boot reads the file right after `boot.cfg`, and the file wins (`[opts] menu.cfg ...` line).
- PROFILER from the file has no time limit. A `boot.cfg` `R2_PROFILE=<s>` still runs even if the saved row is OFF.
- Delete the file to go back to `boot.cfg`.
- Ryujinx: loading checked (GP THREAD off and PROFILER on from boot took effect). The save, which happens on a
  button press, is untested.

**SD folder layout** (2026-11-26).
- The folder root now holds only `RL-NX.nro` and the disc. `config/`: `boot.cfg` (optional, diagnostics only),
  `menu.cfg`, Aurora's `input.json` and `imgui.ini` (Aurora `userPath`). `cache/`: `deko/` (was `Cache/deko`, the
  same folder on FAT), `aurora/` (was `aurora-cache/`), `update.nro`. `logs/` and `saves/` are unchanged.
- `R2_ZFREEZE_GUARD` is on by default (`extern/aurora/lib/gx/command_processor.cpp`); `=0` turns it off. With FPS a
  saved menu row (and the inverted `R2_FPS` test fixed in `switch/deko/device.cpp`), normal play needs no `boot.cfg`.
- No migration code: old files at the root are ignored. The user moves them on the console; the Ryujinx folder was
  moved by hand (its `config/boot.cfg` is the oracle config while testing).

## 9.58 Float FPR typing, the paired-single codegen mode (2026-11-26, uncommitted)

- **What:** the recompiler (`build-switch/host/r2recomp-v4.exe`) types a function's FPR as `float` when every write to it is a single-precision result (`fadds`/`fmuls`/`lfs`/`ps_*` and friends). Those registers live as plain C `float` locals instead of `double` with a round-to-single on every op. Result in `gen/`: 2,819 functions, 11,911 registers typed float.
- **Off switch:** `R2RECOMP_FLOATFPR=0` regenerates the 9.57 form. The previous gen is kept in `build-switch/tmp/gen-v3`.
- **Fixed on the way:** the FPS row fix landed in the same build (see the menu notes).
- **New oracle filter:** `R2_ORACLE_ONLY=<hex>,<hex>,...` in `config/boot.cfg` checks only the listed functions, for targeted probes.
- **Oracle (Ryujinx, 420 s sweep, `build-switch/tmp/oracle5`):** 298 passed, 2 failed, 21 skipped. Both failures (`7FD3F46C`, `7FC08668`) differ only in a NaN payload in f2 (interpreter `fff8000000001fb8`, recompiled `fff8000000000000`; ps1 matches). A NaN that passes through a float loses its low payload bits. This only matters if the game stores the NaN and compares its bits as integers; nothing seen does. Watch for it if those two functions turn up in a bug.
- **Not measured on hardware yet.** The gain is expected in FP-heavy game code (fewer `fcvt` round trips); it needs a log on the console.
