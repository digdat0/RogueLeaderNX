# RL-NX performance plan

Written 2026-11-25, after the measurements in `PERF_HANDOFF.md` §9.43-9.46. This is the working plan for
making the port fast on Switch hardware; `PERF_HANDOFF.md` stays the record of what each step measured.

## What is established (do not re-litigate)

* In-level frame on the console, baseline build: **47.35 ms (21 fps)** = **25.6 ms guest (54 %)** +
  **17.4 ms port (37 %)** + ~4 ms unattributed (§9.44). Guest = recompiled PowerPC + its memory accessors.
  Port = our GX decode/replay, texture hashing, audio, IPC, io.
* Only the guest thread works: the FIFO thread waits 36.2 of 47.1 ms, the render worker 40.5 of 47.1.
* Codegen: **20.6 host instructions per guest instruction**, ~7 of them the memory floor; 31 MB of guest
  `.text`; 1,097 sampled guest functions, largest 1.27 ms, i.e. a long tail with no hot function to fix.
* Clustering + `flatten` measured **0 to −3 %** on hardware (§9.43). The locals emitter has failed to draw
  twice (§9.45). Both attempted the same thing - guest registers in host registers - by hand.
* The ceiling with today's codegen: **39 fps** (port at zero, guest unchanged).

## Phase 0 - measurement and correctness (in this build)

| switch | what it gives |
| --- | --- |
| `R2_ICOUNT=1` | `[meter]` line: M guest insn/frame, **ns/guest-instruction**, M insn/s. The metric that compares codegen builds across scenes; fps cannot. |
| `R2_STATE_HASH=<frame>[,...]` | `[state] frame N hash ...` - XXH3 over the guest's registers and all 256 MiB of RAM. Two builds that disagree about the game cannot pass it. |
| `R2_INPUT="<vblank>:<BUTTON>[*frames],..."` | already in the tree: scripted presses, so a hardware run repeats the same scene. |
| `R2_PROFILE=<seconds>` + `prof_report.py` | where a frame's time goes, per function and per thread. |

**What phase 0 delivered, and what it did not** (measured, see `PERF_HANDOFF.md` §9.46):

* `R2_ICOUNT` works and is the codegen metric it was meant to be (a metering build reports M guest insn/frame
  and ns/insn; the counter is compiled out of shipped builds).
* `R2_STATE_HASH` works mechanically but **a frame-indexed state is not reproducible**: two runs of the same
  NRO printed different hashes at frames 300/600/900, and different draw counts at the same frame. The guest's
  timeline - wall-clock interrupts, audout/fs IPC, the present cadence - differs run to run, so no hash taken
  at a frame index can compare two builds. As built it is a boot sanity probe, not a gate.

**Phase 0b is therefore the real correctness gate, and it is a prerequisite for Phase 3:**

* **0b-i deterministic whole-game mode - tried, dead end** (`PERF_HANDOFF.md` §9.47): freezing the guest's
  timebase between presented frames deadlocks the boot at 4.18 s, because the guest waits on a DSP interrupt
  that only a moving timebase raises, and nothing presents while it waits. A working version would have to
  drive the clock from guest execution, which changes audio/DMA pacing to a synthetic rate purely for a test
  harness. The code stays in the tree, inert; the switch logs that it is disabled.
* **0b-ii the oracle test - this is the gate.** At a guest function entry: copy the 256 MiB arena aside, run the
  same function through the interpreter (`interpret(Ctx&, addr)`, correct by construction), hash the resulting
  `Ctx` and arena, restore both, call the recompiled function via `g_funcTable`, hash again, compare. No
  timing, no input, no scheduling - one call, two engines, identical starting bytes. It names the function that
  diverges and gates every emitter change, including the LLVM backend. Needs, before it can be built: a step
  limit in `interpret()` so a non-returning function cannot hang the test, and an entry probe in the emitter
  (same shape as `icountAdd`, under its own build flag).

Rules from here on: every performance claim names a build and a scene and reports fps **and** ns/guest-insn;
no codegen change is allowed near hardware until 0b gives it a determinism-backed equality check. Reference for
correctness: `debug/RL-NX-1224.elf` (A-baseline), the last known-good.

## Phase 0b - trimming the PC-port leftovers

Already out of the Switch build: desktop `main`, ImGui menu, touch controls, achievements, updater,
`dolphin_import`, `mods`, `settings`, `savestate`, SDL audio (`RL_RUNTIME_PARKED` in `switch/CMakeLists.txt`).

Decisions, each verified by link + boot + state hash, in this order:

1. **`http.cpp` + libcurl** - referenced only by achievements and the updater, both parked. If the Switch
   link agrees, drop them: a TLS stack and a thread pool that never runs.
2. **`tex_dump.cpp`** - debug PNG dumps; keep the code, compile it out unless `R2_TEX_DUMP_BUILD=1`.
3. **Aurora backends** - only the deko3d one belongs in a Switch build; audit what `RL_NX_DEKO` already
   excludes (OpenGL/Cosmo3D desktop paths) and exclude the rest.
4. **Interpreted fallback** - `interp.cpp` stays: the recompiler needs it for functions it cannot translate.
5. **The 31 MB guest `.text`** - not deletable; the lever is layout (hot-page ordering), which is Phase 3.

Explicit expectation: **deletion buys boot time, load time and build time, not fps.** Measure boot each time;
do not claim frame-rate gains from removing dead code.

## Phase 1 - the guest thread carries guest work and nothing else

The port's 17.4 ms sits *between* guest instructions, where it serialises with them. Two cores are idle.

* **1a. Display-list translate cache** on the FIFO/render thread: translate a guest list once, keyed by its
  address plus a validity token, and patch uniforms on replay. Takes `execute`/`cmdLength`/`cpRun` (2.3 ms)
  and most of `onCommand`'s parsing out of the guest thread.
* **1b. Texture dirty tracking** by guest write ranges instead of hashing ~93 textures a frame (2.35 ms).
* **1c. FIFO copy** (0.75 ms + part of `onCommand`): the guest fills its own linear buffer; the render thread
  drains it.
* **Audio stays on the guest thread** (~3.4 ms) by decision: it is the one slice where moving work could
  change what the game hears, and it is not needed to reach the next milestone.

Acceptance: guest thread ≈ 30 ms / 33 fps, `[perf]` presenting unchanged, state hash equal, audio unchanged
(0 underruns, dropped count not worse).

## Phase 2 - a Switch-native GX → deko3d per-draw path

Keep Aurora for what it is good at (device, memory, present) and stop using its general, WebGPU-shaped
emulation inside the per-draw path:

* cached translated display lists (Phase 1a) become cached deko3d command buffers;
* one pipeline per TEV configuration, specialised at load from a precompiled shader cache;
* no validation, dispatch or lookup per draw - the guest's state space is known at build time.

Acceptance: port side ≤ 6 ms; with Phase 1 the frame lands ~24-26 ms / **~39 fps**, which is the ceiling for
today's guest code.

## Phase 3 - the LLVM backend

Decision: **stop hand-patching the C++ emitter; emit LLVM IR per guest function.**

* guest GPRs as SSA values, `phi` at the PowerPC's merge points, `c.r[]` written only at calls and exception
  points (the exception story of §9.45 - `safepoint`/`rfi` are not pure - becomes explicit in the IR);
* one reserved host register holding the guest RAM base, so a load is `ldr [x28, xN]` rather than a GOT-free
  but still computed arena test (9.37 already measured the memory floor);
* carry and CR bits as plain `i32`s, so `adde` chains compile to adds with carry;
* `-flto` across the guest archive; the game link moves to `lld` (GCC's `ld` cannot take bitcode) while NRO
  packaging stays as it is;
* the indirect branch table stays as it is.

Prerequisite: the state hash (Phase 0) must agree with the reference before any performance number is taken.
Milestones: parity, then ≤17 host insns/guest insn (−20 %, guest 20.5 ms), then ≤12 (−40 %, 15.4 ms).
Acceptance per step: ns/guest-instruction on one scene plus hash parity.

## Phase 4 - what 60 fps actually requires

60 fps is 16.7 ms: guest ≤ 13.5 ms **and** port ≤ 3 ms. Port 3 is Phase 2 done well. Guest 13.5 is 1.9x off
today's 25.6, i.e. ~11 host instructions per guest instruction - a 2x codegen improvement, which is a research
result, not a scheduled one.

The honest ladder: **21 → 25.6 (Phase 1) → 33-35 (Phase 2) → 40-45 (Phase 3 at −20-30 %) → 60 only if the
backend reaches −47 %.** Until 40 is in hand, a locked 30 is the target to hold: consistent frame pacing at 30
beats a swinging 21-27.
