# RL-NX performance audit, 2026-11-27

Read-only review. Nothing in this document is implemented; no file outside `docs/` and
`build-switch/tmp/` was written.

**Build audited:** `17:09` (`RL-NX (Rogue Leader) 0.1, build Nov 26 2026 17:09:15`,
`debug/RL-NX-1709.elf`, gen `build-switch/tmp/gen-1709`), drop `E:/RL-NXLOGS/LOGS 11262026-1753`
(641 s, 21,452 game frames), level 22 window = 424 s / 11,862 frames.

**Tools used:** `switch/tools/perf_report.py`, `prof_report.py`, `code_audit.py` (rerun for this
audit: top 100, out `build-switch/tmp/audit1709.txt`), `aarch64-none-elf-objdump` on the ELF, and
the generated C++ in `build-switch/tmp/gen-1709` (read only, never edited).

---

## 0. The number this audit disagrees with, first

`PERF_TLDR.md` says "Build 17:09: **35.2 fps** on level 22 ... main thread 32 ms/frame". The log of
that build says, for the level-22 window (424 s, 11,862 frames):

```
== level 22: 424 s, 11862 game frames, 28.0 fps
   per game frame (ms):  game 37.07 + present 0.02 = 37.10
     gxsync 5.35   dsp 1.89   idle 1.16   audio 0.87   untimed 27.77
   reporter cost 1216.6 us/frame
```

Whole run (level 1 + level 22): **33.5 fps**, and `frame times: <=16.7 ms 5170, <=33.3 10241,
<=50 3215, >50 2826`. So 13 % of all frames and **22.5 % of level-22 frames are over 50 ms**.
Whatever produced 35.2, it was not this drop's level-22 average; the honest reference for the next
A/B is **28.0 fps / 37.1 ms in level 22, 44.2 fps / 23.5 ms in level 1**, with the frame-time
histogram, not a single fps figure.

Also, `37.07 ms` of game thread versus the profiler's `32.72 ms` game section: **the 4.3 ms
difference is the harness running between the profiler's marks** (see §2.1). The frame the player
feels is the 37 ms one.

### How the ms figures in this document were derived

The profiler reports 654,722 main-thread samples over 13,756 frames = **47.6 samples/frame**, but
its own header says the game section is 32.72 ms/frame. Those cannot both be one thread at 1 kHz:
either role 0 covers more than the guest thread or the sampler over-counts by ~1.45×. **The ratios
are usable; the absolute `ms/frame` in `prof_report.py`'s self-time table are upper bounds.** So
every estimate below is computed as

> (share of role-0 samples) × (32.72 ms game section)

which is the conservative reading, and cross-checked against the audit's own bucket percentages
(the audit's `top 100` is 64.1 % of gen samples, gen is 48.4 % of role-0 samples ⇒ gen ≈ 15.8 ms).
Net cost of a change is quoted against the real 37 ms frame where the change is outside the game
section.

---

## 1. Top 5, by expected gain per unit of effort and risk

| # | Change | Est. saving (level 22) | Effort | Risk | Switch |
|---|---|---|---|---|---|
| 1 | **Stop measuring with the harness on** — VM CENSUS is `1.18 ms` of game thread, PERF LOG's reporter is `1.22 ms` of wall clock per frame (4.35 ms of the 37.1 ms frame is outside the profiled section). Both rows already exist. | **−2.4 ms** (6.5 %) | 0 (menu) / 2 h for the reporter rewrite | none | `VM CENSUS` row, `PERF LOG` row (exist) |
| 2 | **GP RAW back on + CP LAG verified** — the measured run had `R2_GP_RAW=0` saved, so `walkBuf`+`cmdLength`+`cpRun` (1.84 ms) ran on the game thread for nothing | **−1.4 ms** (3.8 %) | 0 (menu row) | low (already the default) | `GP RAW` row (exists) |
| 3 | **Split the bridge drain (gxsync 5.35 ms) so a frame end does not wait for the whole decode**; on explosion frames this one timer is 11–44 ms | **−2 to −3 ms** avg, **−8 to −25 ms** on >100 ms frames | 2–3 days | medium | new `R2_GX_DRAIN` (rows: `GX DRAIN`) |
| 4 | **FPSCR out of the compare path** — `fcmpvF` is 7.0 % of all gen samples (0.71 ms) and writes `c.fpscr` on every compare | **−0.7 to −1.0 ms** | 2 days | low (oracle) | `R2RECOMP_FPSCR=0` + new row `FPSCR LOCAL` |
| 5 | **Register-passing direct calls** (guest arg regs as host parameters), on top of the summaries that already exist | **−1.5 to −2.0 ms** | 1 week | medium-high (oracle) | `R2RECOMP_REGCALL=0` + row `REG CALLS` |

Sum of 1–5: **≈ −8 to −9.5 ms**, i.e. level 22 from 37.1 ms to ~28 ms (**~36 fps**) and level 1
from 23.5 to ~15 ms. That is the honest ceiling of "port + codegen tuning" as it stands: **60 fps
needs the guest's own 15.8 ms below 8 ms**, which none of these do. §4 lists the two designs that
could (fastmem over the VM region, or an LLVM back end), and what they need first.

---

## 2. Where the frame actually goes (measured, build 17:09)

### 2.1 The 32.72 ms game section

| what | samples | share | ms of 32.72 |
|---|---|---|---|
| recompiled guest code (`rt::f_*`) | 317,160 | 48.4 % | **15.84** |
| `svcWaitProcessWideKeyAtomic` (the drain) | 71,093 | 10.9 % | 3.55 (timer says 5.35 — see §8) |
| `musyxDspMail` | 22,070 | 3.4 % | 1.10 (timer says 1.89) |
| `svcSendSyncRequest` (67.6 % `fsFileWrite`) | 17,857 | 2.7 % | 0.89 |
| `rdSlow` + `wrSlow` + `psq*Qv` | 35,592 | 5.4 % | 1.78 |
| `censusHit` (VM CENSUS on) | 16,251 | 2.5 % | **0.81** |
| GX walk: `cmdLength`+`walkBuf`+`cpRun`+`burst`+`gxGatherBurst` | 25,342 | 3.9 % | 1.27 |
| `memmove`, `dispatch`, `hle_PSMTXConcat`, long tail | ~35,000 | 5.3 % | ~1.75 |

### 2.2 Inside the guest code (code_audit, top 100 = 64.1 % of gen)

| bucket | % of gen | ms of 15.84 |
|---|---|---|
| guest memory access (test + load/store + swap) | **40.8 %** | 6.46 |
| host register spill/reload (`sp`/`x29`) | **17.5 %** | 2.77 |
| ABI boundary (ctx load/store at entry, exit, calls) | 15.5 % | 2.46 |
| `fcmpvF` (float compare + FPSCR store) | **7.0 %** | 1.11 |
| guest branches | 6.5 % | 1.03 |
| guest ALU/FP work | 2.8 % | 0.44 |
| CR flag compute (`crS`/`crPack`) | 1.5 % | 0.24 |
| `cntlzw` 1.4 %, `srawi` 1.2 %, `fabs_` 1.1 %, pipe forms 2.3 % | 6.0 % | 0.95 |

Static shape of the emitted code, measured on the ELF: **11.25 host instructions per guest
instruction** for the top 100 (284,490 / 25,295), ranging 2.8 (`f_80031CFC`) to 21.5
(`f_80057970`, `f_80055F60`). The hottest function `f_8002759C` is 18,152 bytes (`0x46E8`) for 323
guest instructions = **56 bytes and 14.0 host instructions per guest instruction**.

Samples sit on loads: `ldr` 31.1 %, `ldur` 7.3 %, `ldrb` 3.7 %, `ldrh` 2.4 % of host instructions
sampled. This is a **memory-latency and address-check** problem, not an instruction-count problem.

---

## 3. Candidates in detail

### C1. The harness is inside the frame (2.4 ms, free)

**What.** Two diagnostics are on by default in the saved `menu.cfg` of the reference run
(`R2_VM_CENSUS=1`, `R2_PERFLOG=1`, `R2_PROFILER=1`) and both cost game-thread time:

* **VM census** — `src/runtime/memory_switch.cpp:135 censusHit()`, called from `rdSlow`
  (`:423`, `:429`) and `wrSlow` through `RT_CENSUS`/`RT_CENSUS_ANY` at `:157`, `:163`. It is a
  `[[unlikely]]` branch that is **taken** while the row is on: 16,251 samples = **0.81 ms of the
  game section**, plus it stalls on a 32-probe hash walk with a random-access pattern.
* **Perf reporter** — `switch/src/perfmon_switch.cpp:rlPmFrame()` copies the whole
  `RlPmCounters` and calls `rlnx_dk_totals()` (12 `svcGetInfo` syscalls for thread ticks) **every
  game frame**, and writes 5 formatted lines per second plus up to 6 `[hitch]` lines.
  `perf_report.py` derives **1,216.6 µs/frame** for level 22 from `pm_us`; the worst second in the
  log is `pm_us=4090.1` (4.1 ms per frame). This is *not* in the 32.72 ms game section (the
  profiler's marks bracket it out) — it is 4.35 ms of the 37.07 ms wall frame.
* **Profiler** — the reports are written by a writer thread (`switch/src/prof_switch.cpp:338`),
  but the 30 s table copy runs on the game thread; `fsFileWrite` is 67.6 % of the 0.89 ms
  `svcSendSyncRequest` in this run.

**Why it matters beyond 2.4 ms.** Every number in `PERF_TLDR.md` and `PERF_HANDOFF.md` 9.33–9.58
was taken with the census on (the census is what generated `vmsites.txt`, and it was left on). The
"17.9 ms of game code" is measured under it. Turning it off before the 20:04 comparison changes the
baseline by ~1 ms.

**Where.** No code change needed for the census (row exists, `menu_switch.cpp:70 kVmCensus`,
`susRows` `:92`, `apply` `:130`). For the reporter: `perfmon_switch.cpp:rlPmFrame` — replace the
per-frame `rlnx_dk_totals()` + `pc_log_line` formatting with (a) accumulate into a small array and
format once per second, and (b) read thread ticks once per second instead of once per frame.

**Estimate.** Census 0.81 ms (samples × 32.72/654,722) + reporter ~1.1 of its 1.22 ms (the
formatting and the 12 `svcGetInfo` calls, not the log write) = **−1.9 to −2.4 ms**, of which 0.8 ms
is in the game section and 1.1–1.6 ms is wall clock.

**Risk.** None to correctness; the census is inert when off, and `vmCensusSet(false)` already runs
on the row. The reporter must keep `[pm]`/`[hitch]` key=value format or `perf_report.py` breaks —
`grep` a re-run before trusting it.

**Switch.** `VM CENSUS` and `PERF LOG` rows (both exist). For the reporter rewrite, keep `PERF LOG`
as the switch and verify one run of `perf_report.py` against the old log.

---

### C2. The measured run had GP RAW saved off (1.4–1.8 ms, free)

**What.** `log.txt:5` records
`menu.cfg ... R2_GP_RAW=0 ... R2_VM_CENSUS=1 ...` and the profile confirms it: `walkBuf` 0.48 ms,
`cmdLength` 0.59 ms, `cpRun` 0.33 ms, `burst` 0.20 ms, `gxGatherBurst` 0.24 ms — all on the game
thread (`switch/src/../src/runtime/hw/gx_fifo.cpp:432 gpWalk`, `:439 cpRun`, `:115 cmdLength`).
With `R2_GP_RAW=1` the CP bytes go to the worker and the walker disappears
(`gx_fifo.cpp:470-476`), which is exactly what `CP LAG` was built to make affordable: with
`s_cpLag` on (`gx_fifo.cpp:196 cpLagActive`), a CP register read reports the worker's progress
instead of draining it (`gx_fifo.cpp:793-800 gxCpRead`), so the walker's removal is not paid for
with `gxsync`.

**Evidence.** 1.27 ms of walker samples + 0.24 ms `gxGatherBurst` = **1.5 ms**, less the part of
`gxGatherBurst`/burst that stays (the pipe still bursts on the worker).

**Estimate.** **−1.4 ms** (3.8 % of the frame), plus it removes the "CP LAG on, GP RAW off" state
that nobody has measured.

**Risk.** Low — GP RAW is the default and 9.57 shipped it after the "unknown FIFO opcode E3" fix
(display lists are copied at the call, `gpDlSink`). The one thing to watch is flicker in
explosions/particles (late arrays/textures), which `GP THREAD OFF` isolates.

**Switch.** `GP RAW` row exists. This is a menu action, not a code change: **the next A/B should be
run with GP RAW on and VM CENSUS off**, or it is measuring a configuration nobody intends to ship.

---

### C3. The bridge drain: 5.35 ms average, 44 ms in explosions (2–3 ms, and the whole explosion story)

**What.** `RLPM_GXSYNC` (`src/runtime/perfmon.h:26`) is added in exactly two places in
`bridgeDrain()` (`gx_bridge.cpp:396`) and in the ring-full path of `ringPush()`
(`gx_bridge.cpp:412`, `waitConsumed` at `:381`). Measured: **1.77 calls per frame, 12.3 ms per
call, 5.35 ms/frame** in level 22. It is the largest single non-guest item in the frame.

**Who waits on whom** (`gx_bridge.cpp`, `hw/gx_fifo.cpp`, `gx_fifo.cpp` timeline):

```
guest thread ──ringPush──▶ ring (8 MiB) ──▶ bridge worker (onCommand → Aurora FIFO writes)
     │                                          │
     │ gpSync(): bridgeDrain()  ◀── waits ──────┘
     │           then fifo::drain() ──▶ Aurora FIFO thread (19.4 of 32.4 ms waiting)
     │                                          │
render worker ── record 6.53 ms/present ──▶ GPU (fence wait 0.01 ms: the GPU is never late)
```

Two facts pin it down:

* the **GPU is not the limit**: `fence=0.01 ms`, `acquire 0.24`, `record 6.53`, render thread 29 %
  busy. There is no queue back-pressure from the GPU at all.
* **it scales with draws**, not with guest work. Level-22 `[hitch]` lines, binned:
  `40-60 ms: 457 frames, 7,209 draws, gxsync 11.1 ms` / `60-100 ms: 692 frames, 8,501 draws,
  gxsync 20.3 ms` / `>100 ms: 87 frames, 9,679 draws, gxsync 44.1 ms`. Explosions are 10.8k draws
  against 3.6k on an average frame (`PERF_HANDOFF` 9.56 agrees), and **the drain grows
  super-linearly** with them.

**Where it would go.** `gx_bridge.cpp:684 gpSync()` — split into "wait for the worker to reach
position P" (already possible: `gx_fifo.cpp:61 gxSetGpProgress`, `ringReadPos()` at
`gx_bridge.cpp:407`) and "wait for everything". Only the PE/CP observations that can invalidate
guest memory need the second. The ring-full wait (`:412`) must stay; if that one is the caller,
the fix is a bigger ring or smaller per-frame pushes, not a semantic change.

**Before any of that: name the caller.** `RLPM_GXSYNC` has one counter for three call sites
(`bridgeDrain` from `gpSync`, `bridgeDrain` from `ringPush` full, `bridgeDrain` from
`bridgeApplyWanted`). Splitting it into `RLPM_GXSYNC_DRAIN` / `RLPM_GXSYNC_FULL` is ~10 lines and
one `[pm]` field, and it decides between "defer the drain" (2–3 ms) and "the worker cannot keep up
with 10k draws" (a different fix, C8).

**Estimate.** **−2 to −3 ms** average if the drain is the frame-boundary kind (the worker is 30 %
busy, so overlapping one more frame of decode is free); **−8 to −25 ms on the 87 frames over
100 ms**, which is where "explosions are 15–30 fps" comes from.

**Risk.** Medium. The invariant is that the game must not see the read pointer past a list the
worker has not run (`gx_fifo.cpp:186-200`, CP LAG). Deferring a *frame boundary* drain keeps that
invariant because CP reads and PE events still order themselves. What breaks if you get it wrong:
reused vertex arrays read late → flicker, or a hung frame if the game spins on a token that never
arrives. **The oracle cannot catch this** (it mutes devices); the Ryujinx smoke can only show "no
fatal". What catches it is a hardware run looking at flicker in explosions and the `[pm]`
`gxsync_calls` count, plus the draw-count series against the baseline.

**Switch.** New `R2_GX_DRAIN` + `-` menu row `GX DRAIN` (defer / every frame), applied at a vblank
like `bridgeApplyWanted` (`gx_bridge.cpp:640`).

---

### C4. FPSCR is written on every float compare (0.7–1.0 ms, low risk)

**What.** `ctx.h:797 fcmpvF` (and `:791 fcmpv`) do `c.fpscr = (c.fpscr & ~0xF000u) | (v << 12);` —
a load, mask, or, store **through `Ctx&`** on every `fcmpo`/`fcmpu` in a float-typed function. The
NaN test is a branch chain (`a != a || b != b ? 1 : a < b ? 8 : ...`). Measured: **14,267 samples
in the audit's `fcmpvF` bucket = 7.0 % of gen = 1.11 ms**, plus `cmp`/`fcmp`/`csel` at 6.3 % of
sampled host instructions.

**Where.** `src/recompiler/emit.cpp:200 cmpFn()` chooses `fcmpvF`/`fcmpv`; the call is emitted per
compare and is on the `kSkip` list of `scanCalls` (`emit.cpp:412`), so it is treated as pure — which
is already wrong for `fpscr` and is the reason the store is there. Change: `fpscr` becomes a local
slot like `cr0`/`ctr` in the locals model (`emit.cpp:96 Slot`), written back to `Ctx` only at
(a) a call to a function whose summary writes `fpscr`, (b) `mffs`/`mtfsf`/`mcrfs`, (c) return.
The compare itself becomes branchless: `fcmp s0,s1; csel` on the CCMP result, or compare the raw
bit patterns as integers (`(a<<1)`, sign-magnitude ordering) to get LT/GT/EQ and set the NaN case
with one `ccmp`.

**Estimate.** 1.11 ms of `fcmpvF` samples; a local + branchless form removes the `Ctx` round trip
(~4 of the ~9 instructions) and one of the two branches. **−0.7 to −1.0 ms**, and it also removes
`fpscr` from the "written" set that forces spills at call sites (part of the 2.77 ms spill bucket).

**Risk.** Low but not zero: any guest code that reads FPSCR mid-expression (the `.movie` decoder
checks FPSCR?) would see a stale value. Gate it with the oracle (§7) and an `R2RECOMP_FPSCR=0`
regen for A/B; also assert it on hardware through `soCheck`-style one-shot logging if a guest
`mcrfs` ever reads a field we deferred.

**Switch.** `R2RECOMP_FPSCR` (emit) plus a runtime row `FPSCR LOCAL` so one NRO can A/B it
(the emitted code tests a global bool once per function, as `g_vmSite` does).

---

### C5. Register-passing direct calls (1.5–2.0 ms, medium-high risk)

**What.** A direct guest call today is `f_XXXX(c)` — the callee reloads every slot from `Ctx` at
entry (30 GPRs + 7 doubles + 5 ps1 + 5 CR + `ctr` in `f_8002759C`), and the caller spills/reloads
around it. The summaries already know the exact read/write sets (`emit.cpp:340 struct Summary`,
`g_summ`, closed over callees since 9.57). The next step is to pass the guest's argument registers
**as host parameters**: `void f_X(Ctx& c, u32 r3, u32 r4, ..., u32 r10, double f1, ..., double f8)`
for a callee whose summary says it reads them, and return `r3/r4/f1/f2` by value. This is the
MKDD-NX/`gen_c.py` model, and `abiMask()` (`emit.cpp:202`) already encodes exactly which slots a
call may read or clobber.

**Evidence.** ABI boundary is 15.5 % of gen (2.46 ms) split as `entry/exit` 8.3 % (16,802 samples =
0.84 ms) and `call site marshal` 6.6 % (13,491 = 0.67 ms); spills are a further 2.77 ms, most of
them the "store every slot the function writes, reload every slot it mentions" rule
(`emit.cpp:77 callStmt`, `:1750-1760`). The disassembly shows the entry block of the hottest
function doing ~40 host instructions (11 `stp` of callee-saved, 4 SIMD `q` loads of GPR pairs,
`g_trace` test, `memBase()` GOT load) before its first guest instruction.

**Estimate.** Removing the marshalling for arguments actually read (13.3 slots average) plus the
register files it forces: **−1.5 to −2.0 ms** (call-site 0.67 + entry/exit ~0.84 + ~0.5 of spills).
Optimistic ceiling if the summaries shrink to 4–8 live slots: −2.5 ms.

**Risk.** Medium-high. The ABI2 work (9.56) already relies on summaries being exact; a wrong read
set stops passing an argument that the callee needs, and the failure is *silent* (a stale
`Ctx.r[5]`), not a crash. This is exactly what the oracle catches, function by function: run
`RL_NX_ORACLE=1` with `R2_ORACLE=300 R2_ORACLE_EVERY=41` and require 300/300 as for v10/v12, then
`R2_ORACLE_FAULT=1` must still fail. A Ryujinx smoke additionally catches memory corruption
(illegal instruction at 0x0, missing draws).

**Switch.** `R2RECOMP_REGCALL=0` regenerates the Ctx-only form; runtime row `REG CALLS`
(a global bool the entry block tests, so one NRO can A/B).

---

### C6. The VM inline path is function-granular, and the live gen has 5× more of it than the build that was measured

**What.** `config/GSWE64/vmsites.txt` has **969 site lines and 510 `F <entry>` lines**. An `F`
entry means *every* access in that function takes the V form
(`emit.cpp:1664 g_vmSite = g_vmInline && (cfg_.vmSites.count(pc) || cfg_.vmFns.count(f.entry))`,
`withBase` at `:287-310` appends `V`).

**Static counts (measured, `grep` over the gen trees):**

| gen | V-form accessors | stack (S) forms |
|---|---|---|
| `build-switch/tmp/gen-1709` (**what was measured**) | 4,951 | 58,916 |
| `build-switch/tmp/gen-1802` (hardware census regen) | **28,018** | 58,916 |
| `build-switch/gen` (live, 19:54 = `gen-v11b`) | **24,120** | 82,300 |

So the hardware census multiplied the V-form sites by **4.9×**, and the unmeasured 20:04 build
carries 24,120 of them. What a V form costs is visible in the ELF: the emitted `stwu r1` in
`f_8002759C` (`gen-1709/code_084.cpp:4721`, `wr32V`) is

```
sub w11,w15,#0xc0 ; cmp w11,w12 ; b.le … ; lsr w8,w8,#12 ; ldr x8,[x13,w8,uxtw#3]
cbz x8, slow     ; and x9,x11,#0xfff ; cmp w9,#0xffc ; b.hi slow ; rev w10,w15 ; str w10,[x8,x9]
```
≈ **11 instructions for the VM hit**, and when it misses the arena fallback (5 more) is also
emitted inline. A plain arena access (`rd32`) is ~5. So a V form costs **+6 host instructions on
every access in those 510 functions**, including stack saves, constant-address MEM1 loads
(`rd32V(M, 0x8009BFD4)`, `code_084.cpp:4732`) and the frame pointer update — all of which can
never be VM addresses.

**New evidence, not a repeat of 9.53.** 9.53 rejected *inlining the VM tier into every access*
(+78 % `.text`). This is different: it says the *selection* is too wide, that the function-level
fallback is what made it wide, and that the census's own per-site data is the better key. The
census already records per-site counts, so `F` lines could be dropped entirely (the tool's own
header calls them "the census address had no gen line to name the instruction").

**Estimate.** With 24,120 static sites at +6 instructions: if those sites execute at the same
average rate as the rest of the form mix, the added work is on the order of 1–2 % of guest
instructions **per access that used to be one test**; the measured ceiling is the `arenaPtrM`
(11,260 samples = 0.56 ms) + `inArenaN` (5,931 = 0.30 ms) + part of the int-alu/compare "check"
share (34.9 % of the memory bucket). **−0.4 to −0.9 ms** by restricting to sites only, **and it
stops a 5× regression landing unmeasured**.

**Risk.** Low for correctness — `rd32V` and `rd32` are semantically identical (the V form is a
fast path for the same test). Risk is all in *losing* the VM fast path at a hot site: that is a
speed regression, not a break.

**Switch.** No runtime switch exists today (it is a gen-time decision). To make it A/B-able from
the menu, add one global to `ctx.h`: `if (!g_vmFast) return nullptr;` at the top of `vmHitR`
(`ctx.h:879`) / `vmHitW` (`:887`), one predictable load, and a row `VM FAST`. Then one NRO covers
both. **Also: this is the cheapest experiment in this document** — regen with the `F` lines removed
and compare on the same route.

---

### C7. A fastmem design that survives VM sites it has not seen

**What failed and why** (`AGENTS.md` Stage 11, `switch/src/fastmem_switch.cpp`,
`fastmem_entry.S`): a 4 GiB window, arena at +0x80000000, locked cache at +0xE0000000, and every
other address faulting into `rdSlow`/`wrSlow`. Two blockers: (a) Ryujinx kills the process at the
first fault, so the fault path was never tested; (b) the game's paged module globals live in the
VM region, and *aliasing* the arena into the window is impossible ("code memory makes the source
pages inaccessible"), so every VM access the census missed faulted — 19:53 died at boot.

**The design that fixes both.** The VM region is **32 MiB** (`kVmStart` 0x7E000000, `kVmLen`
0x02000000, 8,192 pages) and `vm.cpp` is its only mapper. Give it its own **fixed 32 MiB host
region `V`** — not an alias of the arena. Map `V` at `W + 0x7E000000` in the fastmem window, page
by page, with the *hardware* MMU. Then:

* every guest address in {arena, locked cache, VM} is `W + a` with **no test at all** — the 6.46 ms
  memory bucket's checks go away, not just the V-form part;
* a page the guest has not paged in is **not mapped**, so touching it takes a real fault and lands
  in `fastmem_entry.S` → `rl_fm_slow` → `vmMiss` → the game's own DSI handler. That is the
  correctness property the alias design lost: an unseen VM site *faults*, it does not read garbage;
* when the game's page table drops a page, `vm.cpp` unmaps it (`svcUnmapMemory` on 4 KiB), so a
  stale read cannot happen either.

Two things it needs that nobody has measured:

1. **The fault rate.** If the game re-pages a working set every frame, a fault per access is
   catastrophic (`svcMapMemory` + exception entry ≈ µs each). The census gives per-*site* counts,
   not per-*page* or per-*miss* counts. Add `vmMiss` and `vmPageIn` counters per frame to
   `memory_switch.cpp`/`vm.cpp` (counters only, like `rl_pm.vmRd`) and read them in one hardware
   run. **This single number decides the whole design.**
2. **32 MiB of extra host memory** on top of the 2.05/2.79 GiB the deko build already uses. It
   should fit; `memInit` (`memory_switch.cpp:452`) must reserve `V` before the arena.

**Estimate.** The addressable cost is the memory bucket's 34.9 % "check/address math" (≈2.25 ms)
plus `rdSlow`/`wrSlow` 1.78 ms plus `vmHitR`'s share — **−2.5 to −3.0 ms** if faults are rare, and
**worse than today** if they are not. That is why (1) comes first.

**Risk.** High, and Ryujinx cannot validate it (no user faults). What can: a hardware run with
`R2_FASTMEM` and the `[fastmem]`/`fmcensus` logging that already exists, plus the oracle for the
non-fault paths (it runs the same accessors with devices muted; with M = window the oracle's
`g_ram` swap must be extended to `V`, which is a real piece of work — *state it as such*).

**Switch.** `R2_FASTMEM` + row `FASTMEM` (the NRO is a separate build today: `RL_NX_FASTMEM=1`).
Since the rule is "switchable from the − menu", the honest form is: fastmem stays a separate NRO
(build flag) and the row only exists to A/B the *test* forms inside one build.

---

### C8. Per-draw cost is where explosions die, and it is CPU, not GPU

**What the logs say** (same drop, `[pm]` + `[hitch]` + role-1 profile):

* **GPU is never the limit**: `fence 0.01 ms`, `acquire 0.24`, `submits 2.53`, `burst_waits 0`.
* **render worker** records 6.53 ms/present at 4,556 draws (1.43 µs/draw) and is 29 % busy.
* **Aurora's FIFO thread** per frame: `draw_prim` 1.20, `prepare_idx_buffer` 1.16, `fill_uniform`
  0.98, `XXH3_hashLong` 0.89, `fifo::process` 0.50, `pos_layout` 0.39 ≈ **5.1 ms busy at 4.5k
  draws (1.13 µs/draw)**, 19.4 of 32.4 ms waiting. At 10.8k draws that is **~12 ms busy** — the
  FIFO thread stops waiting and becomes a co-limit.
* **game thread** per draw: pipe writes (`pipe_wr` 9,032/frame in level 22, 162k in explosions),
  the walker (1.5 ms), and the drain (C3).

So "GPU-side cost in explosions" is the wrong frame: the GPU is idle-waiting, and what explodes is
**per-draw CPU work on three threads plus a synchronization that grows with draws**. The 87 frames
over 100 ms average 9,679 draws and 44.1 ms of `gxsync`.

**Candidates, in order of value:** C3 (drain) first; then the replay cache already in the plan
(`PERF_PLAN` phase 1a: translate a display list once, keyed by address + validity token) which
targets exactly `prepare_idx_buffer` + `fill_uniform` + `draw_prim` — **1.1 µs/draw → ~0.4 µs**,
i.e. −3 ms at 4.5k draws and −7 ms in explosions, on the FIFO thread; and `XXH3` 0.89 ms, which is
`hashBytes` on vertex arrays/textures (`gx_bridge.cpp:973`), not the texture identity cache.

**Risk.** The replay cache's failure mode is missing/stale draws — visible, and caught by comparing
the per-frame draw series from `[pm-gx]` against the baseline (that is what 9.49 used). Not
oracle-able.

**Switch.** New `R2_DL_CACHE` + row `DL CACHE`.

---

### C9. MusyX DSP: 1.1–1.9 ms on the guest thread, and batching is probably a dead end

**What.** `hw/musyx_dsp.cpp:590 musyxDspMail` runs the command list (`runList`, `:348`) and the
mixer (`resample` at `:216`, `s_out[3][kN]`) **on the game thread**, by rule. Measured: 22,070
samples = **1.10 ms of the game section** (timer says 1.89), `dsp_calls 9.3–22.9` per frame =
**~164 µs per mail**.

**Batching, examined.** The DSP block is 160 samples at 32 kHz = 5 ms, so ~3.3 blocks/frame at
60 fps, but the game sends **9–23 mails/frame**. So there *is* something to coalesce — but only if
the extra mails do work that can wait. They are MusyX command-list mails (`(mail>>16)==0xBABE`
then the list, `hw.cpp:970 dspPostMail`), one per studio/aux bus per block, and each list ends by
mixing its own 160 samples. Coalescing them into one pass would mix the same 160 samples once with
all studios instead of three times — worth **~0.4–0.7 ms** — but it changes the order in which
command lists run, which MusyX's own bookkeeping may observe.

**Cheaper and safer:** the mixer itself. `resample()` is scalar `s16` × `s16` polyphase with a
`while` inner loop and `clamp16` per sample; 4 taps × `kSub` samples × voices × 3 studios. NEON
(`smull`/`sqrdmulh` on 8 lanes, 4 taps as `[4]` int16) is a mechanical rewrite that keeps the
guest thread, keeps the sample-exact integer math and is checkable by diffing `s_out` against the
scalar version on the host for a fixed ARAM image.

**Estimate.** **−0.6 to −0.9 ms** from vectorising `resample`+the bus mix, **−0.4 to −0.7 ms** more
if the mails coalesce. Audio is not covered by the oracle: the check is a byte-compare of the
emitted PCM on the host plus a listen on hardware.

**Switch.** `R2_DSP_NEON=0` + row `DSP NEON`.

---

### C10. Entry/exit float conversion (0.3–0.6 ms, very low risk)

**What.** `float`-typed FPRs are stored in `Ctx` as **double** (`ctx.h:635 double ps1[32]`, `f[32]`),
so every entry does `bflt(d2s(c.f[n]))` and every exit `s2d(fbits(...))`
(`emit.cpp:213 slotLoadExpr`, `:218 slotStoreStmt`). `f_8002759C` alone has **38 of them in its
entry block** (15 `c.f[]`, 23 `c.ps1[]`), i.e. ~76 `fcvt` per call.

**Where.** `struct Ctx` (`ctx.h:632`): make the slot union-typed (`double d; float f;`) and have the
emitter store the float bits when the register is float-typed. The field is read raw by
`memSaveState`/oracle hashing, so those paths need the same 32 bits — they already hash bytes.

**Estimate.** 76 `fcvt` per call at 2/cycle is ~40 cycles; over ~10k calls/frame ≈ **0.15–0.3 ms**,
call it 0.3 conservative. Low value alone, but it is a prerequisite for the ps-pair-as-`float32x2`
idea in 9.58 and it shrinks the ABI bucket.

**Risk.** Low: bit-preserving for every non-NaN, and the 9.58 NaN payload difference already exists
in the tree (the known `f2` payload failure). Verify with the oracle only.

**Switch.** `R2RECOMP_FLOATFPR` already exists; no new row needed.

---

### C11. Hitches and frame pacing (0.5 day of measurement, no estimate)

**What the drop says.** 641 s, 21,452 frames, **2,826 frames over 50 ms (13 %)**, and level 22 has
2,675 of 11,862 (22.5 %) over 50 ms, with only 2,784 (23.5 %) at or under 16.7 ms. The worst
frames are level loads: `frame=8848 ms=2585.4`, `frame=9288 ms=1553.3` (with `dsp=81.5`,
`lc=31654`, `psq_q=195238`), then a long tail of 700–1200 ms frames in the menus
(`frame=15001 ms=1191.4 lvl=0`).

**What is worth doing.** For a 50–60 fps goal the *distribution* is the product: a 2585 ms load
frame is 150 dropped frames at 60. Two measurable things:

1. `[pm]` shows `pm_us` up to 4090 µs on hitch-heavy seconds — the log writer's SD writes land on
   the game thread through `pc_log_line` while the frame is late. Capping the hitch lines per
   second (already 6) or writing them from the reporter's own thread removes a self-inflicted
   hitch. Cheap.
2. The load stalls at `lvl` transitions are the `GPU burst` path (`R2_GPU_BURST_WAIT`, 139 burst
   waits in this run). Those are needed for correctness (9.51), so the only lever is upload
   shaping (smaller batches, or letting the worker record while the GPU turns). Not measurable
   from this drop; the `GPU burst:` lines are already there.

**Estimate.** None offered. This is the item where the honest answer is "one hardware run with the
`[pm]` line timestamps and a stopwatch, not a guess".

---

## 4. What would actually reach 60

The frame is 37.1 ms. Candidates C1–C5 total ~8–9.5 ms. Even all of C1–C11 lands around
**21–24 ms (42–48 fps)** in level 22. For 16.7 ms the **15.8 ms of guest code** has to fall below
8 ms — a 2× codegen change, which is C5 (register passing) plus C7 (fastmem) plus accepting
`float32x2` paired-single codegen (9.58's next step: halves the register count of every ps-heavy
function, and `f_80057970`/`f_80055F60` are 21.5 host instructions per guest because of exactly
that). Sequence, best first:

1. C1 + C2 (free, this week, re-establishes the baseline at ~33 fps level 22).
2. C3 after the 10-line counter split (drain), C4 (FPSCR), C6 (V forms) — all oracle-safe.
3. C5 (register passing) — the biggest codegen lever that is not a rewrite.
4. C7 (fastmem over the VM region) — only after the fault-rate counters exist.
5. LLVM backend — only if 3 + 4 stall above ~20 ms.

---

## 5. Where earlier analysis looks wrong

1. **"Build 17:09: 35.2 fps on level 22."** This drop's level-22 window is **28.0 fps / 37.1 ms**
   over 424 s and 11,862 frames. The 35.2 figure is not reproducible from the log it is attributed
   to. Either it came from a different window (level 1 is 44.2) or from an earlier build. The next
   A/B needs the histogram, not a headline.
2. **"Waits 3.9 ms."** `gxsync` alone is **5.35 ms/frame** in level 22 (1.77 calls × 12.3 ms), and
   `idle` 1.16 + `audio` 0.87 on top. `svcWaitProcessWideKeyAtomic` is 10.9 % of all game-thread
   samples — the single largest non-guest item.
3. **"Guest memory 41 %, ABI 19 %, spills 13 %, VM test 6 %, CR 6 %, real work 3 %."** Rerunning
   `code_audit.py` on the same build: memory **40.8 %**, spills **17.5 %** (not 13), ABI **15.5 %**
   (not 19), **`fcmpvF` 7.0 %** (the old "CR 6 %" is really the float compare, and it is a helper
   with a `Ctx` store, not a flag compute), guest branches 6.5 %, real work 2.8 %. "VM TLB test 6 %"
   is absent — in this gen only 4,951 accesses take the V form, so the VM test is not a bucket at
   all yet. The brief's numbers look like a mix of two different builds.
4. **The VM inline path was measured on a build that barely uses it.** 4,951 V-form accesses in the
   measured gen vs **24,120 in the live gen**. `PERF_TLDR`'s plan says "a VM inline path, guided by
   the census, only at sites that hit VM" — but the shipped census is **function-granular for 510
   entries**, so the V test is on every access in those functions, including stack saves and
   constant-address MEM1 loads. Either restrict to `vmsites.txt` site lines (§C6) or expect the
   20:04 build to be slower on the memory path than 17:09.
5. **The reference run had `R2_GP_RAW=0`** (`log.txt:5`), so the 1.5 ms walker is in every
   "current" number and `CP LAG` has never been exercised on hardware in the configuration it was
   designed for (CP LAG requires GP RAW: `cpLagActive()`, `gx_fifo.cpp:196`).
6. **The harness is inside the measurement.** `VM CENSUS` on costs 0.81 ms of game section, and
   `PERF LOG` costs ~1.2 ms of wall clock per frame outside the profiled section. The 32.72 ms
   "game section" and the 37.07 ms real frame are not the same thing, and the TLDR conflates them.
7. **"Explosions are a GPU-side cost."** `fence 0.01 ms` and `acquire 0.24 ms` say the GPU always
   finishes before the next frame; the render worker is 29 % busy. Explosions are per-draw **CPU**
   (game thread pipe+walk+sync, bridge worker, Aurora FIFO thread) and the dominant term is the
   bridge drain (11 → 44 ms as draws go 7.2k → 9.7k).
8. **"6,100–6,800 draws a frame"** (from the 47.35 ms profile). Level 22 in this drop averages
   **4,556 draws / 3,121 merged**, and level 1 **4,049**. Draw counts moved a lot between builds;
   any per-draw arithmetic should be redone from `[pm-gx]` of the build being discussed.
9. **The 13 % of frames over 50 ms are not in any bucket.** Every table above is an *average of
   running frames*; 22.5 % of level-22 frames are longer than 50 ms and they carry the drain cost
   that the average hides (C3, C11).

---

## 6. Switch inventory (what a candidate would sit behind)

Existing `-` menu rows (`switch/src/menu_switch.cpp:70`, keys `:110`, apply `:130`):
`FRAME RATE, STATS LOG, PROFILER, PERF LOG, GPU TIMING, GX THREAD, GP THREAD, AUDIO THREAD,
FAST PIPE, GP RAW, FRAME OVERLAP, NATIVE MTX, VM CENSUS, NATIVE MOVIE, CP LAG, HD TEXTURES,
HD TEX 2048`.

Proposed rows, all runtime booleans so one NRO covers both sides of an A/B:

| row name | `R2_*` env / gen switch | what it toggles |
|---|---|---|
| `GX DRAIN` | `R2_GX_DRAIN` | frame-boundary drain: full / deferred (C3) |
| `FPSCR LOCAL` | `R2_FPSCR_HOT` (+ `R2RECOMP_FPSCR`) | FPSCR in a local vs in `Ctx` (C4) |
| `REG CALLS` | `R2_REGCALL` (+ `R2RECOMP_REGCALL`) | register-passing direct calls (C5) |
| `VM FAST` | `R2_VM_FAST` (+ gen `R2RECOMP_VMSITES`) | V-form fast path on/off at runtime (C6) |
| `DL CACHE` | `R2_DL_CACHE` | display-list translate cache (C8) |
| `DSP NEON` | `R2_DSP_NEON` | vectorised MusyX mixer (C9) |
| `FASTMEM` | `R2_FASTMEM` | only meaningful in the `RL_NX_FASTMEM=1` NRO (C7) |

No new row is needed for C1 (VM CENSUS / PERF LOG exist) or C2 (GP RAW exists).

---

## 7. What I could not measure, and the hardware run that would settle it

1. **The caller of the drain** (`bridgeDrain` from `gpSync` vs from a full ring vs from
   `bridgeApplyWanted`). One counter split in `perfmon.h`/`gx_bridge.cpp` (10 lines) and one
   hardware run decides whether C3 is worth 2 ms or whether the worker simply cannot keep up with
   10k draws.
2. **Whether role 0 is one thread.** 654,722 samples over 13,756 frames is 47.6 ms/frame against a
   32.72 ms game section: either role 0 covers more than the guest thread (SDL's thread registers
   with the profiler?) or the sampler over-counts. One line in the profile header naming the thread
   per role settles it; until then every absolute ms in `prof_report.py` is an upper bound.
3. **VM page-fault and page-in rate.** Decides C7 entirely. Needs `vmMiss`/`vmPageIn` counters
   (counters only, as `rl_pm.vmRd` is) and one in-level run: if page-ins are < ~100/frame, fastmem
   over a fixed 32 MiB VM region is the 2.5–3 ms winning move; if they are thousands, it is dead.
4. **What the 20:04 build (liveness + SDA fast + SO0 + CP LAG) actually did.** Its static effect is
   visible (48k entry loads, 210k stores, 204k reloads removed) but the hardware effect is not: the
   card's `menu.cfg` in this drop still has `VM CENSUS=1` and `GP_RAW=0`, so a run of 20:04 in the
   same configuration would not be comparable. **The run to ask for: 20:04, level 22, VM CENSUS
   off, PERF LOG off for the fps figure and a second pass with PERF LOG on for the `[pm]` detail,
   GP RAW on, same route.** Two runs, because the reporter changes the thing it measures.
5. **Explosion frames specifically.** The 87 frames over 100 ms are worth their own `[hitch]` +
   `[pm]` capture with `GP THREAD` toggled, since that is where 15–30 fps lives and where the
   drain, the walker and the FIFO thread all contend.
6. **Audio correctness of a vectorised mixer** cannot be judged from a log; it needs a listening
   test on hardware plus a host-side byte-compare of the emitted PCM.
7. **Ryujinx could not be used for any of the above beyond "no fatal, no misparse"** — no user page
   faults for fastmem, no timing that means anything, and no way out of the attract loop.

---

## 8. Appendix: the numbers this audit measured itself

* `code_audit.py`, build 17:09, top 100: 203,153 of 317,160 gen samples (64.1 %), **11.25 host
  instructions per guest instruction static** (284,490 / 25,295).
* `nm -S`: `f_8002759C` = 0x46E8 = 18,152 B / 323 guest = 56 B/guest; `f_7FCA15AC` = 0x90E8 /
  698 guest; `f_8001D9EC_orig` = 0x137E0 / 2,680 guest.
* `objdump` of `f_8002759C`'s prologue: 11 `stp` of callee-saved, `sub sp, sp, #0x2e0`, a
  `g_trace` GOT load + `ldrb` + `cbz`, `memBase()` GOT load, 4 SIMD `q` loads for GPR pairs —
  ~40 instructions before the first guest instruction.
* Every `wrSlow` call site spills live SIMD values (`stur q1,[x29,#-128]` … `bl wrSlow` … reload):
  `preserve_most` keeps x9–x15 but not `v0`–`v31`, and Clang keeps GPR pairs in `q` registers. This
  is part of why the slow-path calls are expensive even after 9.53.
* V-form usage per gen: 4,951 (measured) / 28,018 (hardware census regen) / 24,120 (live).
* `vmsites.txt`: 969 site lines, 510 `F` function lines.
* Level-22 `[hitch]` bins vs draws and `gxsync` as in §C3.
* `gxsync` in level 22: 1.77 calls/frame, 12.3 ms/call.
