# RL-NX performance: TL;DR

Updated 2026-11-27, at build 20:04. This is the short version. For full detail:
- `PERF_HANDOFF.md` §9.33-9.58 has the measurements up to 2026-11-26 morning.
- Later stages live only in memory/AGENTS, so this page is the only written summary of them.

**Goal:** 50-60 fps in levels, handheld 720p, at stock clocks. The GPU uses the retail handheld profile; no CPU overclock.

## Where we stand

| | |
|---|---|
| Last measured | **Build 17:09: 35.2 fps on level 22 at stock clocks.** The game thread is 84-86 % busy and is the only limit. |
| Latest build | **20:04**, hardware **unmeasured**. It adds the audit levers: liveness, SDA fast, CP LAG and SO0. |
| Level-22 main thread, 32 ms/frame | Recompiled game code 17.9 ms; CP/GX walk 4.1; waits 3.9; slow memory 2.5; MusyX DSP mail 1.9. |
| What caps us | Game code alone (17.9 ms) is more than the 16.7 ms budget for 60 fps. Runtime tuning can't reach 60; only codegen can. |
| Worst case | Explosions, 15-30 fps. |
| Open bugs | None blocking perf. Crash 8.11, Tatooine Training, was fixed in 19:08 by GPU burst waits. |

## fps history (level 22 unless noted)

| Build | fps | Note |
|---|---|---|
| 09:25 (11-23) | 14-24 | First profile. The game thread was 100 % busy. |
| Baseline (11-25) | 21 | 47 ms frame: 25.6 ms guest code, 17.4 ms port. |
| 19:58 | 30.7 | After the texture-hash cache, -O3, the accessor rework and the thread fixes. |
| 20:48 | 36.9 (3 levels) | **Overclocked**. The user hadn't noticed the OC tool was on. |
| 12:08-12:46 Stages 1-3 | "big gain" | Also overclocked, so there's no clean number. |
| 13:54 | 33.7 (32.0 with FAST PIPE off) | First stock-clock run. |
| 14:45 | ~same | GP RAW was neutral. |
| **17:09** | **35.2** (34.0 with FRAME OVERLAP off) | **Current reference.** |
| 20:04 | ? | Waiting on the user's run. |

## What worked (kept; each has a − menu row or a build flag)

**Port side:**
- **Texture hash cache** (9.34): one full hash a frame instead of 54, about −1.9 ms.
- **Guest accessor rework** (9.37): host code per guest instruction went from 35.7x to 20.6x.
- **Guest code at -O3** (9.36).
- **GX bridge on a worker thread** (GX THREAD).
- **Batched GX ring publish.**
- **In-place CP parse.**
- **GP thread** (GP THREAD): the CP stream runs on a worker.
- **Audio-out thread** (AUDIO THREAD). The DSP itself stays on the guest thread, by rule.
- **cmdLength vertex-size cache** (Stage 5): it had been 2.35 % of the game thread.
- **FAST PIPE** (Stage 4): stores to the gather pipe skip the slow path. 33.7 vs 32.0 fps.
- **FRAME OVERLAP** (Stage 7): Aurora's fifo is double-buffered. About −1.1 ms a frame.
- **NATIVE MTX** (Stage 9): native PSMTX code. About −0.4 ms a frame.
- **NATIVE MOVIE** (Stage 10): libjpeg-turbo decodes the `.movie` frames. That decoder made intro and menus slow (22 ms). Matches the game's decoder within ±1 on 99 % of pixels.
- **GPU burst waits**: these fixed the Tatooine kickoff crash; they aren't a speed change.

**Recompiler (r2recomp v5 → v12), each checked by the oracle:**
- **Hardware fcvt** for float↔double conversion, and **ABI-only register write-back** at returns (Stage 1).
- **Paired-single and float ops in float** (Stage 2). Small gain.
- **Unchecked stack (r1) accesses** plus a single-window arena test (Stage 3). Guest `.text` went from 31.9 to 26.4 MB (−17 %); PSMTXConcat −39 %.
- **Locals emitter v2**: guest registers kept as host locals. The first try (9.45) failed; v2 passed.
- **Call-site summaries** (v3) and **ABI2** (v10). Functions going through opaque dispatch dropped from 4,582 to 2,585.
- **Liveness/dirty pass** (v12): 48k entry loads, 210k stores and 204k reloads removed. Hardware unmeasured.
- **SDA fast path**: 23k r2/r13 small-data sites unchecked. **SO forced to 0**, so dead CR stores go. Both hardware unmeasured.

## What didn't work (don't retry)

- **Hot-call clustering and `flatten`** (9.39-9.43): 0 to −3 % on hardware.
- **Frame-indexed state hash as a correctness gate** (9.46): runs aren't deterministic. The **oracle** test replaced it as the gate.
- **Frozen guest timebase / deterministic mode** (9.47): deadlocks at boot.
- **GP RAW alone** (Stage 6): neutral. It saved 2.5-4 ms of walking but added 3 ms of gxsync drain. CP LAG in 20:04 aims at that drain.
- **FASTMEM** (flat VM window plus fault trampoline): built but not shipped. Ryujinx dies at the first fault, and paged game-module globals can't be aliased.
- **Ruled out by the user's rules:**
  - CPU BOOST: removed, since there's no CPU clock past retail.
  - Lower resolution.
  - `-ffast-math` and strict aliasing.
  - Moving the DSP off the guest thread.
  - Timing anything in Ryujinx.

## Where the game-code time goes (code audit of 17:09)

| Share | Bucket |
|---|---|
| 41 % | Guest memory access |
| 19 % | ABI boundary |
| 13 % | Spills |
| 6 % | VM test |
| 6 % | CR |
| 3 % | Real work |

Build 20:04's liveness and SO0 passes target the ABI, spill and CR buckets.

## Next

1. **User:** run 20:04 on level 22 with `config/menu.cfg` (all perf rows on, GP RAW on) and compare with 35.2 fps. Optionally A/B CP LAG.
2. If guest code is still above about 14 ms, the remaining big levers are:
   - **register-passing calls** built from the summaries;
   - a **VM inline path**, guided by the census, only at sites that hit VM;
   - **guest memory access**, still the largest bucket (41 %). It needs a fastmem that survives unseen VM sites.
3. Smaller levers:
   - batch the MusyX DSP mail (1.9 ms);
   - find the 3.9 ms of main-thread waits.
